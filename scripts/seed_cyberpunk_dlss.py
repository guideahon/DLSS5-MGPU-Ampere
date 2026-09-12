#!/usr/bin/env python3
"""Seed a minimal Cyberpunk graphics profile inside an isolated Proton prefix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def cyberpunk_dlss_settings() -> dict:
    """Return the smallest settings profile that selects DLSS explicitly."""
    return {
        "version": 110,
        "data": [
            {
                "group_name": "/graphics/dlss",
                "options": [
                    {
                        "name": "DLSSFrameGen",
                        "type": "bool",
                        "value": False,
                        "default_value": False,
                    },
                    {
                        "name": "DLSS",
                        "type": "string_list",
                        "is_dynamic": False,
                        "value": "Quality",
                        "index": 2,
                        "default_index": 1,
                        "values": [
                            "Off", "Auto", "Quality", "Balanced",
                            "Performance", "Ultra Performance",
                        ],
                    },
                    {
                        "name": "DLSS_Sharpness",
                        "type": "float",
                        "value": 0.5,
                        "default_value": 0.5,
                        "min_value": 0.0,
                        "max_value": 1.0,
                        "step_value": 0.05,
                    },
                    {
                        "name": "DLSS_D",
                        "type": "bool",
                        "value": False,
                        "default_value": False,
                    },
                    {
                        "name": "DLAA",
                        "type": "bool",
                        "value": False,
                        "default_value": False,
                    },
                ],
            },
            {
                "group_name": "/graphics/dynamicresolution",
                "options": [
                    {
                        "name": "DynamicResolutionScaling",
                        "type": "bool",
                        "value": False,
                        "default_value": False,
                    },
                    {
                        "name": "FSR2",
                        "type": "string_list",
                        "is_dynamic": False,
                        "value": "Off",
                        "index": 0,
                        "default_index": 0,
                        "values": [
                            "Off", "Auto", "Quality", "Balanced",
                            "Performance", "Ultra Performance",
                        ],
                    },
                    {
                        "name": "XESS",
                        "type": "string_list",
                        "is_dynamic": False,
                        "value": "Off",
                        "index": 0,
                        "default_index": 0,
                        "values": [
                            "Off", "Auto", "Ultra Quality", "Quality",
                            "Balanced", "Performance",
                        ],
                    },
                ],
            },
        ],
    }


def write_cyberpunk_dlss_profile(prefix_root: Path) -> Path:
    """Write the profile below Proton's ``pfx`` without touching game files."""
    target = (prefix_root.expanduser().resolve() / "pfx/drive_c/users/steamuser"
              / "AppData/Local/CD Projekt Red/Cyberpunk 2077/UserSettings.json")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(cyberpunk_dlss_settings(), indent=2) + "\n",
                      encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", required=True,
                        help="STEAM_COMPAT_DATA_PATH aislado")
    args = parser.parse_args()
    print(write_cyberpunk_dlss_profile(Path(args.prefix)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
