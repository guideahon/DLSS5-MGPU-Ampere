import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class SteamContextProbeTests(unittest.TestCase):
    def test_probe_is_read_only_and_finds_login_shape_and_manifest(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            steam = home / ".steam" / "steam"
            (steam / "config").mkdir(parents=True)
            (steam / "config" / "loginusers.vdf").write_text(
                '"users" { "76561198078942239" { "AccountName" "redacted" } }\n',
                encoding="utf-8")
            library = home / "library" / "SteamLibrary"
            manifest_dir = library / "steamapps"
            manifest_dir.mkdir(parents=True)
            (manifest_dir / "appmanifest_275850.acf").write_text(
                '"AppState" { "appid" "275850" }\n', encoding="utf-8")
            before = sorted(path.relative_to(home).as_posix()
                            for path in home.rglob("*"))
            env = os.environ.copy()
            env["HOME"] = str(home)
            env["MGPU_STEAM_LIBRARY_ROOTS"] = str(library)
            command = [sys.executable, str(root / "scripts/probe_steam_context.py"),
                       "--appid", "275850", "--home", str(home), "--json"]
            result = subprocess.run(command, check=True, text=True,
                                    capture_output=True, env=env)
            report = json.loads(result.stdout)
            self.assertTrue(report["login"]["authenticated_looking"])
            self.assertTrue(report["manifest"]["present"])
            self.assertFalse(report["usable_authenticated_context"])
            self.assertEqual(before, sorted(path.relative_to(home).as_posix()
                                            for path in home.rglob("*")))


if __name__ == "__main__":
    unittest.main()
