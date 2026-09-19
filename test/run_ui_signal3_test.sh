#!/bin/sh
# Native LVGL 8.4 rendering + behaviour tests. No device, heating or flashing.
# Usage: sh test/run_ui_signal3_test.sh [capture-directory]
# Optional: SANITIZE=1 sh test/run_ui_signal3_test.sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lvgl=${LVGL_DIR:-"$root/.pio/libdeps/freenove_oven/lvgl"}
if [ ! -f "$lvgl/lvgl.h" ]; then
  printf '%s\n' 'LVGL not found. Run pio pkg install -e freenove_oven first, or set LVGL_DIR to the LVGL 8.4.0 source folder.' >&2
  exit 2
fi
if [ "$#" -gt 1 ]; then printf 'Usage: %s [capture-directory]\n' "$0" >&2; exit 2; fi
if [ "$#" -eq 1 ]; then mkdir -p "$1"; captures=$(CDPATH= cd -- "$1" && pwd)
else captures=$(mktemp -d "${TMPDIR:-/tmp}/toaster-signal3-captures.XXXXXX"); fi
build=$(mktemp -d "${TMPDIR:-/tmp}/toaster-signal3-build.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
flags='-DLV_CONF_SKIP -DLV_FONT_MONTSERRAT_12=1 -DLV_FONT_MONTSERRAT_16=1 -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_32=1 -DLV_FONT_MONTSERRAT_48=1 -DLV_COLOR_16_SWAP=0'
if [ "${SANITIZE:-0}" = 1 ]; then flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"; fi
CC=${CC:-cc}; CXX=${CXX:-c++}
export lvgl build flags CC
find "$lvgl/src" -name '*.c' -print0 | xargs -0 -n 1 -P "${JOBS:-4}" sh -c '
  relative=${1#"$lvgl/"}
  object=$(printf "%s" "$relative" | tr / _)
  "$CC" $flags -I"$lvgl" -c "$1" -o "$build/$object.o"
' sh
"$CXX" -std=c++11 -Wall -Wextra -Werror $flags \
  -I"$root/test/ui_host" -I"$root/include" -I"$lvgl" \
  "$root/src/ui.cpp" "$root/src/thermal_engine.cpp" "$root/src/relay_autotune.cpp" \
  "$root/src/process_validation.cpp" \
  "$root/test/ui_signal3_test.cpp" "$build"/*.o -o "$build/ui_signal3_test"
"$build/ui_signal3_test" "$captures"
printf 'Real LVGL RGB565 captures (synthetic inputs): %s\n' "$captures"
