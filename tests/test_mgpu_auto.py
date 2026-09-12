import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts import mgpu_auto


class DiscoveryTests(unittest.TestCase):
    def test_compose_winedllpath_deduplicates_inherited_entries(self):
        self.assertEqual(
            mgpu_auto.compose_winedllpath("/tmp/a", "/tmp/b:/tmp/a", "", "/tmp/c"),
            "/tmp/a:/tmp/b:/tmp/c",
        )

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

            with mock.patch.object(mgpu_auto, "steam_roots", return_value=[steam_root]), \
                 mock.patch("pathlib.Path.home", return_value=root / "home"):
                games = mgpu_auto.discover_games()

            self.assertEqual(len(games), 1)
            self.assertEqual(games[0].appid, "123")
            self.assertEqual(games[0].name, "Example Game")
            self.assertEqual(games[0].executables, [str(install / "ExampleGame.exe")])

    def test_discovers_explicit_mounted_library_without_steam_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            library = Path(temp) / "SteamLibrary"
            manifest_dir = library / "steamapps"
            install = manifest_dir / "common" / "Mounted Game"
            install.mkdir(parents=True)
            (install / "MountedGame.exe").write_bytes(b"MZ")
            (manifest_dir / "appmanifest_456.acf").write_text(
                '"AppState" {\n'
                '  "appid" "456"\n'
                '  "name" "Mounted Game"\n'
                '  "installdir" "Mounted Game"\n'
                '}\n', encoding="utf-8")

            with mock.patch.dict(mgpu_auto.os.environ, {
                    "MGPU_STEAM_LIBRARY_ROOTS": str(library),
            }, clear=True), \
                 mock.patch.object(mgpu_auto, "steam_roots", return_value=[]), \
                 mock.patch("pathlib.Path.home", return_value=Path(temp) / "home"):
                games = mgpu_auto.discover_games()

            self.assertEqual(len(games), 1)
            self.assertEqual(games[0].appid, "456")
            self.assertEqual(games[0].install_dir, str(install))


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
    def test_launch_policy_preserves_opt_in_vkd3d_identity_selectors(self):
        game = mgpu_auto.Game(
            "direct", "Example", "/tmp/game", "/tmp/prefix", ["/tmp/game.exe"])
        plan = {"status": "READY_REMOTE", "render_gpu": 1, "neural_gpu": 0}
        runtime = {
            "proton": "/tmp/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/helper",
            "remote_profile": "/tmp/profile",
            "bridge": [],
            "remote_runtime": {
                "core": "/tmp/profile/_nvngx_real.dll",
                "dlss": "/tmp/profile/nvngx_dlss_real.dll",
                "nr": "/tmp/profile/nvngx_dlssnr.dll",
            },
        }
        with mock.patch.dict(mgpu_auto.os.environ, {
                "VKD3D_DUPLICATE_LUID_INDEX": "1",
                "VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE": "1",
        }, clear=True):
            policy = mgpu_auto.launch_preparation(game, plan, runtime)

        self.assertEqual(policy["env"]["VKD3D_DUPLICATE_LUID_INDEX"], "1")
        self.assertEqual(
            policy["env"]["VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE"], "1")

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
        self.assertIn('REQUIRE_DISTINCT_IDENTITY="${MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY:-1}"',
                      runner)
        self.assertIn("setsid timeout --signal=TERM --kill-after=5s", runner)

    def test_vkd3d_fence_smoke_has_internal_watchdog(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_vkd3d_cross_adapter_fence_smoke.sh").read_text(
            encoding="utf-8")
        self.assertIn('PROBE_TIMEOUT_SECONDS="${MGPU_VKD3D_FENCE_TIMEOUT_SECONDS:-60}"',
                      runner)
        self.assertIn("exec setsid timeout --signal=TERM --kill-after=5s", runner)

    def test_cross_adapter_probe_reports_gpu_native_fence_diagnostics(self):
        root = Path(__file__).resolve().parents[1]
        smoke = (root / "tests/d3d12_cross_adapter_frame_smoke.cpp").read_text(
            encoding="utf-8")
        self.assertIn('gpu_native_fence_export_a_hr', smoke)
        self.assertIn('gpu_native_fence_export_b_hr', smoke)
        self.assertIn('gpu_native_fence_fd_a', smoke)
        self.assertIn('gpu_native_fence_fd_b', smoke)
        self.assertIn('MGPU_CROSS_ADAPTER_REQUIRE_DISTINCT_IDENTITY', smoke)
        self.assertIn('physical_identity_distinct', smoke)

    def test_cross_adapter_smoke_can_require_real_command_list_queue(self):
        root = Path(__file__).resolve().parents[1]
        smoke = (root / "tests/d3d12_cross_adapter_frame_smoke.cpp").read_text(
            encoding="utf-8")
        self.assertIn("MGPU_CROSS_ADAPTER_QUEUE_SPI", smoke)
        self.assertIn("MGPU_CROSS_ADAPTER_REQUIRE_QUEUE_SPI", smoke)
        self.assertIn("MGPU_CROSS_ADAPTER_QUEUE_SPI_ONLY", smoke)
        self.assertIn("queue_spi_success", smoke)
        self.assertIn("IID_ID3D12DXVKInteropDevice7", smoke)

    def test_gpu_native_ngx_composite_gate_cannot_pass_partially(self):
        root = Path(__file__).resolve().parents[1]
        smoke = (root / "tests/d3d12_cross_adapter_frame_smoke.cpp").read_text(
            encoding="utf-8")
        self.assertIn("MGPU_CROSS_ADAPTER_REQUIRE_NGX_WITH_GPU_NATIVE", smoke)
        self.assertIn('gpu_native_ngx_composite_requested', smoke)
        self.assertIn('gpu_native_ngx_composite_success', smoke)
        self.assertIn('ngx_frames_completed == ngx_frame_count', smoke)

    def test_vkd3d_build_uses_strict_duplicate_luid_identity_guard(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_vkd3d_experimental.sh").read_text(
            encoding="utf-8")
        patch = (root / "patches/vkd3d-duplicate-luid-strict-identity.patch").read_text(
            encoding="utf-8")
        self.assertIn("vkd3d-duplicate-luid-strict-identity.patch", builder)
        self.assertIn("Could not select a distinct Vulkan physical device", patch)
        self.assertIn('PATCH_FILES[@]:0:7', builder)

    def test_vkd3d_interop_probe_uses_per_device_identity_sequence(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_vkd3d_experimental.sh").read_text(
            encoding="utf-8")
        runner = (root / "scripts/run_vkd3d_interop_probe.sh").read_text(
            encoding="utf-8")
        patch = (root / "patches/vkd3d-duplicate-luid-per-device.patch").read_text(
            encoding="utf-8")
        self.assertIn("vkd3d-duplicate-luid-per-device.patch", builder)
        self.assertIn("vkd3d-mingw-pathcch-compat.patch", builder)
        self.assertIn("VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE", runner)
        self.assertIn("duplicate_luid_next_index", patch)

    def test_vkd3d_build_wires_command_list_queue_spi(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_vkd3d_experimental.sh").read_text(
            encoding="utf-8")
        patch = (root / "patches/vkd3d-command-list-queue-spi.patch").read_text(
            encoding="utf-8")
        self.assertIn("vkd3d-command-list-queue-spi.patch", builder)
        self.assertIn("ID3D12DXVKInteropDevice7", patch)
        self.assertIn("GetCommandListQueue", patch)
        self.assertIn("last_submit_queue", patch)

    def test_winevulkan_build_orders_overlapping_extension_patches(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_winevulkan_experimental.sh").read_text(
            encoding="utf-8")
        memory = builder.index("winevulkan-expose-external-memory-fd.patch")
        semaphore = builder.index("winevulkan-expose-external-semaphore-fd.patch")
        self.assertLess(memory, semaphore)
        self.assertIn("wine-win32u-import-memory-fd.patch", builder)

    def test_bridge_has_opt_in_gpu_native_pair_probe(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_bridge.sh").read_text(encoding="utf-8")
        patch = (root / "patches/dlss5-linux-bridge-gpu-native-probe.patch").read_text(
            encoding="utf-8")
        self.assertIn("dlss5-linux-bridge-gpu-native-probe.patch", builder)
        self.assertIn("MGPU_DLSSNR_GPU_NATIVE_SYNC_PROBE", patch)
        self.assertIn("gpu_native_bridge_probe", patch)

    def test_bridge_wires_command_list_queue_probe(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_bridge.sh").read_text(encoding="utf-8")
        patch = (root / "patches/dlss5-linux-bridge-command-list-queue.patch").read_text(
            encoding="utf-8")
        self.assertIn("dlss5-linux-bridge-command-list-queue.patch", builder)
        self.assertIn("MGPU_DLSSNR_GPU_NATIVE_QUEUE_PROBE", patch)
        self.assertIn("kVkd3dInteropDevice7", patch)
        self.assertIn("GetCommandListQueue", patch)

    def test_bridge_wires_loader_audit(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_bridge.sh").read_text(encoding="utf-8")
        patch = (root / "patches/dlss5-linux-bridge-loader-audit.patch").read_text(
            encoding="utf-8")
        self.assertIn("dlss5-linux-bridge-loader-audit.patch", builder)
        self.assertIn("loader_audit dll_process_attach", patch)
        self.assertIn('extern "C" BOOL WINAPI DllMain', patch)
        self.assertIn("DllMain", patch)

    def test_bridge_wires_remote_only_create_fallback(self):
        root = Path(__file__).resolve().parents[1]
        builder = (root / "scripts/build_bridge.sh").read_text(encoding="utf-8")
        patch = (root / "patches/dlss5-linux-bridge-remote-create-fallback.patch").read_text(
            encoding="utf-8")
        self.assertIn("dlss5-linux-bridge-remote-create-fallback.patch", builder)
        self.assertIn("synthetic_remote_handle", patch)
        self.assertIn("RemoteNgxFeatureProbeEnabled", patch)
        self.assertIn("remote_only local EvaluateFeature skipped", patch)
        self.assertIn("std::lock_guard<std::mutex> lock(state_mutex)", patch)

    def test_ngx_runner_wires_optional_dxvk(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_ngx_test.sh").read_text(encoding="utf-8")
        self.assertIn('DXVK_DIR="${MGPU_DXVK_DIR:-}"', runner)
        self.assertIn('cp "${DXVK_DIR}/dxgi.dll"', runner)
        self.assertIn("WINEDLLOVERRIDES=", runner)

    def test_ngx_runner_can_reuse_existing_core_without_official_demo(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_ngx_test.sh").read_text(encoding="utf-8")
        self.assertIn('SKIP_OFFICIAL_DEMO="${MGPU_NGX_SKIP_OFFICIAL_DEMO:-0}"', runner)
        self.assertIn("MGPU_NGX_SKIP_OFFICIAL_DEMO=1", runner)
        self.assertIn('MGPU_NGX_CORE_DLL:-${POSITIVE_PREFIX}', runner)
        self.assertIn('cp "${BRIDGE_DIR}/_nvngx.dll" "${TEST_DIR}/_nvngx.dll"', runner)

    def test_b_first_probe_autodetects_existing_runtime_profile(self):
        root = Path(__file__).resolve().parents[1]
        probe = (root / "scripts/run_ngx_same_process_b_probe.sh").read_text(
            encoding="utf-8")
        self.assertIn("MGPU_NGX_PROFILE_DIR:-${ROOT_DIR}/build/"
                      "proton-resource-pair-worker-experimental", probe)
        self.assertIn('export NGX_BRIDGE_DIR="${PROFILE_DIR}"', probe)
        self.assertIn('export MGPU_NGX_CORE_DLL="${PROFILE_DIR}/_nvngx_real.dll"', probe)
        self.assertIn('export MGPU_NGX_SKIP_OFFICIAL_DEMO=', probe)
        self.assertIn('SDK_CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/dlss5-sdk/DLSS"', probe)
        self.assertIn('export NGX_SDK_DIR="${SDK_CACHE_DIR}"', probe)

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

    def test_direct_remote_policy_requires_explicit_prefix(self):
        policy = mgpu_auto.direct_remote_launch_policy(
            Path("/tmp/Test.exe"), "/opt/GE-Proton/proton", [], None,
            {"status": "READY_REMOTE"}, {},
        )
        self.assertFalse(policy["ready"])
        self.assertIn("--prefix", policy["reason"])

    def test_direct_remote_policy_reuses_pair_worker_environment(self):
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "remote_profile": "/tmp/project/build/proton",
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/mgpu-cuda-external-p2p-copy-helper",
            "remote_runtime": {
                "core": "/tmp/project/build/proton/_nvngx_real.dll",
                "dlss": "/tmp/project/build/proton/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/proton/nvngx_dlssnr.dll",
            },
        }
        policy = mgpu_auto.direct_remote_launch_policy(
            Path("/tmp/game/CitySample.exe"), "/opt/GE-Proton/proton",
            ["-dx12"], Path("/tmp/compat"),
            {"status": "READY_REMOTE", "render_gpu": 0, "neural_gpu": 1},
            runtime,
        )
        self.assertTrue(policy["ready"])
        self.assertEqual(policy["mode"], "remote-neural")
        self.assertEqual(policy["command"], [
            "/opt/GE-Proton/proton", "run", "/tmp/game/CitySample.exe", "-dx12",
        ])
        self.assertEqual(policy["env"]["STEAM_COMPAT_DATA_PATH"], "/tmp/compat")
        self.assertEqual(policy["env"]["MGPU_DLSSNR_TRANSPORT"],
                         "resource-fd-pair-worker")
        self.assertEqual(policy["env"]["MGPU_CROSS_ADAPTER_GPU_NATIVE"], "0")
        self.assertEqual(policy["env"]["PROTON_ENABLE_NVAPI"], "1")
        self.assertIn("/tmp/project/build/proton", policy["env"]["WINEDLLPATH"])
        self.assertEqual(policy["env"]["WINEDLLOVERRIDES"],
                         "_nvngx=n,b;d3d12=n,b;d3d12core=n,b;"
                         "nvngx_dlss=n;nvngx_dlssnr=n")

    def test_direct_remote_policy_can_prefer_streamline_development_copies(self):
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "remote_profile": "/tmp/project/build/proton",
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/helper",
            "remote_runtime": {
                "core": "/tmp/project/build/_nvngx_real.dll",
                "dlss": "/tmp/project/build/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/nvngx_dlssnr.dll",
            },
        }
        with mock.patch.dict(mgpu_auto.os.environ, {
                "MGPU_STREAMLINE_DEV_DLL_DIR": "/tmp/streamline-dev",
        }, clear=False):
            policy = mgpu_auto.direct_remote_launch_policy(
                Path("/tmp/game/Cyberpunk2077.exe"),
                "/opt/GE-Proton/proton", [], Path("/tmp/prefix"),
                {"status": "READY_REMOTE", "render_gpu": 0,
                 "neural_gpu": 1}, runtime)
        self.assertEqual(policy["env"]["MGPU_STREAMLINE_DEV_DLL_DIR"],
                         "/tmp/streamline-dev")
        self.assertEqual(policy["env"]["WINEDLLPATH"].split(os.pathsep)[0],
                         "/tmp/streamline-dev")
        self.assertTrue(policy["env"]["WINEDLLOVERRIDES"].startswith(
            "sl.interposer=n,b;sl.common=n,b;"))

    def test_direct_remote_policy_preserves_opt_in_steam_identity(self):
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "remote_profile": "/tmp/project/build/proton",
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/helper",
            "remote_runtime": {
                "core": "/tmp/project/build/_nvngx_real.dll",
                "dlss": "/tmp/project/build/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/nvngx_dlssnr.dll",
            },
        }
        with mock.patch.dict(mgpu_auto.os.environ, {
                "SteamAppId": "2050650",
                "SteamGameId": "2050650",
                "SteamClientLaunch": "1",
                "SL_ENABLE_CONSOLE_LOGGING": "1",
                "SL_LOG_LEVEL": "verbose",
        }, clear=False):
            policy = mgpu_auto.direct_remote_launch_policy(
                Path("/tmp/game/re4.exe"), "/opt/GE-Proton/proton", [],
                Path("/tmp/prefix"),
                {"status": "READY_REMOTE", "render_gpu": 1, "neural_gpu": 0},
                runtime,
            )
        self.assertEqual(policy["env"]["SteamAppId"], "2050650")
        self.assertEqual(policy["env"]["SteamGameId"], "2050650")
        self.assertEqual(policy["env"]["SteamClientLaunch"], "1")
        self.assertEqual(policy["env"]["SL_ENABLE_CONSOLE_LOGGING"], "1")
        self.assertEqual(policy["env"]["SL_LOG_LEVEL"], "verbose")

    def test_direct_remote_policy_can_opt_into_authenticated_steam_runtime(self):
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "remote_profile": "/tmp/project/build/proton",
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/helper",
            "remote_runtime": {
                "core": "/tmp/project/build/_nvngx_real.dll",
                "dlss": "/tmp/project/build/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/nvngx_dlssnr.dll",
            },
        }
        with mock.patch.dict(mgpu_auto.os.environ, {
                "MGPU_USE_STEAM": "1",
                "MGPU_STEAM_APPID": "275850",
        }, clear=False):
            policy = mgpu_auto.direct_remote_launch_policy(
                Path("/tmp/game/NMS.exe"), "/opt/GE-Proton/proton", [],
                Path("/tmp/prefix"),
                {"status": "READY_REMOTE", "render_gpu": 1, "neural_gpu": 0},
                runtime,
            )
        self.assertTrue(policy["ready"])
        self.assertEqual(policy["env"]["UMU_USE_STEAM"], "1")
        self.assertEqual(policy["env"]["UMU_ID"], "umu-275850")
        self.assertEqual(policy["env"]["SteamAppId"], "275850")
        self.assertNotIn("MGPU_STEAM_CONTEXT_ERROR", policy["env"])

    def test_direct_remote_policy_rejects_missing_steam_appid(self):
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "remote_profile": "/tmp/project/build/proton",
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/helper",
            "remote_runtime": {
                "core": "/tmp/project/build/_nvngx_real.dll",
                "dlss": "/tmp/project/build/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/nvngx_dlssnr.dll",
            },
        }
        with mock.patch.dict(mgpu_auto.os.environ, {
                "MGPU_USE_STEAM": "1",
        }, clear=False):
            policy = mgpu_auto.direct_remote_launch_policy(
                Path("/tmp/game/NMS.exe"), "/opt/GE-Proton/proton", [],
                Path("/tmp/prefix"),
                {"status": "READY_REMOTE", "render_gpu": 1, "neural_gpu": 0},
                runtime,
            )
        self.assertFalse(policy["ready"])
        self.assertIn("MGPU_STEAM_APPID", policy["reason"])

    def test_infer_direct_install_root_finds_unreal_plugin_runtime(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "CitySample"
            executable = root / "Binaries/Win64/CitySample-Win64-Shipping.exe"
            runtime = root / "Plugins/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll"
            executable.parent.mkdir(parents=True)
            runtime.parent.mkdir(parents=True)
            executable.write_bytes(b"exe")
            runtime.write_bytes(b"real")
            self.assertEqual(mgpu_auto.infer_direct_install_root(executable), root)

    def test_remote_launch_policy_is_explicit_and_wires_pair_worker(self):
        game = mgpu_auto.Game(
            "123", "Example", "/tmp/game", "/tmp/compat",
            ["/tmp/game/Example.exe"],
        )
        runtime = {
            "bridge": ["/tmp/project/build/bridge-nvngx.dll"],
            "proton": "/opt/GE-Proton/proton",
            "vkd3d": "/tmp/vkd3d",
            "helper": "/tmp/project/build/mgpu-cuda-external-p2p-copy-helper",
            "remote_runtime": {
                "core": "/tmp/project/build/_nvngx_real.dll",
                "dlss": "/tmp/project/build/nvngx_dlss_real.dll",
                "nr": "/tmp/project/build/nvngx_dlssnr.dll",
            },
        }
        policy = mgpu_auto.launch_preparation(
            game, {"status": "READY_REMOTE", "render_gpu": 0, "neural_gpu": 1},
            runtime,
        )
        self.assertTrue(policy["ready"])
        self.assertEqual(policy["mode"], "remote-neural")
        self.assertEqual(policy["command"], ["/opt/GE-Proton/proton", "run",
                                               "/tmp/game/Example.exe"])
        self.assertEqual(policy["env"]["MGPU_REMOTE_TRANSPORT"],
                         "resource-fd-pair-worker-remote-ngx")
        self.assertEqual(policy["env"]["MGPU_CROSS_ADAPTER_GPU_NATIVE"], "0")
        self.assertEqual(policy["env"]["DLSS_NR_DLL"],
                         "/tmp/project/build/nvngx_dlssnr.dll")

    def test_real_game_probe_is_reversible_and_gpu_native_off(self):
        root = Path(__file__).resolve().parents[1]
        runner = (root / "scripts/run_real_game_remote_probe.sh").read_text(
            encoding="utf-8")
        self.assertIn("trap restore_all EXIT INT TERM", runner)
        self.assertIn("setsid python3", runner)
        self.assertIn("dlss5-guardian-restore", runner)
        self.assertIn("RUNNER_START_TICKS", runner)
        self.assertIn('fields[0] != "Z"', runner)
        self.assertIn("mv -f \"$INJECT_TMP\" \"$GAME_DLL\"", runner)
        self.assertIn("game_dll_restored=true", runner)
        self.assertIn("sha256sum", runner)
        self.assertIn('export MGPU_CROSS_ADAPTER_GPU_NATIVE=0', runner)
        self.assertIn("VKD3D_DUPLICATE_LUID_INDEX_PER_DEVICE=1", runner)
        self.assertIn("--force-system32-ngx", runner)
        self.assertIn('"$RUNNER" run cmd.exe /c exit', runner)
        self.assertIn('PROTON_ENABLE_NVAPI="${PROTON_ENABLE_NVAPI:-1}"', runner)
        self.assertIn("prepare_proton_mgpu_runner.py", runner)
        self.assertIn("MGPU_PROTON_COPY_NVIDIA_NGX=0", runner)
        self.assertIn("system32-nvngx-runtime.sha256", runner)
        self.assertIn("_nvngx_real.dll", runner)
        self.assertIn("bridge-nvngx.dll", runner)
        self.assertIn("--patch-streamline-signature", runner)
        self.assertIn("--streamline-dir", runner)
        self.assertIn("--audit-loader", runner)
        self.assertIn("patch_streamline_signature.py", runner)
        self.assertIn("MGPU_STREAMLINE_DEV_DLL_DIR", runner)
        self.assertIn("sl.interposer=n,b;sl.common=n,b;", runner)
        self.assertIn("PREWARM_TIMEOUT_SECONDS", runner)
        self.assertIn("timeout --signal=TERM --kill-after=5s", runner)
        self.assertIn("cleanup_done=0", runner)
        self.assertIn('kill -TERM "$GUARDIAN_PID"', runner)
        self.assertIn("wait \"$GUARDIAN_PID\"", runner)
        self.assertIn("PROTON_LOG_DIR", runner)
        self.assertIn("loader-audit.status", runner)
        self.assertIn("loader_trace_without_ngx", runner)
        self.assertIn("no_loader_trace_observed", runner)

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

    def test_runtime_status_accepts_project_nr_profile_for_remote_transport(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / "project"
            install = root / "game"
            prefix_system32 = root / "prefix/drive_c/windows/system32"
            profile = project / "build/proton-resource-pair-worker-experimental"
            install.mkdir(parents=True)
            prefix_system32.mkdir(parents=True)
            profile.mkdir(parents=True)
            (install / "nvngx_dlss.dll").write_bytes(b"real")
            proton = root / "proton"
            proton.write_text("#!/bin/sh\n", encoding="utf-8")
            proton.chmod(0o755)
            vkd3d = root / "vkd3d"
            vkd3d.mkdir()
            (vkd3d / "d3d12.dll").write_bytes(b"d3d12")
            (vkd3d / "d3d12core.dll").write_bytes(b"d3d12core")
            (project / "build").mkdir(exist_ok=True)
            (project / "build/mgpu-cuda-external-p2p-copy-helper").write_bytes(b"helper")
            for name in ("_nvngx.dll", "bridge-nvngx.dll", "_nvngx_real.dll",
                         "nvngx_dlss_real.dll", "nvngx_dlssnr.dll"):
                (profile / name).write_bytes(name.encode())
            game = mgpu_auto.Game("123", "Example", str(install),
                                  str(root / "prefix"), [str(install / "game.exe")])
            environment = {"PROTON": str(proton), "VKD3D_DLL_DIR": str(vkd3d)}
            with mock.patch.object(mgpu_auto, "ROOT", project), \
                 mock.patch.dict(mgpu_auto.os.environ, environment, clear=False):
                runtime = mgpu_auto.runtime_status(game)

            self.assertTrue(runtime["available"])
            self.assertTrue(runtime["transport_available"])
            self.assertEqual(runtime["remote_profile"], str(profile))

    def test_runtime_status_auto_selects_project_vkd3d_profile(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            project = root / "project"
            install = root / "game"
            profile = project / "build/proton-resource-pair-worker-experimental"
            install.mkdir(parents=True)
            profile.mkdir(parents=True)
            (install / "nvngx_dlss.dll").write_bytes(b"real")
            proton = root / "proton"
            proton.write_text("#!/bin/sh\n", encoding="utf-8")
            proton.chmod(0o755)
            (project / "build/mgpu-cuda-external-p2p-copy-helper").write_bytes(b"helper")
            for name in ("_nvngx.dll", "bridge-nvngx.dll", "_nvngx_real.dll",
                         "nvngx_dlss_real.dll", "nvngx_dlssnr.dll",
                         "d3d12.dll", "d3d12core.dll"):
                (profile / name).write_bytes(name.encode())
            game = mgpu_auto.Game("123", "Example", str(install),
                                  str(root / "prefix"), [str(install / "game.exe")])

            with mock.patch.object(mgpu_auto, "ROOT", project), \
                 mock.patch.object(mgpu_auto, "REMOTE_NGX_PROFILES", (profile,)), \
                 mock.patch.dict(mgpu_auto.os.environ, {}, clear=True):
                runtime = mgpu_auto.runtime_status(
                    game, proton_override=str(proton))

            self.assertTrue(runtime["transport_available"])
            self.assertEqual(runtime["vkd3d"], str(profile))

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

    def test_execute_direct_creates_only_selected_prefix_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            prefix = root / "new-prefix"
            result = mgpu_auto.execute_direct({
                "command": ["/bin/sh", "-c", "exit 0"],
                "cwd": str(root),
                "env": {"WINEPREFIX": str(prefix)},
            }, 5)
            self.assertEqual(result["return_code"], 0)
            self.assertFalse(result["timed_out"])
            self.assertTrue(prefix.is_dir())

    def test_execute_direct_cleans_detached_child_by_prefix(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            runner = root / "proton"
            runner.write_text(
                "#!/bin/sh\n"
                "setsid sleep 60 >/dev/null 2>&1 &\n"
                "wait\n",
                encoding="utf-8",
            )
            runner.chmod(0o755)
            target = root / "Target.exe"
            target.write_bytes(b"MZ")
            prefix = root / "compat"
            result = mgpu_auto.execute_direct({
                "command": [str(runner), "run", str(target)],
                "cwd": str(root),
                "env": {"STEAM_COMPAT_DATA_PATH": str(prefix)},
            }, 1)
            self.assertTrue(result["timed_out"])
            self.assertFalse(mgpu_auto.owned_process_ids(
                [str(prefix)], {os.getpid(), os.getppid()}))

    def test_process_ancestor_ids_includes_current_process_chain(self):
        ancestors = mgpu_auto.process_ancestor_ids(os.getpid())
        self.assertIn(os.getpid(), ancestors)
        self.assertIn(os.getppid(), ancestors)


if __name__ == "__main__":
    unittest.main()
