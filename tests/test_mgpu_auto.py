import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import mgpu_auto


class DiscoveryTests(unittest.TestCase):
    def test_vdf_value_unescapes_paths(self):
        text = r'"installdir" "Example\\Game"'
        self.assertEqual(mgpu_auto.read_vdf_value(text, "installdir"), "Example\\Game")

    def test_discovers_games_and_executable_from_library_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            library = root / "Library"
            manifest_dir = library / "steamapps"
            install = manifest_dir / "common" / "Example Game"
            install.mkdir(parents=True)
            (install / "ExampleGame.exe").write_bytes(b"MZ")
            manifest_dir.mkdir(exist_ok=True)
            (manifest_dir / "appmanifest_123.acf").write_text(
                '"AppState" {\n'
                '  "appid" "123"\n'
                '  "name" "Example Game"\n'
                '  "installdir" "Example Game"\n'
                '}\n', encoding="utf-8")
            steam_root = root / "Steam"
            (steam_root / "steamapps").mkdir(parents=True)
            (steam_root / "steamapps/libraryfolders.vdf").write_text(
                '"libraryfolders" {\n'
                f'  "1" {{ "path" "{library}" }}\n'
                '}\n', encoding="utf-8")

            with mock.patch.object(mgpu_auto, "steam_roots", return_value=[steam_root]):
                games = mgpu_auto.discover_games()

            self.assertEqual(len(games), 1)
            self.assertEqual(games[0].appid, "123")
            self.assertEqual(games[0].name, "Example Game")
            self.assertEqual(games[0].executables, [str(install / "ExampleGame.exe")])


class PlanningTests(unittest.TestCase):
    def setUp(self):
        self.gpus = [
            mgpu_auto.Gpu(0, "RTX 3090", "595", "0000:01:00.0", "Off", 5, 1000, 24576),
            mgpu_auto.Gpu(1, "RTX 3090", "595", "0000:03:00.0", "On", 20, 2000, 24576),
        ]
        self.ok_p2p = {"available": True}
        self.ok_interop = {"available": True}

    def test_selects_display_gpu_for_neural_and_other_for_render(self):
        plan = mgpu_auto.select_plan(self.gpus, self.ok_p2p, self.ok_interop,
                                     {"available": True, "transport_available": True})
        self.assertEqual(plan["status"], "READY_REMOTE")
        self.assertEqual(plan["render_gpu"], 0)
        self.assertEqual(plan["neural_gpu"], 1)

    def test_low_memory_keeps_local_fallback(self):
        gpus = list(self.gpus)
        gpus[1] = mgpu_auto.Gpu(1, "RTX 3090", "595", "0000:03:00.0", "On", 20,
                                22000, 24576)
        plan = mgpu_auto.select_plan(gpus, self.ok_p2p, self.ok_interop,
                                     {"available": True})
        self.assertEqual(plan["status"], "READY_LOCAL_ONLY")
        self.assertIn("4 GiB", plan["reason"])

    def test_runtime_without_transport_never_reports_remote(self):
        plan = mgpu_auto.select_plan(self.gpus, self.ok_p2p, self.ok_interop,
                                     {"available": True,
                                      "transport_available": False,
                                      "transport_reason": "cross-adapter pendiente"})
        self.assertEqual(plan["status"], "READY_LOCAL_ONLY")
        self.assertEqual(plan["reason"], "cross-adapter pendiente")

    def test_failed_transport_is_not_remote(self):
        plan = mgpu_auto.select_plan(self.gpus, {"available": False}, self.ok_interop)
        self.assertEqual(plan["status"], "P2P_UNAVAILABLE")


class RuntimeAndProfileTests(unittest.TestCase):
    def test_remote_mvp_requires_explicit_runtime_environment(self):
        with mock.patch.dict(mgpu_auto.os.environ, {}, clear=True):
            report = mgpu_auto.remote_mvp_report()

        self.assertFalse(report["available"])
        self.assertIn("PROTON", report["error"])
        self.assertIn("DLSS_NR_DLL", report["error"])

    def test_remote_mvp_accepts_explicit_core_without_demo_bootstrap(self):
        environment = {
            name: "/tmp/test"
            for name in ("PROTON", "NGX_SDK_DIR", "DLSS_RUNTIME_DLL",
                         "DLSS_NR_DLL", "VKD3D_DLL_DIR", "MGPU_NGX_CORE_DLL")
        }
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            probe.chmod(0o755)
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(
                     mgpu_auto.subprocess, "run",
                     return_value=mock.Mock(returncode=1, stdout="", stderr="")):
                report = mgpu_auto.remote_mvp_report()

        self.assertFalse(report["available"])
        self.assertNotIn("DLSS_DEMO_DIR", report["error"])

    def test_pci_selector_is_propagated_by_host_runners(self):
        root = Path(__file__).resolve().parents[1]
        ngx_runner = (root / "scripts/run_ngx_test.sh").read_text(encoding="utf-8")
        official_runner = (root / "scripts/run_official_d3d12_host_probe.sh").read_text(
            encoding="utf-8")
        self.assertGreaterEqual(ngx_runner.count("MGPU_NGX_PRIMARY_PCI="), 2)
        self.assertIn("MGPU_NGX_PRIMARY_PCI=\"${HOST_PRIMARY_PCI}\"", official_runner)

    def test_cross_adapter_runner_has_external_watchdog(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_d3d12_cross_adapter_frame_probe.sh").read_text(
            encoding="utf-8")
        self.assertIn('PROBE_TIMEOUT_SECONDS="${MGPU_CROSS_ADAPTER_TIMEOUT_SECONDS:-60}"',
                      runner)
        self.assertIn("setsid timeout --signal=TERM --kill-after=5s", runner)

    def test_cross_adapter_probe_reports_gpu_native_fence_diagnostics(self):
        root = Path(__file__).resolve().parents[1]
        smoke = (root / "tests/d3d12_cross_adapter_frame_smoke.cpp").read_text(
            encoding="utf-8")
        self.assertIn('gpu_native_fence_export_a_hr', smoke)
        self.assertIn('gpu_native_fence_export_b_hr', smoke)
        self.assertIn('gpu_native_fence_fd_a', smoke)
        self.assertIn('gpu_native_fence_fd_b', smoke)

    def test_ngx_runner_wires_optional_dxvk(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_ngx_test.sh").read_text(encoding="utf-8")
        self.assertIn('DXVK_DIR="${MGPU_DXVK_DIR:-}"', runner)
        self.assertIn('cp "${DXVK_DIR}/dxgi.dll"', runner)
        self.assertIn("WINEDLLOVERRIDES=", runner)

    def test_remote_ngx_auto_selects_pair_worker_profile(self):
        payload = {"gpu_a_to_b": True, "resource_fd_mode": True,
                   "resource_planes_readback": True, "helper_p2p": True,
                   "queue_a_cpu_fence": True, "queue_b_cpu_fence": True,
                   "readback_validation": True, "ngx_b_evaluate": True,
                   "ngx_b_readback": True}
        environment = {
            name: "/tmp/test"
            for name in ("PROTON", "NGX_SDK_DIR", "DLSS_RUNTIME_DLL",
                         "DLSS_NR_DLL", "VKD3D_DLL_DIR")
        }
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd-pair-worker-remote-ngx"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            probe.chmod(0o755)
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(
                     mgpu_auto.subprocess, "run",
                     return_value=mock.Mock(returncode=0,
                                            stdout=json.dumps(payload), stderr="")) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertFalse(report["available"])
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertTrue(worker_environment["NGX_BRIDGE_DIR"].endswith(
            "build/proton-resource-pair-worker-experimental"))

    def test_remote_mvp_accepts_only_a_complete_success_json(self):
        payload = {
            "gpu_a_to_b": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout="", stderr=json.dumps(payload) + "\n")
        environment = {
            name: "/tmp/test"
            for name in ("PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR",
                         "DLSS_RUNTIME_DLL", "DLSS_NR_DLL", "VKD3D_DLL_DIR")
        }
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", return_value=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertTrue(report["report"]["ngx_b_evaluate"])
        run_mock.assert_called_once()
        self.assertEqual(run_mock.call_args.kwargs["env"]["MGPU_NGX_CROSS_ADAPTER"], "1")

    def test_remote_mvp_both_directions_requires_matching_direction_metadata(self):
        payloads = [
            {"gpu_a_to_b": True, "reverse_direction": False,
             "source_cuda_ordinal": 0, "destination_cuda_ordinal": 1,
             "helper_p2p": True, "queue_a_cpu_fence": True,
             "queue_b_cpu_fence": True, "readback_validation": True,
             "ngx_b_evaluate": True, "ngx_b_readback": True},
            {"gpu_a_to_b": True, "reverse_direction": True,
             "source_cuda_ordinal": 1, "destination_cuda_ordinal": 0,
             "helper_p2p": True, "queue_a_cpu_fence": True,
             "queue_b_cpu_fence": True, "readback_validation": True,
             "ngx_b_evaluate": True, "ngx_b_readback": True},
        ]
        completed = [mock.Mock(returncode=0, stdout=json.dumps(item) + "\n", stderr="")
                     for item in payloads]
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR", "MGPU_REMOTE_DIRECTIONS")}
        environment["MGPU_REMOTE_DIRECTIONS"] = "both"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(len(report["directions"]), 2)
        self.assertEqual(run_mock.call_count, 2)
        self.assertEqual(run_mock.call_args_list[1].kwargs["env"]["MGPU_CROSS_ADAPTER_REVERSE"], "1")

    def test_remote_mvp_resource_fd_transport_adds_plane_gates(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", return_value=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"], "resource-fd")
        self.assertEqual(run_mock.call_args.kwargs["env"]["MGPU_CROSS_ADAPTER_RESOURCE_FD"], "1")

    def test_remote_mvp_resource_fd_propagates_frame_loop_flags(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
            "frame_loop_requested": True,
            "frame_loop_success": True,
            "frame_loop_payload_varied": True,
            "frame_loop_frames_completed": 3,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd"
        environment["MGPU_REMOTE_FRAME_LOOP"] = "1"
        environment["MGPU_REMOTE_FRAME_LOOP_FRAMES"] = "3"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", return_value=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_FRAME_LOOP"], "1")
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_FRAME_COUNT"], "3")

    def test_remote_mvp_resource_pair_daemon_requires_returned_output(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_daemon_mode": True,
            "resource_daemon_commands": 8,
            "remote_output_returned": True,
            "remote_output_nonzero": 6216988,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-pair-daemon"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", return_value=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"], "resource-pair-daemon")
        self.assertEqual(run_mock.call_args.kwargs["env"]["MGPU_CROSS_ADAPTER_RESOURCE_DAEMON"], "1")
        self.assertEqual(run_mock.call_args.kwargs["env"]["MGPU_CROSS_ADAPTER_DAEMON_REPEAT"], "8")

    def test_remote_mvp_bridge_pair_worker_sets_bridge_environment(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd-pair-worker"
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "run_d3d12_cross_adapter_frame_probe.sh"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", return_value=completed) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"], "resource-fd-pair-worker")
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_DLSSNR_TRANSPORT"],
                         "resource-fd-pair-worker")
        self.assertEqual(worker_environment["MGPU_REMOTE_ADAPTER_INDEX"], "0")
        self.assertTrue(worker_environment["MGPU_CUDA_WORKER_HELPER"].endswith(
            "build/mgpu-cuda-external-p2p-copy-helper"))

    def test_remote_mvp_remote_ngx_requires_bridge_log_gates(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd-pair-worker-remote-ngx"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)

            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=1 completed=1 wait=0\n"
                    "output_return_copy=ok output_return_validation=ok "
                    "fnv1a=0x1234 nonzero=10 response=OK 1417 1234 10\n",
                    encoding="utf-8")
                return completed

            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"], "resource-fd-pair-worker-remote-ngx")
        self.assertTrue(report["remote_ngx"][0]["evaluate"])
        self.assertTrue(report["remote_ngx"][0]["submit"])
        self.assertTrue(report["remote_ngx"][0]["output_returned"])
        self.assertTrue(report["remote_ngx"][0]["output_validation"])
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_NGX_PRIME_SOURCE"], "0")
        self.assertEqual(worker_environment["MGPU_DLSSNR_SKIP_LOCAL_NGX"], "1")
        self.assertEqual(worker_environment["MGPU_DLSSNR_REMOTE_NGX_FEATURE"], "1")
        self.assertEqual(worker_environment["MGPU_DLSSNR_VALIDATE_REMOTE_OUTPUT"], "1")

    def test_remote_mvp_sequential_dual_requires_local_after_remote_gates(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = (
            "resource-fd-pair-worker-sequential-dual")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)

            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=1 completed=1 wait=0\n"
                    "output_return_copy=ok output_return_validation=ok "
                    "fnv1a=0x1234 nonzero=10 response=OK 1417 1234 10\n"
                    "local_after_remote_init result=0x00000001\n"
                    "local_after_remote_create result=0x00000001\n"
                    "DLSSNR Evaluate result=0x00000001 frame=1\n",
                    encoding="utf-8")
                return completed

            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"],
                         "resource-fd-pair-worker-sequential-dual")
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_DLSSNR_PROBE_LOCAL_AFTER_REMOTE"], "1")
        self.assertEqual(worker_environment["MGPU_DLSSNR_RESET_REMOTE_BEFORE_LOCAL"], "1")
        self.assertTrue(report["remote_ngx"][0]["local_after_remote_create"])

    def test_remote_mvp_persistent_requires_multiple_completed_frames(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
            "ngx_b_frames_requested": 3,
            "ngx_b_frames_completed": 3,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = (
            "resource-fd-pair-worker-remote-ngx-persistent")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)

            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=3 completed=3 wait=0\n"
                    "remote_ngx_frame_reset result=0x00000000 frame=2\n"
                    "remote_ngx_frame_reset result=0x00000000 frame=3\n"
                    "output_return_copy=ok output_return_validation=ok\n",
                    encoding="utf-8")
                return completed

            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertEqual(report["transport"],
                         "resource-fd-pair-worker-remote-ngx-persistent")
        self.assertEqual(report["remote_ngx"][0]["persistent_frames"], 3)
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_DLSSNR_REMOTE_NGX_PERSISTENT"], "1")
        self.assertEqual(worker_environment["MGPU_NGX_FRAME_COUNT"], "3")

    def test_remote_mvp_presentation_accepts_automatic_orientation_retry(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": True,
            "source_cuda_ordinal": 1,
            "destination_cuda_ordinal": 0,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
            "ngx_b_frames_requested": 3,
            "ngx_b_frames_completed": 3,
            "presentation_requested": True,
            "presentation_success": True,
            "presentation_frames_requested": 3,
            "presentation_frames_presented": 3,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = (
            "resource-fd-pair-worker-remote-ngx-persistent")
        environment["MGPU_REMOTE_PRESENT"] = "1"
        environment["MGPU_REMOTE_PRESENT_FRAMES"] = "3"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)

            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=3 completed=3 wait=0\n"
                    "output_return_copy=ok output_return_validation=ok\n",
                    encoding="utf-8")
                return completed

            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertTrue(report["presentation_requested"])
        self.assertTrue(report["directions"][0]["presentation"]["success"])
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_PRESENT"], "1")
        self.assertEqual(worker_environment["MGPU_PRESENT_FRAMES"], "3")
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_PRESENT_AUTO"], "1")

    def test_remote_mvp_raster_fixture_is_an_explicit_gate(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
            "raster_requested": True,
            "raster_ready": True,
            "raster_submitted": True,
            "readback_nonzero": 123,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd-pair-worker-remote-ngx"
        environment["MGPU_REMOTE_RASTER"] = "1"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)
            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=1 completed=1 wait=0\n"
                    "output_return_copy=ok output_return_validation=ok\n",
                    encoding="utf-8")
                return completed
            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertTrue(report["raster_requested"])
        self.assertEqual(run_mock.call_args.kwargs["env"]["MGPU_CROSS_ADAPTER_RASTER"], "1")

    def test_remote_mvp_frame_loop_is_an_explicit_gate(self):
        payload = {
            "gpu_a_to_b": True,
            "reverse_direction": False,
            "source_cuda_ordinal": 0,
            "destination_cuda_ordinal": 1,
            "resource_fd_mode": True,
            "resource_planes_readback": True,
            "helper_p2p": True,
            "queue_a_cpu_fence": True,
            "queue_b_cpu_fence": True,
            "readback_validation": True,
            "ngx_b_evaluate": True,
            "ngx_b_readback": True,
            "frame_loop_requested": True,
            "frame_loop_success": True,
            "frame_loop_payload_varied": True,
            "frame_loop_frames_completed": 3,
        }
        completed = mock.Mock(returncode=0, stdout=json.dumps(payload) + "\n", stderr="")
        environment = {name: "/tmp/test" for name in (
            "PROTON", "NGX_SDK_DIR", "DLSS_DEMO_DIR", "DLSS_RUNTIME_DLL",
            "DLSS_NR_DLL", "VKD3D_DLL_DIR")}
        environment["MGPU_REMOTE_TRANSPORT"] = "resource-fd-pair-worker-remote-ngx"
        environment["MGPU_REMOTE_FRAME_LOOP"] = "1"
        environment["MGPU_REMOTE_FRAME_LOOP_FRAMES"] = "3"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            probe = root / "run_d3d12_cross_adapter_frame_probe.sh"
            output_dir = root / "out"
            output_dir.mkdir()
            log_path = output_dir / "dlssnr-proxy.log"
            probe.write_text("#!/bin/sh\n", encoding="utf-8")
            environment["OUT_DIR"] = str(output_dir)

            def fake_run(*_args, **_kwargs):
                log_path.write_text(
                    "remote_ngx_evaluate result=0x00000001\n"
                    "remote_ngx_submit result=0x00000000 "
                    "device_removed=0x00000000 fence=1 completed=1 wait=0\n"
                    "output_return_copy=ok output_return_validation=ok\n",
                    encoding="utf-8")
                return completed

            with mock.patch.dict(mgpu_auto.os.environ, environment, clear=True), \
                 mock.patch.object(mgpu_auto, "REMOTE_MVP_PROBE", probe), \
                 mock.patch.object(mgpu_auto.subprocess, "run", side_effect=fake_run) as run_mock:
                report = mgpu_auto.remote_mvp_report()

        self.assertTrue(report["available"])
        self.assertTrue(report["frame_loop_requested"])
        worker_environment = run_mock.call_args.kwargs["env"]
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_FRAME_LOOP"], "1")
        self.assertEqual(worker_environment["MGPU_CROSS_ADAPTER_FRAME_COUNT"], "3")

    def test_image_cuda_p2p_report_requires_both_directions_and_readback(self):
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "mgpu-vulkan-image-cuda-p2p-probe"
            probe.write_bytes(b"probe")
            outputs = [
                mock.Mock(returncode=0, stdout=(
                    '{"source":0,"destination":1,"'
                    'cuda_image_allocation_p2p":true,"readback_ok":true}\n'),
                          stderr=""),
                mock.Mock(returncode=0, stdout=(
                    '{"source":1,"destination":0,"'
                    'cuda_image_allocation_p2p":true,"readback_ok":true}\n'),
                          stderr=""),
            ]
            with mock.patch.object(mgpu_auto, "IMAGE_CUDA_P2P_PROBE", probe), \
                 mock.patch.object(mgpu_auto, "run", side_effect=outputs):
                report = mgpu_auto.image_cuda_p2p_report()

            self.assertTrue(report["available"])
            self.assertEqual(
                [(item["source"], item["destination"]) for item in report["directions"]],
                [(0, 1), (1, 0)],
            )

    def test_vulkan_cuda_external_semaphore_report_requires_both_native_directions(self):
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / "mgpu-vulkan-cuda-external-semaphore-probe"
            probe.write_bytes(b"probe")
            outputs = [
                mock.Mock(returncode=0, stdout=(
                    '{"available":true,"vulkan_gpu":0,"cuda_device":1,'
                    '"vulkan_to_cuda":true,"cuda_to_vulkan":true}\n'), stderr=""),
                mock.Mock(returncode=0, stdout=(
                    '{"available":true,"vulkan_gpu":1,"cuda_device":0,'
                    '"vulkan_to_cuda":true,"cuda_to_vulkan":true}\n'), stderr=""),
            ]
            with mock.patch.object(mgpu_auto, "VULKAN_CUDA_SEMAPHORE_PROBE", probe), \
                 mock.patch.object(mgpu_auto, "run", side_effect=outputs):
                report = mgpu_auto.vulkan_cuda_external_semaphore_report()

            self.assertTrue(report["available"])
            self.assertEqual(
                [(item["vulkan_gpu"], item["cuda_device"])
                 for item in report["directions"]],
                [(0, 1), (1, 0)],
            )
            self.assertIn("D3D12/VKD3D", report["scope"])

    def test_maps_vulkan_and_cuda_by_uuid_not_index(self):
        output = (
            "Vulkan device map:\n"
            "  Vulkan 0: NVIDIA GeForce RTX 3090 UUID=aa:bb PCI=0:3:0.0\n"
            "  Vulkan 1: NVIDIA GeForce RTX 3090 UUID=cc:dd PCI=0:1:0.0\n"
            "CUDA device map:\n"
            "  CUDA 0: NVIDIA GeForce RTX 3090 UUID=cc:dd\n"
            "  CUDA 1: NVIDIA GeForce RTX 3090 UUID=aa:bb\n"
        )
        vulkan, cuda = mgpu_auto.parse_device_uuid_maps(output)
        cuda_by_uuid = {uuid: index for index, uuid in cuda.items()}
        self.assertEqual(cuda_by_uuid[vulkan[0]], 1)
        self.assertEqual(cuda_by_uuid[vulkan[1]], 0)

    def test_direct_launch_policy_pins_render_gpu_and_keeps_fallback(self):
        gpu = mgpu_auto.Gpu(0, "RTX 3090", "595", "0000:01:00.0", "Off", 0, 1, 24576)
        policy = mgpu_auto.direct_launch_policy(
            Path("/tmp/Test.exe"), "wine", ["--test"], Path("/tmp/prefix"),
            {"neural_gpu": 1}, gpu,
        )
        self.assertEqual(policy["command"], ["wine", "/tmp/Test.exe", "--test"])
        self.assertEqual(policy["env"]["VKD3D_VULKAN_DEVICE"], "0")
        self.assertEqual(policy["env"]["VKD3D_FILTER_DEVICE_NAME"], "RTX 3090")
        self.assertEqual(policy["env"]["WINEPREFIX"], "/tmp/prefix")
        self.assertEqual(policy["mode"], "local-fallback")

    def test_proton_launch_policy_uses_run_and_compat_data(self):
        gpu = mgpu_auto.Gpu(1, "RTX 3090", "595", "0000:03:00.0", "On", 0, 1, 24576)
        policy = mgpu_auto.direct_launch_policy(
            Path("/tmp/Test.exe"), "/opt/GE-Proton/proton", [], Path("/tmp/compat"),
            {"neural_gpu": 0}, gpu,
        )
        self.assertEqual(policy["command"], ["/opt/GE-Proton/proton", "run", "/tmp/Test.exe"])
        self.assertEqual(policy["env"]["STEAM_COMPAT_DATA_PATH"], "/tmp/compat")
        self.assertEqual(policy["env"]["UMU_USE_STEAM"], "0")
        self.assertNotIn("WINEPREFIX", policy["env"])

    def test_runtime_discovery_and_profile_are_local(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            install = root / "game"
            prefix_system32 = root / "prefix/drive_c/windows/system32"
            bridge_dir = root / "project/build/proton"
            install.mkdir(parents=True)
            prefix_system32.mkdir(parents=True)
            bridge_dir.mkdir(parents=True)
            (install / "nvngx_dlss.dll").write_bytes(b"local")
            (prefix_system32 / "nvngx_dlssnr.dll").write_bytes(b"local")
            bridge = bridge_dir / "bridge-nvngx.dll"
            bridge.write_bytes(b"local")
            game = mgpu_auto.Game("123", "Example", str(install),
                                  str(root / "prefix"), [str(install / "game.exe")])

            with mock.patch.object(mgpu_auto, "ROOT", root / "project"), \
                 mock.patch("pathlib.Path.home", return_value=root / "home"):
                runtime = mgpu_auto.runtime_status(game)
                plan = {"status": "READY_REMOTE", "render_gpu": 0, "neural_gpu": 1}
                profile = mgpu_auto.write_profile(game, plan, runtime)

            self.assertTrue(runtime["available"])
            self.assertIn(str(bridge), runtime["bridge"])
            self.assertTrue(profile.exists())
            content = profile.read_text(encoding="utf-8")
            self.assertIn('render = "0"', content)
            self.assertIn('neural = "1"', content)
            self.assertIn("fallback_local = true", content)

    def test_runtime_proxy_is_not_treated_as_real_dlss(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            install = root / "game"
            prefix_system32 = root / "prefix/drive_c/windows/system32"
            bridge_dir = root / "project/build/proton"
            install.mkdir(parents=True)
            prefix_system32.mkdir(parents=True)
            bridge_dir.mkdir(parents=True)
            (install / "nvngx_dlss.dll").write_bytes(
                b"proxy imports _nvngx_real.dll and bridge-nvngx.dll")
            (prefix_system32 / "nvngx_dlssnr.dll").write_bytes(b"local")
            (bridge_dir / "bridge-nvngx.dll").write_bytes(b"local")
            game = mgpu_auto.Game("123", "Example", str(install),
                                  str(root / "prefix"), [])

            with mock.patch.object(mgpu_auto, "ROOT", root / "project"):
                runtime = mgpu_auto.runtime_status(game)

            self.assertFalse(runtime["available"])
            self.assertEqual(len(runtime["proxy_runtimes"]), 1)
            self.assertIn("runtime DLSS real", runtime["reason"])

    def test_launch_preparation_exposes_local_fallback(self):
        game = mgpu_auto.Game("1", "Game", "/game", "/prefix", [])
        preparation = mgpu_auto.launch_preparation(
            game,
            {"status": "READY_LOCAL_ONLY", "render_gpu": 0, "neural_gpu": 1,
             "reason": "memoria insuficiente"},
            {"available": False},
        )
        self.assertTrue(preparation["ready"])
        self.assertEqual(preparation["mode"], "local-fallback")
        self.assertTrue(preparation["fallback_local"])


if __name__ == "__main__":
    unittest.main()
