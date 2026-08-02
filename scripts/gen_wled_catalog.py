#!/usr/bin/env python3
"""Generate include/generated/wled_catalog.h from the WLED firmware sources.

Extracts the effect catalog (names, slider labels, 1D/2D/audio/palette flags) and
the palette catalog (names plus 8 sampled RGB preview stops) so the remote ships
them baked in instead of repeatedly fetching multi-KB lists over Wi-Fi.

Usage: python3 scripts/gen_wled_catalog.py [--wled /path/to/WLED] [--out header]
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# HTML color values for the CRGB:: names used in WLED's palettes.cpp (FastLED HTMLColorCode).
CRGB_NAMES = {
    "Aqua": 0x00FFFF, "Aquamarine": 0x7FFFD4, "Black": 0x000000, "Blue": 0x0000FF,
    "CadetBlue": 0x5F9EA0, "CornflowerBlue": 0x6495ED, "DarkBlue": 0x00008B,
    "DarkCyan": 0x008B8B, "DarkGreen": 0x006400, "DarkOliveGreen": 0x556B2F,
    "DarkRed": 0x8B0000, "ForestGreen": 0x228B22, "Green": 0x008000,
    "LawnGreen": 0x7CFC00, "LightBlue": 0xADD8E6, "LightGreen": 0x90EE90,
    "LightSkyBlue": 0x87CEFA, "LimeGreen": 0x32CD32, "Maroon": 0x800000,
    "MediumAquamarine": 0x66CDAA, "MediumBlue": 0x0000CD, "MidnightBlue": 0x191970,
    "Navy": 0x000080, "OliveDrab": 0x6B8E23, "Orange": 0xFFA500, "Red": 0xFF0000,
    "SeaGreen": 0x2E8B57, "SkyBlue": 0x87CEEB, "Teal": 0x008080, "White": 0xFFFFFF,
    "YellowGreen": 0x9ACD32,
}

DEFAULT_SLIDER_NAMES = ["Effect speed", "Effect intensity"]
PREVIEW_STOPS = 8

FLAG_1D = 0x01
FLAG_2D = 0x02
FLAG_AUDIO_VOLUME = 0x04
FLAG_AUDIO_FREQ = 0x08
FLAG_PALETTE = 0x10


def parse_effects(wled: Path):
    fx_h = (wled / "wled00" / "FX.h").read_text()
    fx_cpp = (wled / "wled00" / "FX.cpp").read_text()

    ids = {m.group(1): int(m.group(2))
           for m in re.finditer(r"#define\s+(FX_MODE_\w+)\s+(\d+)", fx_h)}
    mode_count = int(re.search(r"#define\s+MODE_COUNT\s+(\d+)", fx_h).group(1))

    data = {m.group(1): m.group(2)
            for m in re.finditer(r'const char (_data_FX_MODE_\w+)\[\]\s+PROGMEM\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', fx_cpp)}

    live_lines = "\n".join(l for l in fx_cpp.splitlines() if not l.lstrip().startswith("//"))
    registered = {}  # id -> metadata string
    for m in re.finditer(r"addEffect\((FX_MODE_\w+),\s*&\w+,\s*(_data_FX_MODE_\w+)\)", live_lines):
        fx_id = ids.get(m.group(1))
        meta = data.get(m.group(2))
        if fx_id is None or meta is None:
            print(f"warning: unresolved addEffect: {m.group(0)}", file=sys.stderr)
            continue
        registered[fx_id] = meta
    # FX_MODE_STATIC (id 0) is seeded via _modeData.push_back, not addEffect
    if 0 not in registered and "_data_FX_MODE_STATIC" in data:
        registered[0] = data["_data_FX_MODE_STATIC"]

    effects = []
    for fx_id in range(mode_count):
        meta = registered.get(fx_id)
        if meta is None:
            effects.append(None)
            continue
        # <name>@<sliders>;<colors>;<palette>;<flags>;<defaults>
        parts = meta.split(";")
        head = parts[0]
        name, _, slider_part = head.partition("@")
        has_meta = "@" in head or len(parts) > 1

        slots = slider_part.split(",") if slider_part else []
        labels = []
        for i in range(8):
            raw = slots[i] if i < len(slots) else ""
            if not has_meta and i < 2:
                raw = "!"  # no metadata at all: legacy default speed/intensity sliders
            if raw == "!":
                raw = DEFAULT_SLIDER_NAMES[i] if i < 2 else ""
            labels.append(raw)

        palette_part = parts[2] if len(parts) > 2 else ""
        flags_part = parts[3] if len(parts) > 3 else ""

        flags = 0
        if palette_part.strip():
            flags |= FLAG_PALETTE
        if "2" in flags_part:
            flags |= FLAG_2D
        if "1" in flags_part or not flags_part.strip("0"):
            flags |= FLAG_1D  # '1' present, or flags empty/only '0' (default 1D)
        if "v" in flags_part:
            flags |= FLAG_AUDIO_VOLUME
        if "f" in flags_part:
            flags |= FLAG_AUDIO_FREQ

        effects.append({"name": name, "sliders": "|".join(labels).rstrip("|"), "flags": flags})
    return effects


def parse_palette_names(wled: Path):
    text = (wled / "wled00" / "FX_fcn.cpp").read_text()
    m = re.search(r'JSON_palette_names\[\]\s+PROGMEM\s*=\s*R"=====\((.*?)\)====="', text, re.S)
    return json.loads(m.group(1))


def sample_gradient(anchors, count=PREVIEW_STOPS):
    """anchors: list of (index0..255, r, g, b) -> `count` evenly spaced RGB stops."""
    stops = []
    for i in range(count):
        pos = i * 255 // (count - 1)
        prev = anchors[0]
        nxt = anchors[-1]
        for a in anchors:
            if a[0] <= pos:
                prev = a
            if a[0] >= pos:
                nxt = a
                break
        if nxt[0] == prev[0]:
            frac = 0.0
        else:
            frac = (pos - prev[0]) / (nxt[0] - prev[0])
        stops.append(tuple(round(prev[c] + (nxt[c] - prev[c]) * frac) for c in (1, 2, 3)))
    return stops


def parse_color_token(tok):
    tok = tok.strip()
    if tok.startswith("CRGB::"):
        name = tok[6:]
        if name not in CRGB_NAMES:
            raise ValueError(f"unknown CRGB color: {name}")
        return CRGB_NAMES[name]
    return int(tok, 0)


def parse_palettes(wled: Path):
    text = (wled / "wled00" / "palettes.cpp").read_text()

    # FastLED-style 16-entry palettes (ids 6-12)
    pal16 = {}
    for m in re.finditer(r"const TProgmemRGBPalette16 (\w+)\s+PROGMEM\s*=\s*\{(.*?)\};", text, re.S):
        toks = [t for t in re.split(r"[,\s]+", m.group(2)) if t]
        pal16[m.group(1)] = [parse_color_token(t) for t in toks]

    m = re.search(r"const TProgmemRGBPalette16 \*const fastledPalettes\[\]\s+PROGMEM\s*=\s*\{(.*?)\};", text, re.S)
    fastled_order = re.findall(r"&(\w+)", m.group(1))

    # gradient palettes (ids 13+): flat arrays of (index, r, g, b)
    grad = {}
    for m in re.finditer(r"const (?:uint8_t|byte) (\w+_gp)\[\]\s+PROGMEM\s*=\s*\{(.*?)\};", text, re.S):
        body = re.sub(r"//[^\n]*", "", m.group(2))
        vals = [int(v) for v in re.findall(r"\d+", body)]
        grad[m.group(1)] = [tuple(vals[i:i + 4]) for i in range(0, len(vals), 4)]

    m = re.search(r"const uint8_t\* const gGradientPalettes\[\]\s+PROGMEM\s*=\s*\{(.*?)\};", text, re.S)
    grad_order = re.findall(r"(\w+_gp)", m.group(1))

    previews = []
    for name in fastled_order:
        colors = pal16[name]
        anchors = [(round(i * 255 / (len(colors) - 1)), (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)
                   for i, c in enumerate(colors)]
        previews.append(sample_gradient(anchors))
    for name in grad_order:
        previews.append(sample_gradient(grad[name]))
    return previews


def wled_version(wled: Path):
    try:
        rev = subprocess.check_output(["git", "-C", str(wled), "rev-parse", "--short", "HEAD"],
                                      text=True).strip()
    except Exception:
        rev = "unknown"
    m = re.search(r'#define\s+versionString\s+"([^"]+)"', (wled / "wled00" / "wled.h").read_text())
    return f"{m.group(1) if m else 'unknown'} ({rev})"


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wled", default=str(Path(__file__).resolve().parents[2] / "WLED-Fork" / "WLED"))
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "include" / "generated" / "wled_catalog.h"))
    args = ap.parse_args()
    wled = Path(args.wled)

    effects = parse_effects(wled)
    names = parse_palette_names(wled)
    previews = parse_palettes(wled)
    dynamic = 6  # palette ids 0-5 depend on the current segment colors
    if len(names) != dynamic + len(previews):
        print(f"warning: {len(names)} palette names but {dynamic}+{len(previews)} previews", file=sys.stderr)

    out = []
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstddef>")
    out.append("#include <cstdint>")
    out.append("")
    out.append(f"// Generated by scripts/gen_wled_catalog.py from WLED {wled_version(wled)} - do not edit.")
    out.append("")
    out.append("constexpr uint8_t kFxFlag1D = 0x01;")
    out.append("constexpr uint8_t kFxFlag2D = 0x02;")
    out.append("constexpr uint8_t kFxFlagAudioVolume = 0x04;")
    out.append("constexpr uint8_t kFxFlagAudioFreq = 0x08;")
    out.append("constexpr uint8_t kFxFlagPalette = 0x10;")
    out.append("")
    out.append("struct WledFxInfo {")
    out.append("  const char* name;     // nullptr = reserved/unused effect id")
    out.append("  const char* sliders;  // '|'-separated labels: speed|intensity|c1|c2|c3|o1|o2|o3 (empty = control unused)")
    out.append("  uint8_t flags;")
    out.append("};")
    out.append("")
    out.append("// index == WLED effect id")
    out.append("inline const WledFxInfo kWledFx[] = {")
    for fx_id, fx in enumerate(effects):
        if fx is None:
            out.append(f"    {{nullptr, nullptr, 0}},  // {fx_id}")
        else:
            out.append(f"    {{{cstr(fx['name'])}, {cstr(fx['sliders'])}, 0x{fx['flags']:02X}}},  // {fx_id}")
    out.append("};")
    out.append("constexpr size_t kWledFxCount = sizeof(kWledFx) / sizeof(kWledFx[0]);")
    out.append("")
    out.append("// index == WLED palette id")
    out.append("inline const char* const kWledPaletteNames[] = {")
    for i in range(0, len(names), 6):
        out.append("    " + ", ".join(cstr(n) for n in names[i:i + 6]) + ",")
    out.append("};")
    out.append("constexpr size_t kWledPaletteCount = sizeof(kWledPaletteNames) / sizeof(kWledPaletteNames[0]);")
    out.append(f"constexpr size_t kWledPaletteDynamicCount = {dynamic};  // ids 0-{dynamic - 1} derive from current colors at runtime")
    out.append("")
    out.append(f"// {PREVIEW_STOPS} evenly spaced RGB stops per fixed palette; index == palette id - kWledPaletteDynamicCount")
    out.append(f"inline const uint8_t kWledPaletteStops[][{PREVIEW_STOPS}][3] = {{")
    for i, stops in enumerate(previews):
        row = ", ".join(f"{{{r},{g},{b}}}" for r, g, b in stops)
        out.append(f"    {{{row}}},  // {dynamic + i} {names[dynamic + i] if dynamic + i < len(names) else ''}")
    out.append("};")
    out.append("constexpr size_t kWledPaletteStopsCount = sizeof(kWledPaletteStops) / sizeof(kWledPaletteStops[0]);")
    out.append("")

    output_path = Path(args.out)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(out))
    used = sum(1 for e in effects if e)
    print(f"wrote {args.out}: {used}/{len(effects)} effects, {len(names)} palettes ({len(previews)} previews)")


if __name__ == "__main__":
    main()
