#!/usr/bin/env python3
"""Generate WLED effect metadata and remote.json mappings.

The generator reads the local official WLED checkout and emits:
  - include/generated/wled_effects.h for the touchscreen UI
  - remote.json for WLED's ESP-NOW JSON remote handler
"""

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import ssl
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


DEFAULT_WLED = Path("/path-to-wled")
MAX_EFFECT_ID = 255
EFFECT_PREVIEW_COLORS = 32
EFFECT_PREVIEW_FRAMES = 20
MAX_WS_FRAME_BYTES = 65536
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


def lerp(a: int, b: int, amount: float) -> int:
    return round(a + (b - a) * amount)


def rgb_tuple(color: int) -> tuple[int, int, int]:
    return (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF


def rgb_hex(color: tuple[int, int, int]) -> int:
    return (color[0] << 16) | (color[1] << 8) | color[2]


def blend(a: int, b: int, amount: float) -> int:
    ar, ag, ab = rgb_tuple(a)
    br, bg, bb = rgb_tuple(b)
    return rgb_hex((lerp(ar, br, amount), lerp(ag, bg, amount), lerp(ab, bb, amount)))


def gradient(colors: list[int], count: int = EFFECT_PREVIEW_COLORS) -> list[int]:
    if not colors:
        return [0] * count
    if len(colors) == 1:
        return colors * count
    result = []
    for i in range(count):
        pos = i * (len(colors) - 1) / max(1, count - 1)
        left = int(pos)
        right = min(len(colors) - 1, left + 1)
        result.append(blend(colors[left], colors[right], pos - left))
    return result


def repeating(colors: list[int], count: int = EFFECT_PREVIEW_COLORS) -> list[int]:
    return [colors[i % len(colors)] for i in range(count)]


def sparkle(base: int, spark: int = 0xFFFFFF, count: int = EFFECT_PREVIEW_COLORS) -> list[int]:
    result = [base] * count
    for i in (1, 6, 11, 14):
        if i < count:
            result[i] = spark
    return result


def color_channel(color: int, shift: int) -> int:
    return (color >> shift) & 0xFF


def scale_color(color: int, amount: float) -> int:
    return rgb_hex(
        (
            round(color_channel(color, 16) * amount),
            round(color_channel(color, 8) * amount),
            round(color_channel(color, 0) * amount),
        )
    )


def rgb565(color: int) -> int:
    red = color_channel(color, 16)
    green = color_channel(color, 8)
    blue = color_channel(color, 0)
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def parse_led_color(value: str) -> int:
    value = value.strip().lstrip("#")
    if len(value) > 6:
        value = value[-6:]
    return int(value, 16)


def downsample(colors: list[int], count: int = EFFECT_PREVIEW_COLORS) -> list[int]:
    if not colors:
        return [0] * count

    result = []
    for index in range(count):
        start = index * len(colors) // count
        stop = max(start + 1, (index + 1) * len(colors) // count)
        sample = colors[start:stop]
        red = sum(color_channel(color, 16) for color in sample) // len(sample)
        green = sum(color_channel(color, 8) for color in sample) // len(sample)
        blue = sum(color_channel(color, 0) for color in sample) // len(sample)
        result.append(rgb_hex((red, green, blue)))
    return result


def effect_preview(name: str, effect_id: int) -> list[int]:
    n = name.lower()
    if effect_id == 0 or "solid" in n:
        return [0xFF6000] * EFFECT_PREVIEW_COLORS
    if "rainbow" in n or "pride" in n:
        return gradient([0xFF0000, 0xFF8A00, 0xFFE600, 0x00D95A, 0x00C8FF, 0x304CFF, 0xB000FF])
    if "fire" in n or "candle" in n or "sunrise" in n:
        return gradient([0x050000, 0x7A0000, 0xFF3A00, 0xFFC000, 0xFFFFFF])
    if "water" in n or "ocean" in n or "lake" in n or "pacifica" in n:
        return gradient([0x001040, 0x0058FF, 0x00D2FF, 0xC6FFF8])
    if "noise" in n:
        return repeating([0x111827, 0x0058FF, 0x00D2FF, 0x00BE50, 0x8040FF, 0xFF30A0])
    if "meteor" in n or "comet" in n or "loading" in n or "chase" in n or "scanner" in n or "scan" in n:
        return repeating([0x000000, 0x0B1220, 0x22D3EE, 0xFFFFFF, 0x22D3EE, 0x0B1220])
    if "twinkle" in n or "sparkle" in n or "glitter" in n or "fairy" in n:
        return sparkle(0x07111F)
    if "lightning" in n or "strobe" in n:
        return repeating([0x000000, 0xFFFFFF, 0x67E8F9, 0x000000, 0x000000])
    if "matrix" in n or "freq" in n or "grav" in n:
        return repeating([0x001A0A, 0x00BE50, 0x34D399, 0x08140E])
    if "halloween" in n:
        return repeating([0x000000, 0xFF6000, 0x000000, 0x8040FF])
    if "heartbeat" in n:
        return repeating([0x200000, 0xFF2020, 0xFFB0B0, 0xFF2020, 0x200000, 0x000000])
    if "plasma" in n or "phase" in n or "aurora" in n:
        return gradient([0x2600FF, 0x00D2FF, 0x00BE50, 0x8040FF, 0xFF30A0])
    if "color" in n or "palette" in n or "gradient" in n or "blend" in n:
        return gradient([0xFF30A0, 0xFF6000, 0xFFD600, 0x00BE50, 0x00D2FF, 0x8040FF])
    if "bpm" in n or "pulse" in n or "breathe" in n or "fade" in n:
        return gradient([0x07111F, 0x0058FF, 0x22D3EE, 0x0058FF, 0x07111F])

    palette = [0x0058FF, 0x00D2FF, 0x8040FF, 0xFF30A0, 0xFF6000, 0x00BE50]
    offset = effect_id % len(palette)
    return gradient([palette[offset], palette[(offset + 2) % len(palette)], palette[(offset + 4) % len(palette)]])


def effect_preview_style(name: str, effect_id: int) -> str:
    n = name.lower()
    if effect_id == 0 or "solid" in n:
        return "static"
    if "fire" in n or "candle" in n or "sunrise" in n:
        return "flicker"
    if "twinkle" in n or "sparkle" in n or "glitter" in n or "fairy" in n:
        return "sparkle"
    if "lightning" in n or "strobe" in n:
        return "flash"
    if "bpm" in n or "pulse" in n or "breathe" in n or "fade" in n or "heartbeat" in n:
        return "pulse"
    if "plasma" in n or "phase" in n or "aurora" in n or "water" in n or "ocean" in n or "lake" in n or "pacifica" in n:
        return "wave"
    return "scroll"


def triangle_wave(frame: int, period: int, low: float, high: float) -> float:
    phase = frame % period
    half = period / 2
    ramp = phase if phase < half else period - phase
    return low + (high - low) * ramp / half


def pseudo_noise(frame: int, index: int, seed: int) -> int:
    value = frame * 73 + index * 151 + seed * 41
    value ^= value << 7
    value ^= value >> 9
    value ^= value << 3
    return value & 0xFF


def fallback_preview_frames(name: str, effect_id: int, frame_count: int) -> list[list[int]]:
    base = effect_preview(name, effect_id)
    style = effect_preview_style(name, effect_id)
    frames = []
    for frame in range(frame_count):
        row = []
        for index, color in enumerate(base):
            if style == "scroll":
                row.append(base[(index + frame) % len(base)])
            elif style == "pulse":
                row.append(scale_color(color, triangle_wave(frame, frame_count, 0.28, 1.0)))
            elif style == "sparkle":
                noise = pseudo_noise(frame, index, effect_id)
                row.append(0xFFFFFF if noise > 224 else scale_color(color, 0.37))
            elif style == "flicker":
                noise = pseudo_noise(frame, index, effect_id)
                row.append(scale_color(color, 0.58 + (noise / 255) * 0.42))
            elif style == "flash":
                phase = frame % 10
                row.append(0xFFFFFF if phase in {0, 1, 4} else scale_color(color, 0.16 if phase > 5 else 0.5))
            elif style == "wave":
                amount = triangle_wave(frame + index, frame_count, 0.32, 1.0)
                row.append(scale_color(base[(index + frame // 2) % len(base)], amount))
            else:
                row.append(color)
        frames.append(row)
    return frames


def http_json(url: str, payload: dict | None = None, timeout: float = 5.0) -> dict:
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def http_get(url: str, timeout: float = 5.0) -> None:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        response.read()


def load_preview_cache(path: Path, host: str, led_count: int, frame_count: int) -> dict[int, list[list[int]]]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    if (
        data.get("host") != host
        or data.get("led_count") != led_count
        or data.get("frame_count") != frame_count
        or data.get("color_count") != EFFECT_PREVIEW_COLORS
    ):
        return {}
    effects = data.get("effects", {})
    return {int(effect_id): frames for effect_id, frames in effects.items()}


def save_preview_cache(
    path: Path,
    host: str,
    led_count: int,
    frame_count: int,
    previews: dict[int, list[list[int]]],
) -> None:
    data = {
        "host": host,
        "led_count": led_count,
        "frame_count": frame_count,
        "color_count": EFFECT_PREVIEW_COLORS,
        "effects": {str(effect_id): frames for effect_id, frames in sorted(previews.items())},
    }
    path.write_text(json.dumps(data, separators=(",", ":")) + "\n", encoding="utf-8")


class WebSocketClient:
    def __init__(self, url: str, timeout: float = 5.0) -> None:
        self.url = url
        self.timeout = timeout
        self.sock: socket.socket | ssl.SSLSocket | None = None

    def __enter__(self) -> "WebSocketClient":
        parsed = urllib.parse.urlparse(self.url)
        secure = parsed.scheme == "wss"
        host = parsed.hostname or ""
        port = parsed.port or (443 if secure else 80)
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query

        raw = socket.create_connection((host, port), timeout=self.timeout)
        if secure:
            raw = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
        raw.settimeout(self.timeout)
        self.sock = raw

        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        raw.sendall(request.encode("ascii"))
        response = self._read_http_response()
        expected = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
        ).decode("ascii")
        if " 101 " not in response.split("\r\n", 1)[0]:
            raise RuntimeError(f"WebSocket upgrade failed: {response.splitlines()[0]}")
        if expected not in response:
            raise RuntimeError("WebSocket upgrade failed: accept key mismatch")
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if self.sock:
            try:
                self.send_frame(0x8, b"")
            except OSError:
                pass
            self.sock.close()
            self.sock = None

    def _read_http_response(self) -> str:
        chunks = bytearray()
        while b"\r\n\r\n" not in chunks:
            chunk = self._recv_exact(1)
            chunks.extend(chunk)
        return chunks.decode("iso-8859-1")

    def _recv_exact(self, length: int) -> bytes:
        if not self.sock:
            raise RuntimeError("WebSocket is not connected")
        if length > MAX_WS_FRAME_BYTES:
            raise RuntimeError(f"WebSocket frame too large: {length} bytes")
        data = bytearray()
        while len(data) < length:
            chunk = self.sock.recv(length - len(data))
            if not chunk:
                raise RuntimeError("WebSocket closed")
            data.extend(chunk)
        return bytes(data)

    def send_frame(self, opcode: int, payload: bytes) -> None:
        if not self.sock:
            raise RuntimeError("WebSocket is not connected")
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.extend([0x80 | 126])
            header.extend(struct.pack("!H", length))
        else:
            header.extend([0x80 | 127])
            header.extend(struct.pack("!Q", length))
        mask = os.urandom(4)
        header.extend(mask)
        masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def send_text(self, value: str) -> None:
        self.send_frame(0x1, value.encode("utf-8"))

    def recv_frame(self) -> tuple[int, bytes]:
        first, second = self._recv_exact(2)
        opcode = first & 0x0F
        masked = second & 0x80
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        if length > MAX_WS_FRAME_BYTES:
            raise RuntimeError(f"WebSocket frame too large: {length} bytes")

        mask = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(length)
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        if opcode == 0x9:
            self.send_frame(0xA, payload)
        return opcode, payload

    def recv_live_colors(self) -> list[int] | None:
        while True:
            opcode, payload = self.recv_frame()
            if opcode == 0x8:
                raise RuntimeError("WebSocket closed")
            if opcode != 0x2 or len(payload) < 5 or payload[0] != ord("L"):
                continue
            start = 4 if payload[1] == 2 else 2
            colors = []
            for index in range(start, len(payload) - 2, 3):
                colors.append(rgb_hex((payload[index], payload[index + 1], payload[index + 2])))
            return colors

    def drain(self, seconds: float) -> None:
        if not self.sock:
            return
        end = time.monotonic() + seconds
        old_timeout = self.sock.gettimeout()
        try:
            while time.monotonic() < end:
                self.sock.settimeout(max(0.05, min(0.2, end - time.monotonic())))
                try:
                    self.recv_frame()
                except (socket.timeout, TimeoutError):
                    pass
        finally:
            self.sock.settimeout(old_timeout)


def wled_effect_state(effect_id: int, led_count: int) -> dict:
    return {
        "on": True,
        "bri": 255,
        "seg": [
            {
                "id": 0,
                "start": 0,
                "stop": led_count,
                "fx": effect_id,
                "sx": 128,
                "ix": 128,
                "pal": 0,
                "col": [[255, 160, 0], [0, 0, 0], [0, 0, 0]],
            }
        ],
    }


def set_wled_effect(base_url: str, effect_id: int, led_count: int) -> None:
    state_url = urllib.parse.urljoin(base_url, "json/state")
    state = wled_effect_state(effect_id, led_count)
    try:
        http_json(state_url, state)
        return
    except urllib.error.HTTPError as error:
        if error.code != 501:
            raise

    # Some builds reject JSON state writes, but the classic HTTP API is almost always available.
    api = f"win&T=1&A=255&FX={effect_id}&SX=128&IX=128&FP=0"
    http_get(urllib.parse.urljoin(base_url, api))


def capture_wled_preview_frames(
    host: str,
    effects: list[dict],
    led_count: int,
    frame_count: int,
    interval: float,
    settle: float,
    cache_path: Path | None,
) -> dict[int, list[list[int]]]:
    base_url = host.rstrip("/") + "/"
    parsed = urllib.parse.urlparse(base_url)
    ws_scheme = "wss" if parsed.scheme == "https" else "ws"
    ws_url = urllib.parse.urlunparse((ws_scheme, parsed.netloc, "/ws", "", "", ""))
    previews = load_preview_cache(cache_path, host, led_count, frame_count) if cache_path else {}

    def capture_effect_ws(effect: dict) -> list[list[int]]:
        effect_id = effect["id"]
        with WebSocketClient(ws_url) as ws:
            ws.send_text(json.dumps({"lv": True}, separators=(",", ":")))
            ws.send_text(json.dumps(wled_effect_state(effect_id, led_count), separators=(",", ":")))
            ws.drain(settle)
            frames = []
            while len(frames) < frame_count:
                colors = ws.recv_live_colors()
                if colors:
                    frames.append(downsample(colors[:led_count]))
                    if interval > 0:
                        ws.drain(interval)
            return frames

    ws_available = True
    for effect in effects:
        if effect["id"] in previews:
            print(f"Using cached {effect['name']}")
            continue
        for attempt in range(2):
            try:
                previews[effect["id"]] = capture_effect_ws(effect)
                if cache_path:
                    save_preview_cache(cache_path, host, led_count, frame_count, previews)
                print(f"Captured {effect['name']}")
                break
            except (OSError, OverflowError, RuntimeError, socket.timeout, TimeoutError, ValueError) as error:
                if attempt == 0:
                    time.sleep(0.35)
                    continue
                print(f"WebSocket preview failed for {effect['name']}: {error}.")
                ws_available = False
        if effect["id"] not in previews:
            break

    if not ws_available:
        print("Falling back to HTTP live preview for remaining effects.")
        live_url = urllib.parse.urljoin(base_url, "json/live")
        for effect in effects:
            if effect["id"] in previews:
                continue
            effect_id = effect["id"]
            try:
                set_wled_effect(base_url, effect_id, led_count)
                time.sleep(settle)
                frames = []
                for _ in range(frame_count):
                    live = http_json(live_url)
                    colors = [parse_led_color(value) for value in live.get("leds", [])]
                    frames.append(downsample(colors[:led_count]))
                    time.sleep(interval)
                previews[effect_id] = frames
                if cache_path:
                    save_preview_cache(cache_path, host, led_count, frame_count, previews)
                print(f"Captured {effect['name']}")
            except (OSError, urllib.error.URLError, json.JSONDecodeError, ValueError) as error:
                print(f"Preview capture failed for {effect['name']}: {error}. Using generated fallback.")
                previews[effect_id] = fallback_preview_frames(effect["name"], effect_id, frame_count)

    return previews


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


def write_header(path: Path, effects: list[dict], preview_frames: dict[int, list[list[int]]], frame_count: int) -> None:
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
        f"  uint16_t preview[{frame_count}][{EFFECT_PREVIEW_COLORS}];",
        "};",
        "",
        f"constexpr size_t kWledEffectPreviewColors = {EFFECT_PREVIEW_COLORS};",
        f"constexpr size_t kWledEffectPreviewFrames = {frame_count};",
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
        frames = preview_frames.get(effect["id"], fallback_preview_frames(effect["name"], effect["id"], frame_count))
        preview = ", ".join(
            "{" + ", ".join(f"0x{rgb565(color):04X}" for color in frame) + "}" for frame in frames
        )
        lines.append(
            f'    {{{effect["id"]}, {effect["button"]}, {effect["mask"]}, "{c_escape(effect["name"])}", "{c_escape(effect["labels"])}", {{{preview}}}}},'
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
        "--preview-host",
        help="optional WLED controller base URL used to capture exact /json/live preview frames",
    )
    parser.add_argument("--preview-leds", type=int, default=150)
    parser.add_argument("--preview-frames", type=int, default=EFFECT_PREVIEW_FRAMES)
    parser.add_argument("--preview-interval", type=float, default=0.15)
    parser.add_argument("--preview-settle", type=float, default=0.45)
    parser.add_argument("--preview-cache", type=Path, default=Path(".wled-preview-cache.json"))
    parser.add_argument(
        "--include-2d-only",
        action="store_true",
        help="include effects WLED hides unless a 2D matrix segment is available",
    )
    args = parser.parse_args()

    effects = parse_effects(args.wled, include_2d_only=args.include_2d_only)
    if args.preview_host:
        preview_frames = capture_wled_preview_frames(
            args.preview_host,
            effects,
            args.preview_leds,
            args.preview_frames,
            args.preview_interval,
            args.preview_settle,
            args.preview_cache,
        )
    else:
        preview_frames = {
            effect["id"]: fallback_preview_frames(effect["name"], effect["id"], args.preview_frames)
            for effect in effects
        }
    write_header(args.header, effects, preview_frames, args.preview_frames)
    write_remote_json(args.remote, effects)
    print(f"Generated {len(effects)} effects through WLED effect ID {max(effect['id'] for effect in effects)}.")


if __name__ == "__main__":
    main()
