from pathlib import Path
import struct
import zlib

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
source = project_dir / "wled.png"
target = project_dir / "include" / "generated" / "wled_logo_png.h"

if not source.exists():
    raise FileNotFoundError(f"Startup logo not found: {source}")

png = source.read_bytes()
if png[:8] != b"\x89PNG\r\n\x1a\n":
    raise ValueError(f"Startup logo must be a PNG file: {source}")

pos = 8
width = 0
height = 0
bit_depth = 0
color_type = 0
palette = []
palette_alpha = []
idat = bytearray()

while pos < len(png):
    length = struct.unpack(">I", png[pos:pos + 4])[0]
    chunk_type = png[pos + 4:pos + 8]
    chunk_data = png[pos + 8:pos + 8 + length]
    pos += length + 12

    if chunk_type == b"IHDR":
        width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(">IIBBBBB", chunk_data)
        if compression != 0 or filter_method != 0 or interlace != 0:
            raise ValueError("Startup logo PNG must be non-interlaced with standard compression/filtering")
    elif chunk_type == b"PLTE":
        palette = [tuple(chunk_data[i:i + 3]) for i in range(0, len(chunk_data), 3)]
    elif chunk_type == b"tRNS":
        palette_alpha = list(chunk_data)
    elif chunk_type == b"IDAT":
        idat.extend(chunk_data)
    elif chunk_type == b"IEND":
        break

if width <= 0 or height <= 0 or not idat:
    raise ValueError(f"Startup logo PNG is missing image data: {source}")
if bit_depth != 8 or color_type not in (2, 3, 6):
    raise ValueError("Startup logo PNG must be 8-bit RGB, RGBA, or indexed color")
if color_type == 3 and not palette:
    raise ValueError("Indexed startup logo PNG is missing a palette")

channels = {2: 3, 3: 1, 6: 4}[color_type]
raw = zlib.decompress(bytes(idat))
stride = width * channels
rows = []
read_pos = 0
previous = [0] * stride

def paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c

for _ in range(height):
    filter_type = raw[read_pos]
    read_pos += 1
    row = list(raw[read_pos:read_pos + stride])
    read_pos += stride

    for i, value in enumerate(row):
        left = row[i - channels] if i >= channels else 0
        up = previous[i]
        upper_left = previous[i - channels] if i >= channels else 0

        if filter_type == 1:
            row[i] = (value + left) & 0xFF
        elif filter_type == 2:
            row[i] = (value + up) & 0xFF
        elif filter_type == 3:
            row[i] = (value + ((left + up) >> 1)) & 0xFF
        elif filter_type == 4:
            row[i] = (value + paeth(left, up, upper_left)) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"Unsupported PNG filter type: {filter_type}")

    rows.append(row)
    previous = row

pixels = []

for row in rows:
    for x in range(width):
        if color_type == 3:
            index = row[x]
            r, g, b = palette[index]
            alpha = palette_alpha[index] if index < len(palette_alpha) else 255
        elif color_type == 2:
            offset = x * 3
            r, g, b = row[offset:offset + 3]
            alpha = 255
        else:
            offset = x * 4
            r, g, b, alpha = row[offset:offset + 4]

        if alpha < 255:
            r = (r * alpha + 127) // 255
            g = (g * alpha + 127) // 255
            b = (b * alpha + 127) // 255

        pixels.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

target.parent.mkdir(parents=True, exist_ok=True)

pixel_values = [f"0x{pixel:04X}" for pixel in pixels]
lines = []
for i in range(0, len(pixel_values), 10):
    lines.append("  " + ", ".join(pixel_values[i:i + 10]) + ",")

content = f"""#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO)
#include <pgmspace.h>
#endif

#ifndef PROGMEM
#define PROGMEM
#endif

constexpr uint32_t kWledLogoWidth = {width};
constexpr uint32_t kWledLogoHeight = {height};
constexpr size_t kWledLogoPixelCount = {len(pixels)};

const uint16_t kWledLogoPixels[] PROGMEM = {{
{chr(10).join(lines)}
}};
"""

if not target.exists() or target.read_text() != content:
    target.write_text(content)
