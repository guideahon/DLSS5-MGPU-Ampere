#!/usr/bin/env python3
"""Report the Steam prerequisites for an isolated Proton game probe.

This is deliberately read-only.  It never starts Steam and never emits the
contents of loginusers.vdf; it only records whether a usable-looking session
file and the requested app manifest are present.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def steam_roots(home: Path) -> list[Path]:
    candidates = [
        home / ".steam/steam",
        home / ".steam/root",
        home / ".local/share/Steam",
        home / ".var/app/com.valvesoftware.Steam/data/Steam",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
    ]
    return list(dict.fromkeys(path for path in candidates if path.exists()))


def library_roots(home: Path) -> list[Path]:
    values = [value for value in os.environ.get(
        "MGPU_STEAM_LIBRARY_ROOTS", "").split(os.pathsep) if value]
    roots = [Path(value).expanduser() for value in values]
    roots.extend(steam_roots(home))
    mount_base = Path("/media") / home.name
    if mount_base.is_dir():
        roots.extend(sorted(mount_base.glob("*/SteamLibrary")))
    run_mount_base = Path("/run/media") / home.name
    if run_mount_base.is_dir():
        roots.extend(sorted(run_mount_base.glob("*/SteamLibrary")))
    return list(dict.fromkeys(path for path in roots if path.exists()))


def login_summary(roots: list[Path]) -> dict[str, object]:
    files = [root / "config/loginusers.vdf" for root in roots
             if (root / "config/loginusers.vdf").is_file()]
    account_count = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        # Account IDs are keys in loginusers.vdf.  Do not return usernames or
        # account names; the count is enough to classify the local state.
        account_count += len(re.findall(r'"[0-9]{10,}"\s*\{', text))
    return {
        "files": [str(path) for path in files],
        "present": bool(files),
        "account_count": account_count,
        "authenticated_looking": bool(files and account_count),
    }


def client_running() -> bool:
    for name in ("steam", "steamwebhelper"):
        result = subprocess.run(
            ["pgrep", "-x", name], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, check=False)
        if result.returncode == 0:
            return True
    return False


def manifest_summary(roots: list[Path], appid: str | None) -> dict[str, object]:
    manifests: list[str] = []
    if appid and re.fullmatch(r"[0-9]+", appid):
        name = f"appmanifest_{appid}.acf"
        for root in roots:
            candidate = root / "steamapps" / name
            if candidate.is_file():
                manifests.append(str(candidate))
    return {"appid": appid or None, "files": manifests,
            "present": bool(manifests)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--appid", default=os.environ.get("MGPU_STEAM_APPID") or
                        os.environ.get("SteamAppId"))
    parser.add_argument("--home", type=Path, default=Path.home())
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    home = args.home.expanduser().resolve()
    roots = library_roots(home)
    login = login_summary(roots)
    manifest = manifest_summary(roots, args.appid)
    running = client_running()
    report = {
        "home": str(home),
        "roots": [str(root) for root in roots],
        "client_running": running,
        "login": login,
        "manifest": manifest,
        "usable_authenticated_context": bool(
            running and login["authenticated_looking"]),
    }
    if args.json:
        print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    else:
        print("client_running=%s authenticated_looking=%s manifest_present=%s" %
              (running, login["authenticated_looking"], manifest["present"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
