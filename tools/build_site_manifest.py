#!/usr/bin/env python3
"""Index firmware files for the GitHub Pages site (site/program.html).

Scans a directory of firmware files named like the build outputs
  <env>-<version>-merged.bin   ESP32 full image (browser install)
  <env>-<version>.uf2          nRF52 / RP2040 (drag and drop)
  <env>-<version>.hex / .bin   STM32
for personal relay and K.I.S.S. Messenger envs, and writes:
  index.json                   list read by program.js
  <env>.manifest.json          ESP Web Tools manifest, one per ESP32 build

Usage: build_site_manifest.py <firmware_dir>
"""
import json
import re
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

KINDS = {"personal_relay": ("relay", "Personal Relay"), "kiss_messenger": ("kiss", "K.I.S.S. Messenger")}
NAME_RE = re.compile(r"^(?P<env>.+?_(?P<kind>personal_relay|kiss_messenger))-(?P<ver>.+?)(?P<merged>-merged)?\.(?P<ext>bin|uf2|hex)$")

# ESP image header chip_id -> ESP Web Tools chipFamily
ESP_CHIPS = {0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3", 12: "ESP32-C2", 13: "ESP32-C6", 16: "ESP32-H2"}
UF2_FAMILIES = {0xADA52840: "nrf52", 0xE48BFF56: "rp2040", 0xE48BFF59: "rp2040"}   # nRF52840, RP2040, RP2350


def esp_chip(path):
    """Chip family from the bootloader image header in a merged image (bootloader at 0x0 or 0x1000)."""
    data = path.read_bytes()[:0x1010]
    for off in (0x0, 0x1000):
        if len(data) > off + 14 and data[off] == 0xE9:
            chip_id = struct.unpack_from("<H", data, off + 12)[0]
            return ESP_CHIPS.get(chip_id)
    return None


def uf2_platform(path):
    with path.open("rb") as f:
        block = f.read(32)
    if len(block) < 32 or block[:8] != b"UF2\nWQ]\x9e":
        return None
    flags, family = struct.unpack_from("<I", block, 8)[0], struct.unpack_from("<I", block, 28)[0]
    if flags & 0x2000:
        return UF2_FAMILIES.get(family)
    return None


def pretty_board(env, kind_suffix):
    return env[: -len(kind_suffix) - 1].replace("_", " ")


def main(fw_dir):
    fw_dir = Path(fw_dir)
    builds = {}
    for p in sorted(fw_dir.iterdir()):
        m = NAME_RE.match(p.name)
        if not m:
            continue
        b = builds.setdefault(m["env"], {"kind": m["kind"], "version": m["ver"], "files": {}})
        if m["merged"]:
            b["files"]["merged"] = p.name
        else:
            b["files"][m["ext"]] = p.name

    firmwares = []
    for env, b in sorted(builds.items()):
        kind, kind_label = KINDS[b["kind"]]
        files = b["files"]
        entry = {"id": env, "kind": kind, "board": pretty_board(env, b["kind"]), "version": b["version"], "files": files}
        if "merged" in files:
            chip = esp_chip(fw_dir / files["merged"])
            if chip is None:
                print(f"skip {env}: unknown ESP chip", file=sys.stderr)
                continue
            entry.update(platform="esp32", chip=chip, manifest=f"{env}.manifest.json")
            manifest = {
                "name": f"{kind_label} for {entry['board']}",
                "version": b["version"],
                "new_install_prompt_erase": True,
                "builds": [{"chipFamily": chip, "parts": [{"path": files["merged"], "offset": 0}]}],
            }
            (fw_dir / entry["manifest"]).write_text(json.dumps(manifest, indent=2))
        elif "uf2" in files:
            plat = uf2_platform(fw_dir / files["uf2"])
            if plat is None:
                print(f"skip {env}: unknown UF2 family", file=sys.stderr)
                continue
            entry["platform"] = plat
        elif "hex" in files:
            entry["platform"] = "stm32"
        else:
            print(f"skip {env}: no installable file", file=sys.stderr)
            continue
        firmwares.append(entry)

    index = {"generated": datetime.now(timezone.utc).isoformat(timespec="seconds"), "firmwares": firmwares}
    (fw_dir / "index.json").write_text(json.dumps(index, indent=2))
    counts = {}
    for f in firmwares:
        counts[f["kind"]] = counts.get(f["kind"], 0) + 1
    print(f"indexed {len(firmwares)} firmwares: {counts}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
