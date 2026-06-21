from pathlib import Path
import struct
import zlib

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
logo_source = project_dir / "wled.png"
qr_source = project_dir / "qr.png"
target = project_dir / "include" / "generated" / "wled_logo_png.h"


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


def read_png_rgba(source, label):
    if not source.exists():
        raise FileNotFoundError(f"{label} PNG not found: {source}")

    png = source.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{label} must be a PNG file: {source}")

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
                raise ValueError(f"{label} PNG must be non-interlaced with standard compression/filtering")
        elif chunk_type == b"PLTE":
            palette = [tuple(chunk_data[i:i + 3]) for i in range(0, len(chunk_data), 3)]
        elif chunk_type == b"tRNS":
            palette_alpha = list(chunk_data)
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width <= 0 or height <= 0 or not idat:
        raise ValueError(f"{label} PNG is missing image data: {source}")
    if bit_depth != 8 or color_type not in (2, 3, 6):
        raise ValueError(f"{label} PNG must be 8-bit RGB, RGBA, or indexed color")
    if color_type == 3 and not palette:
        raise ValueError(f"Indexed {label} PNG is missing a palette")

    channels = {2: 3, 3: 1, 6: 4}[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    rows = []
    read_pos = 0
    previous = [0] * stride

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
                raise ValueError(f"Unsupported {label} PNG filter type: {filter_type}")

        rows.append(row)
        previous = row

    rgba_pixels = []
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

            rgba_pixels.append((r, g, b, alpha))

    return width, height, rgba_pixels


def flatten_to_rgb565(src, bg):
    bg_r, bg_g, bg_b = bg
    dst = []
    for r, g, b, alpha in src:
        if alpha < 255:
            inverse = 255 - alpha
            r = (r * alpha + bg_r * inverse + 127) // 255
            g = (g * alpha + bg_g * inverse + 127) // 255
            b = (b * alpha + bg_b * inverse + 127) // 255
        dst.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return dst


def resize_nearest(src, src_w, src_h, dst_w, dst_h):
    dst = []
    for y in range(dst_h):
        src_y = min(src_h - 1, (y * src_h + dst_h // 2) // dst_h)
        for x in range(dst_w):
            src_x = min(src_w - 1, (x * src_w + dst_w // 2) // dst_w)
            dst.append(src[src_y * src_w + src_x])
    return dst


def format_pixels(values):
    formatted = [f"0x{pixel:04X}" for pixel in values]
    result = []
    for i in range(0, len(formatted), 10):
        result.append("  " + ", ".join(formatted[i:i + 10]) + ",")
    return chr(10).join(result)


logo_width, logo_height, logo_rgba_pixels = read_png_rgba(logo_source, "Startup logo")
logo_pixels = flatten_to_rgb565(logo_rgba_pixels, (0x00, 0x00, 0x00))

header_width = 96
header_height = max(1, round(logo_height * header_width / logo_width))
header_rgba_pixels = resize_nearest(logo_rgba_pixels, logo_width, logo_height, header_width, header_height)
header_pixels = flatten_to_rgb565(header_rgba_pixels, (0x11, 0x18, 0x21))

qr_width, qr_height, qr_rgba_pixels = read_png_rgba(qr_source, "Help QR")
qr_pixels = flatten_to_rgb565(qr_rgba_pixels, (0xFF, 0xFF, 0xFF))

target.parent.mkdir(parents=True, exist_ok=True)

content = f"""#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ARDUINO)
#include <pgmspace.h>
#endif

#ifndef PROGMEM
#define PROGMEM
#endif

constexpr uint32_t kWledLogoWidth = {logo_width};
constexpr uint32_t kWledLogoHeight = {logo_height};
constexpr size_t kWledLogoPixelCount = {len(logo_pixels)};
constexpr uint32_t kWledLogoHeaderWidth = {header_width};
constexpr uint32_t kWledLogoHeaderHeight = {header_height};
constexpr size_t kWledLogoHeaderPixelCount = {len(header_pixels)};
constexpr uint32_t kHelpQrWidth = {qr_width};
constexpr uint32_t kHelpQrHeight = {qr_height};
constexpr size_t kHelpQrPixelCount = {len(qr_pixels)};

const uint16_t kWledLogoPixels[] PROGMEM = {{
{format_pixels(logo_pixels)}
}};

const uint16_t kWledLogoHeaderPixels[] PROGMEM = {{
{format_pixels(header_pixels)}
}};

const uint16_t kHelpQrPixels[] PROGMEM = {{
{format_pixels(qr_pixels)}
}};
"""

if not target.exists() or target.read_text() != content:
    target.write_text(content)
