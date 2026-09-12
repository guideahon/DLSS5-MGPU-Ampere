import json
import tempfile
import unittest
from pathlib import Path

from scripts.seed_cyberpunk_dlss import write_cyberpunk_dlss_profile


class CyberpunkProfileTests(unittest.TestCase):
    def test_profile_is_written_inside_prefix_and_selects_dlss(self):
        with tempfile.TemporaryDirectory() as temp:
            target = write_cyberpunk_dlss_profile(Path(temp) / "compat")
            self.assertTrue(target.is_file())
            self.assertIn("pfx/drive_c/users/steamuser", str(target))
            payload = json.loads(target.read_text(encoding="utf-8"))

        groups = {group["group_name"]: group["options"]
                  for group in payload["data"]}
        options = {option["name"]: option for option in groups["/graphics/dlss"]}
        self.assertEqual(options["DLSS"]["value"], "Quality")
        self.assertFalse(options["DLSSFrameGen"]["value"])
        self.assertFalse(options["DLAA"]["value"])
        dynamic = {option["name"]: option
                   for option in groups["/graphics/dynamicresolution"]}
        self.assertEqual(dynamic["FSR2"]["value"], "Off")
        self.assertEqual(dynamic["XESS"]["value"], "Off")


if __name__ == "__main__":
    unittest.main()
