#!/usr/bin/env python3
"""Generate WLED effect metadata and remote.json mappings.

The generator reads the local official WLED checkout and emits:
  - include/generated/wled_effects.h for the touchscreen UI
  - remote.json for WLED's ESP-NOW JSON remote handler
"""

import argparse
import json
import re
from pathlib import Path


DEFAULT_WLED = Path("/path-to-wled")
MAX_EFFECT_ID = 255
SKIP_EFFECT_DEFINES: set[str] = set()

PRESET_FIRST_REMOTE = 23
SETTING_FIRST_REMOTE = 36
COLOR_FIRST_REMOTE = 51
EFFECT_FIRST_REMOTE = 61

SETTING_COMMANDS = [
    ("Palette -", {"seg": {"pal": "w~-1"}}),
    ("Palette +", {"seg": {"pal": "w~1"}}),
    ("Speed -", {"seg": {"sx": "~-16"}}),
    ("Speed +", {"seg": {"sx": "~16"}}),
    ("Intensity -", {"seg": {"ix": "~-16"}}),
    ("Intensity +", {"seg": {"ix": "~16"}}),
    ("Custom 1 -", {"seg": {"c1": "~-16"}}),
    ("Custom 1 +", {"seg": {"c1": "~16"}}),
    ("Custom 2 -", {"seg": {"c2": "~-16"}}),
    ("Custom 2 +", {"seg": {"c2": "~16"}}),
    ("Custom 3 -", {"seg": {"c3": "~-4"}}),
    ("Custom 3 +", {"seg": {"c3": "~4"}}),
    ("Option 1", {"seg": {"o1": "t"}}),
    ("Option 2", {"seg": {"o2": "t"}}),
    ("Option 3", {"seg": {"o3": "t"}}),
]

COLORS = [
    ("Warm", [255, 180, 90], 0xFFB45A, False),
    ("White", [255, 255, 255], 0xFFFFFF, True),
    ("Red", [255, 32, 32], 0xFF2020, False),
    ("Orange", [255, 96, 0], 0xFF6000, False),
    ("Yellow", [255, 214, 0], 0xFFD600, True),
    ("Green", [0, 190, 80], 0x00BE50, False),
    ("Cyan", [0, 210, 255], 0x00D2FF, True),
    ("Blue", [0, 88, 255], 0x0058FF, False),
    ("Purple", [128, 64, 255], 0x8040FF, False),
    ("Pink", [255, 48, 160], 0xFF30A0, False),
]

DEFAULT_SLIDERS_AC = ["Effect speed", "Effect intensity", "", "", ""]
DEFAULT_SLIDERS_SR = ["Effect speed", "Effect intensity", "Custom 1", "Custom 2", "Custom 3"]
DEFAULT_OPTIONS = ["Option 1", "Option 2", "Option 3"]


def c_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def read_effect_defines(fx_h: Path) -> dict[str, int]:
    defines: dict[str, int] = {}
    pattern = re.compile(r"^#define\s+(FX_MODE_[A-Z0-9_]+)\s+(\d+)\b")
    for line in fx_h.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            defines[match.group(1)] = int(match.group(2))
    return defines


def read_metadata_strings(fx_cpp: Path) -> dict[str, str]:
    text = fx_cpp.read_text(encoding="utf-8")
    pattern = re.compile(
        r"static\s+const\s+char\s+(_data_[A-Za-z0-9_]+)\[\]\s+PROGMEM\s*=\s*\"((?:\\.|[^\"])*)\";",
        re.MULTILINE,
    )
    return {name: bytes(value, "utf-8").decode("unicode_escape") for name, value in pattern.findall(text)}


def read_effect_order(fx_cpp: Path) -> list[tuple[str, str]]:
    order: list[tuple[str, str]] = []
    pattern = re.compile(r"addEffect\(\s*(FX_MODE_[A-Z0-9_]+)\s*,\s*&[A-Za-z0-9_]+\s*,\s*(_data_[A-Za-z0-9_]+)\s*\)")
    for line in fx_cpp.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("//"):
            continue
        match = pattern.search(stripped)
        if match:
            order.append((match.group(1), match.group(2)))
    return order


def clean_label(value: str, fallback: str) -> str:
    value = value.strip()
    if not value or value == "!":
        return fallback
    return value


def effect_flags(metadata: str) -> str:
    if "@" not in metadata:
        return "1"

    effect_parts = metadata.split("@", 1)[1].split(";")
    if len(effect_parts) < 4 or not effect_parts[3]:
        return "1"
    return effect_parts[3]


def is_2d_only(metadata: str) -> bool:
    flags = effect_flags(metadata)
    return "2" in flags and "1" not in flags


def parse_controls(effect_id: int, metadata: str) -> tuple[int, str]:
    controls = ""
    if "@" in metadata:
        controls = metadata.split("@", 1)[1]
    effect_parts = controls.split(";") if controls else []

    mask = 0
    labels: list[str] = []

    if not controls:
        defaults = DEFAULT_SLIDERS_AC if effect_id < 128 else DEFAULT_SLIDERS_SR
        slider_values = defaults
        option_values: list[str] = []
        palette_values = ["!"]
    else:
        slider_values = effect_parts[0].split(",") if len(effect_parts) > 0 and effect_parts[0] else []
        option_values = slider_values[5:8] if len(slider_values) > 5 else []
        slider_values = slider_values[:5]
        palette_values = effect_parts[2].split(",") if len(effect_parts) > 2 and effect_parts[2] else []

    slider_fallbacks = DEFAULT_SLIDERS_SR
    for index, raw in enumerate(slider_values[:5]):
        if raw == "":
            continue
        mask |= 1 << index
        labels.append(clean_label(raw, slider_fallbacks[index]))

    for index, raw in enumerate(option_values[:3]):
        if raw == "":
            continue
        mask |= 1 << (5 + index)
        labels.append(clean_label(raw, DEFAULT_OPTIONS[index]))

    if palette_values:
        palette = palette_values[0].strip()
        if palette and not palette.lstrip("-").isdigit():
            mask |= 1 << 8
            labels.append("Palette")

    return mask, "|".join(labels)


def parse_effects(wled_root: Path, include_2d_only: bool = False) -> list[dict]:
    fx_h = wled_root / "wled00" / "FX.h"
    fx_cpp = wled_root / "wled00" / "FX.cpp"
    defines = read_effect_defines(fx_h)
    metadata = read_metadata_strings(fx_cpp)
    by_id: dict[int, dict] = {
        0: {
            "id": 0,
            "name": "Solid",
            "mask": 0,
            "labels": "",
        }
    }

    for define, data_name in read_effect_order(fx_cpp):
        if define in SKIP_EFFECT_DEFINES:
            continue

        effect_id = defines.get(define)
        raw = metadata.get(data_name)
        if effect_id is None or raw is None or effect_id > MAX_EFFECT_ID:
            continue
        if not include_2d_only and is_2d_only(raw):
            continue

        name = raw.split("@", 1)[0].strip()
        if name in {"RSVD", "Reserved"}:
            continue
        mask, labels = parse_controls(effect_id, raw)
        by_id[effect_id] = {
            "id": effect_id,
            "name": name,
            "mask": mask,
            "labels": labels,
        }

    effects = [by_id[i] for i in sorted(by_id) if i <= MAX_EFFECT_ID]
    effects.sort(key=lambda effect: effect["name"].casefold())
    for index, effect in enumerate(effects):
        button = EFFECT_FIRST_REMOTE + index
        if button > 255:
            raise RuntimeError("effect button range exceeded")
        effect["button"] = button
    return effects


def write_header(path: Path, effects: list[dict]) -> None:
    lines = [
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "struct WledEffectInfo {",
        "  uint8_t id;",
        "  uint8_t button;",
        "  uint16_t controls;",
        "  const char* name;",
        "  const char* labels;",
        "};",
        "",
        "constexpr uint16_t kFxControlSpeed = 1 << 0;",
        "constexpr uint16_t kFxControlIntensity = 1 << 1;",
        "constexpr uint16_t kFxControlCustom1 = 1 << 2;",
        "constexpr uint16_t kFxControlCustom2 = 1 << 3;",
        "constexpr uint16_t kFxControlCustom3 = 1 << 4;",
        "constexpr uint16_t kFxControlOption1 = 1 << 5;",
        "constexpr uint16_t kFxControlOption2 = 1 << 6;",
        "constexpr uint16_t kFxControlOption3 = 1 << 7;",
        "constexpr uint16_t kFxControlPalette = 1 << 8;",
        "",
        f"constexpr uint8_t kWledEffectFirstButton = {EFFECT_FIRST_REMOTE};",
        f"constexpr uint8_t kWledEffectLastButton = {effects[-1]['button']};",
        f"constexpr uint8_t kWledEffectMaxId = {max(effect['id'] for effect in effects)};",
        "",
        "constexpr WledEffectInfo kWledEffects[] = {",
    ]
    for effect in effects:
        lines.append(
            f'    {{{effect["id"]}, {effect["button"]}, {effect["mask"]}, "{c_escape(effect["name"])}", "{c_escape(effect["labels"])}"}},'
        )
    lines.extend(
        [
            "};",
            "",
            "constexpr size_t kWledEffectCount = sizeof(kWledEffects) / sizeof(kWledEffects[0]);",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def write_remote_json(path: Path, effects: list[dict]) -> None:
    data: dict[str, dict] = {}
    for preset in range(8, 21):
        data[str(PRESET_FIRST_REMOTE + preset - 8)] = {
            "cmd": {"ps": preset},
            "label": f"Preset {preset}",
        }

    for offset, (label, command) in enumerate(SETTING_COMMANDS):
        data[str(SETTING_FIRST_REMOTE + offset)] = {"cmd": command, "label": label}

    for offset, (label, rgb, _, _) in enumerate(COLORS):
        data[str(COLOR_FIRST_REMOTE + offset)] = {
            "cmd": {"on": True, "seg": {"fx": 0, "col": [rgb, [], []]}},
            "label": label,
        }

    for effect in effects:
        data[str(effect["button"])] = {
            "cmd": {"on": True, "seg": {"fx": effect["id"]}},
            "label": effect["name"],
        }

    lines = ["{"]
    items = list(data.items())
    for index, (key, value) in enumerate(items):
        comma = "," if index < len(items) - 1 else ""
        lines.append(f"  {json.dumps(key)}: {json.dumps(value, separators=(', ', ': '))}{comma}")
    lines.append("}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wled", type=Path, default=DEFAULT_WLED)
    parser.add_argument("--header", type=Path, default=Path("include/generated/wled_effects.h"))
    parser.add_argument("--remote", type=Path, default=Path("remote.json"))
    parser.add_argument(
        "--include-2d-only",
        action="store_true",
        help="include effects WLED hides unless a 2D matrix segment is available",
    )
    args = parser.parse_args()

    effects = parse_effects(args.wled, include_2d_only=args.include_2d_only)
    write_header(args.header, effects)
    write_remote_json(args.remote, effects)
    print(f"Generated {len(effects)} effects through WLED effect ID {max(effect['id'] for effect in effects)}.")


if __name__ == "__main__":
    main()
