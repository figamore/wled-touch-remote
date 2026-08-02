#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <esp32-cyd|jc4880p443>" >&2
  exit 2
fi

variant="$1"
case "$variant" in
  esp32-cyd)
    output="web-installer/simulator/esp32-cyd.html"
    board_flags=(-D WLED_CYD_ENABLE_BATTERY=1)
    ;;
  jc4880p443)
    output="web-installer/simulator/jc4880p443.html"
    board_flags=(-D WLED_BOARD=WLED_BOARD_JC4880P443 -D WLED_CYD_ENABLE_BATTERY=0)
    ;;
  *)
    echo "Unknown simulator variant: $variant" >&2
    exit 2
    ;;
esac

if ! command -v em++ >/dev/null 2>&1; then
  echo "em++ was not found. Install and activate the Emscripten SDK first." >&2
  exit 1
fi

# The browser build invokes Emscripten directly instead of PlatformIO, so run
# the normal pre-build generator explicitly.  CI starts from a clean checkout
# where this ignored generated header does not yet exist.
python3 scripts/git_version.py

lvgl_dir=".pio/libdeps/macos/lvgl"
if [ ! -f "$lvgl_dir/lvgl.h" ]; then
  echo "LVGL dependency not found at $lvgl_dir. Run: pio pkg install -e macos" >&2
  exit 1
fi

arduinojson_dir=".pio/libdeps/macos/ArduinoJson/src"
if [ ! -f "$arduinojson_dir/ArduinoJson.h" ]; then
  echo "ArduinoJson dependency not found at $arduinojson_dir. Run: pio pkg install -e macos" >&2
  exit 1
fi

mkdir -p "$(dirname "$output")"

sources=()
while IFS= read -r source; do
  sources+=("$source")
done < <(find src -type f -name '*.cpp' ! -name 'BatteryMonitor.cpp' | sort)

lvgl_sources=()
while IFS= read -r source; do
  lvgl_sources+=("$source")
done < <(find "$lvgl_dir/src" -type f -name '*.c' | sort)

object_dir="$(mktemp -d "${TMPDIR:-/tmp}/wled-lvgl.XXXXXX")"
trap 'rm -rf "$object_dir"' EXIT

lvgl_objects=()
for index in "${!lvgl_sources[@]}"; do
  object="$object_dir/lvgl-$index.o"
  emcc -O3 \
    -D LV_CONF_INCLUDE_SIMPLE \
    -D WLED_TOUCH_SIMULATOR=1 \
    "${board_flags[@]}" \
    -I include \
    -I "$lvgl_dir" \
    -c "${lvgl_sources[$index]}" \
    -o "$object"
  lvgl_objects+=("$object")
done

em++ -O3 -std=c++17 \
  -D LV_CONF_INCLUDE_SIMPLE \
  -D WLED_TOUCH_SIMULATOR=1 \
  -D LGFX_SDL \
  "${board_flags[@]}" \
  -I include/sim \
  -I include \
  -I "$lvgl_dir" \
  -I "$arduinojson_dir" \
  "${sources[@]}" \
  "${lvgl_objects[@]}" \
  -s USE_SDL=2 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=67108864 \
  -s MAXIMUM_MEMORY=268435456 \
  -s NO_EXIT_RUNTIME=1 \
  -s ENVIRONMENT=web \
  --shell-file tools/web_simulator_shell.html \
  -o "$output"
