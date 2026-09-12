#!/usr/bin/env python3
"""Create a development-only copy of a Streamline DLL without its signature gate.

This never edits the input file.  The byte pattern is deliberately strict and
the tool refuses binaries whose layout is different or ambiguous.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


CHECK_PATTERN = bytes.fromhex("85 c0 0f 94 c3 85 c0 74")
RETURN_PATTERN = bytes.fromhex("0f b6 c3")


def patch_streamline_signature(source: Path, destination: Path) -> dict[str, int | str]:
    source = source.expanduser().resolve()
    destination = destination.expanduser().resolve()
    if source == destination:
        raise ValueError("refusing to patch the input file in place")
    data = bytearray(source.read_bytes())
    offsets = []
    cursor = 0
    while True:
        offset = data.find(CHECK_PATTERN, cursor)
        if offset < 0:
            break
        offsets.append(offset)
        cursor = offset + 1
    if len(offsets) != 1:
        raise RuntimeError(
            f"unexpected Streamline signature layout: {len(offsets)} matches"
        )
    check = offsets[0]
    target = data.find(RETURN_PATTERN, check + len(CHECK_PATTERN), check + 0x300)
    if target < 0:
        raise RuntimeError("signature return epilogue not found")
    if data.find(RETURN_PATTERN, target + 1, check + 0x300) >= 0:
        raise RuntimeError("signature return epilogue is ambiguous")

    # sete bl becomes bl=1, and the conditional failure branch becomes an
    # unconditional jump to the existing success epilogue (movzx eax, bl).
    data[check + 2:check + 5] = b"\xb3\x01\x90"
    branch = check + 7
    relative = target - (branch + 5)
    data[branch:branch + 6] = b"\xe9" + struct.pack("<i", relative) + b"\x90"

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    destination.chmod(source.stat().st_mode & 0o7777)
    return {"check_offset": check, "return_offset": target,
            "source": str(source), "destination": str(destination)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(patch_streamline_signature(args.input, args.output),
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
