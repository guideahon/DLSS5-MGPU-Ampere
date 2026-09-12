import tempfile
import unittest
from pathlib import Path

from scripts.prepare_proton_mgpu_runner import prepare_runner


class ProtonRunnerTests(unittest.TestCase):
    def test_runner_is_private_and_gates_nvidia_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source_root = root / "GE-Proton"
            source_root.mkdir()
            source_runner = source_root / "proton"
            source_runner.write_text(
                "#!/usr/bin/env python3\n"
                "                if g_session.nvidia_wine_dll_dir:\n"
                "                    copy_driver_dlls()\n",
                encoding="utf-8",
            )
            source_runner.chmod(0o755)
            (source_root / "files").mkdir()
            (source_root / "utilities.py").write_text("# fixture\n",
                                                       encoding="utf-8")

            private = prepare_runner(source_runner, root / "private")
            self.assertTrue(private.is_file())
            self.assertEqual(private.stat().st_mode & 0o111, 0o111)
            self.assertIn(
                'os.environ.get("MGPU_PROTON_COPY_NVIDIA_NGX", "1") != "0"',
                private.read_text(encoding="utf-8"),
            )
            self.assertTrue((private.parent / "files").is_symlink())
            self.assertTrue((private.parent / "utilities.py").is_symlink())
            self.assertNotEqual(private, source_runner)


if __name__ == "__main__":
    unittest.main()
