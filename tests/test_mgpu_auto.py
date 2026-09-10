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
