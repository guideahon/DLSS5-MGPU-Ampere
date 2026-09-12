import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class NmsDlssSeedTests(unittest.TestCase):
    def test_seed_is_isolated_to_prefix_and_writes_dlss_settings(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            prefix = Path(temp) / "compat-data"
            command = [sys.executable, str(root / "scripts/seed_nms_dlss.py"),
                       "--prefix", str(prefix), "--json"]
            result = subprocess.run(command, check=True, text=True,
                                    capture_output=True)
            report = json.loads(result.stdout)
            self.assertEqual(report["dlss_quality"], "MaxQuality")
            self.assertEqual(report["frame_generation"], "Off")
            for item in report["settings"]:
                settings = Path(item["path"])
                self.assertTrue(settings.is_file())
                text = settings.read_text(encoding="utf-8")
                self.assertIn('name="DLSSQuality" value="MaxQuality"', text)
                self.assertIn('name="DLSSFrameGeneration" value="Off"', text)
                self.assertTrue(str(settings).startswith(str(prefix)))


if __name__ == "__main__":
    unittest.main()
