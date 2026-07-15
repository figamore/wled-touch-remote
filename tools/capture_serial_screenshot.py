#!/usr/bin/env python3
import argparse
import struct
import sys
import zlib
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial is required. Try: python -m pip install pyserial", file=sys.stderr)
    raise


def read_line(port: serial.Serial) -> bytes:
    line = port.readline()
    if not line:
        raise TimeoutError("Timed out waiting for serial output")
    return line


def read_le16(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


def read_le32(data: bytes, offset: int) -> int:
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def variant_path(output: Path, name: str) -> Path:
    return output.with_name(f"{output.stem}-{name}{output.suffix}")


PERMUTATIONS = {
    "rgb": (0, 1, 2),
    "rbg": (0, 2, 1),
    "grb": (1, 0, 2),
    "gbr": (1, 2, 0),
    "brg": (2, 0, 1),
    "bgr": (2, 1, 0),
}


def transform_bmp(data: bytes, transform: str) -> bytes:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Screenshot is not a BMP file")

    invert = transform.endswith("-invert")
    name = transform.removesuffix("-invert")
    if name not in PERMUTATIONS:
        raise ValueError(f"Unknown color transform: {transform}")

    pixel_offset = read_le32(data, 10)
    width = read_le32(data, 18)
    height_raw = read_le32(data, 22)
    planes = read_le16(data, 26)
    bits_per_pixel = read_le16(data, 28)
    compression = read_le32(data, 30)
    if planes != 1 or bits_per_pixel != 24 or compression != 0:
        raise ValueError("Only uncompressed 24-bit BMP screenshots are supported")

    height = abs(height_raw)
    row_stride = (width * 3 + 3) & ~3
    if pixel_offset + row_stride * height > len(data):
        raise ValueError("BMP pixel data is shorter than expected")

    order = PERMUTATIONS[name]
    source = bytearray(data)
    transformed = bytearray(source)
    for row in range(height):
        row_start = pixel_offset + row * row_stride
        for x in range(width):
            i = row_start + x * 3
            old_rgb = (source[i + 2], source[i + 1], source[i])
            new_rgb = [old_rgb[index] for index in order]
            if invert:
                new_rgb = [255 - value for value in new_rgb]
            transformed[i] = new_rgb[2]
            transformed[i + 1] = new_rgb[1]
            transformed[i + 2] = new_rgb[0]
    return bytes(transformed)


def bmp_to_rgb_rows(data: bytes) -> tuple[int, int, list[bytes]]:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Screenshot is not a BMP file")

    pixel_offset = read_le32(data, 10)
    width = read_le32(data, 18)
    height_raw = read_le32(data, 22)
    planes = read_le16(data, 26)
    bits_per_pixel = read_le16(data, 28)
    compression = read_le32(data, 30)
    if planes != 1 or bits_per_pixel != 24 or compression != 0:
        raise ValueError("Only uncompressed 24-bit BMP screenshots are supported")

    top_down = height_raw & 0x80000000 != 0
    height = ((~height_raw + 1) & 0xFFFFFFFF) if top_down else height_raw
    row_stride = (width * 3 + 3) & ~3
    if pixel_offset + row_stride * height > len(data):
        raise ValueError("BMP pixel data is shorter than expected")

    rows = []
    for output_y in range(height):
        source_y = output_y if top_down else height - 1 - output_y
        row_start = pixel_offset + source_y * row_stride
        row = bytearray()
        for x in range(width):
            i = row_start + x * 3
            row.extend((data[i + 2], data[i + 1], data[i]))
        rows.append(bytes(row))
    return width, height, rows


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def bmp_to_png(data: bytes) -> bytes:
    width, height, rows = bmp_to_rgb_rows(data)
    scanlines = b"".join(b"\x00" + row for row in rows)
    return b"".join(
        (
            b"\x89PNG\r\n\x1a\n",
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
            png_chunk(b"IDAT", zlib.compress(scanlines, 9)),
            png_chunk(b"IEND", b""),
        )
    )


def image_bytes_for_output(data: bytes, output: Path) -> bytes:
    if output.suffix.lower() == ".bmp":
        return data
    return bmp_to_png(data)


def write_color_variants(data: bytes, output: Path) -> list[Path]:
    variants = []

    for name in PERMUTATIONS:
        for invert in (False, True):
            suffix = f"{name}{'-invert' if invert else ''}"
            path = variant_path(output, suffix)
            path.write_bytes(image_bytes_for_output(transform_bmp(data, suffix), path))
            variants.append(path)

    return variants


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture a CYD screenshot over serial.")
    parser.add_argument("port", help="Serial port, for example /dev/cu.usbserial-110")
    parser.add_argument("-b", "--baud", type=int, default=115200)
    parser.add_argument("-o", "--output", type=Path, default=Path("cyd-screenshot.png"))
    parser.add_argument("-t", "--timeout", type=float, default=60.0)
    parser.add_argument(
        "--transform",
        choices=[name for name in PERMUTATIONS] + [f"{name}-invert" for name in PERMUTATIONS],
        help="Apply a color transform to the main output BMP.",
    )
    parser.add_argument(
        "--variants",
        action="store_true",
        help="Also save diagnostic channel-transform variants.",
    )
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=args.timeout) as port:
        port.reset_input_buffer()
        port.write(b"screenshot-serial\n")
        port.flush()

        while True:
            line = read_line(port)
            if line.startswith(b"BEGIN_BMP "):
                size = int(line.split()[1])
                break
            sys.stderr.write(line.decode("utf-8", errors="replace"))

        chunks = []
        remaining = size
        while remaining > 0:
            chunk = port.read(min(4096, remaining))
            if not chunk:
                received = size - remaining
                raise TimeoutError(f"Expected {size} bytes, got {received}")
            chunks.append(chunk)
            remaining -= len(chunk)
            received = size - remaining
            print(f"\rReceived {received}/{size} bytes", end="", file=sys.stderr)
        print(file=sys.stderr)
        data = b"".join(chunks)

        end = read_line(port)
        if not end.endswith(b"END_BMP\n"):
            sys.stderr.write(end.decode("utf-8", errors="replace"))
            raise RuntimeError("Did not receive END_BMP marker")

    if args.transform:
        data = transform_bmp(data, args.transform)

    output_data = image_bytes_for_output(data, args.output)
    args.output.write_bytes(output_data)
    print(f"Saved {args.output} ({len(output_data)} bytes)")
    if args.variants:
        variants = write_color_variants(data, args.output)
        print("Saved color variants:")
        for path in variants:
            print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
