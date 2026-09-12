import tempfile
import unittest
from pathlib import Path

from scripts.patch_streamline_signature import patch_streamline_signature


class StreamlineSignaturePatchTests(unittest.TestCase):
    def test_patch_changes_only_the_guard_and_keeps_input(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "sl.common.dll"
            output = root / "patched/sl.common.dll"
            payload = (b"prefix" + bytes.fromhex(
                "85 c0 0f 94 c3 85 c0 74 09"
                "48 8d 0d 08 b9 05 00 eb 37"
                "90 90 90 90 90 90"
                "0f b6 c3"
            ) + b"suffix")
            source.write_bytes(payload)
            source.chmod(0o755)
            original = source.read_bytes()

            result = patch_streamline_signature(source, output)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(result["check_offset"], 6)
            self.assertEqual(output.stat().st_mode & 0o111, 0o111)
            patched = output.read_bytes()
            self.assertEqual(patched[8:11], bytes.fromhex("b3 01 90"))
            self.assertEqual(patched[13:19],
                             bytes.fromhex("e9 0c 00 00 00 90"))


if __name__ == "__main__":
    unittest.main()
