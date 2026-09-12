#!/usr/bin/env python3
"""Seed No Man's Sky DLSS settings inside an isolated Proton prefix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def set_property(root: ET.Element, name: str, value: str) -> None:
    for element in root.iter("Property"):
        if element.get("name") == name:
            element.set("value", value)
            return
    ET.SubElement(root, "Property", name=name, value=value)


def seed_file(path: Path) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    existed = path.is_file()
    if existed:
        try:
            root = ET.parse(path).getroot()
        except (ET.ParseError, OSError) as exc:
            raise SystemExit(f"no se pudo leer {path}: {exc}") from exc
    else:
        root = ET.Element("Data", template="TkGraphicsSettings")
        set_property(root, "Version", "2")
        set_property(root, "FullScreen", "false")
        set_property(root, "Borderless", "true")
        set_property(root, "UseScreenResolution", "true")
        set_property(root, "ResolutionWidth", "1280")
        set_property(root, "ResolutionHeight", "720")
    set_property(root, "DLSSQuality", "MaxQuality")
    set_property(root, "DLSSFrameGeneration", "Off")
    set_property(root, "FFXSRQuality", "Off")
    set_property(root, "FFXSR2Quality", "Off")
    set_property(root, "XESSQuality", "Off")
    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="utf-8", xml_declaration=True)
    return existed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    prefix = args.prefix.expanduser().resolve()
    user_root = prefix / "pfx" / "drive_c" / "users" / "steamuser"
    candidates = (
        user_root / "AppData" / "Roaming" / "HelloGames" / "NMS" /
        "TKGRAPHICSSETTINGS.MXML",
        user_root / "AppData" / "Local" / "HelloGames" / "NMS" /
        "TKGRAPHICSSETTINGS.MXML",
    )
    seeded = []
    for path in candidates:
        seeded.append({"path": str(path), "existed": seed_file(path)})
    report = {"prefix": str(prefix), "settings": seeded,
              "dlss_quality": "MaxQuality", "frame_generation": "Off"}
    if args.json:
        print(json.dumps(report, ensure_ascii=False))
    else:
        for item in seeded:
            print(f"seeded={item['path']} existed={item['existed']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
