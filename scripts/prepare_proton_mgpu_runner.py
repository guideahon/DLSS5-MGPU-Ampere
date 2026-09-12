#!/usr/bin/env python3
"""Prepare an isolated GE-Proton runner with the NVIDIA NGX copy disabled.

GE-Proton copies the host driver's ``_nvngx.dll`` into every prefix during
``setup_prefix``.  The remote bridge needs to own that path for one isolated
run, so this helper creates a lightweight runner tree made of symlinks and a
private copy of the Proton entrypoint.  The original Proton installation is
never modified.
"""

from __future__ import annotations

import argparse
from pathlib import Path


_COPY_GUARD = '                if g_session.nvidia_wine_dll_dir:\n'
_COPY_GUARD_PATCHED = (
    '                if g_session.nvidia_wine_dll_dir and '
    'os.environ.get("MGPU_PROTON_COPY_NVIDIA_NGX", "1") != "0":\n'
)


def prepare_runner(source_runner: Path, output_root: Path) -> Path:
    source_runner = source_runner.resolve()
    source_root = source_runner.parent
    output_root = output_root.resolve()
    if not source_runner.is_file():
        raise FileNotFoundError(source_runner)
    if output_root.exists() and any(output_root.iterdir()):
        raise FileExistsError(f"output runner is not empty: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    source = source_runner.read_text(encoding="utf-8")
    occurrences = source.count(_COPY_GUARD)
    if occurrences != 1:
        raise RuntimeError(
            "GE-Proton entrypoint has an unexpected NVIDIA NGX copy layout "
            f"(expected 1 marker, found {occurrences})"
        )
    patched = source.replace(_COPY_GUARD, _COPY_GUARD_PATCHED, 1)

    for entry in sorted(source_root.iterdir(), key=lambda item: item.name):
        if entry.name == source_runner.name:
            continue
        (output_root / entry.name).symlink_to(
            entry, target_is_directory=entry.is_dir()
        )

    output_runner = output_root / source_runner.name
    output_runner.write_text(patched, encoding="utf-8")
    output_runner.chmod(source_runner.stat().st_mode & 0o7777)
    return output_runner


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    print(prepare_runner(args.runner, args.output_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
