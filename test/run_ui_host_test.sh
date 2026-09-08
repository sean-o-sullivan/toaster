#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lvgl="$root/.pio/libdeps/freenove_oven/lvgl"
out=$(mktemp -d "${TMPDIR:-/tmp}/toaster-ui-test.XXXXXX")
cd "$out"
export CFLAGS="-DLV_CONF_SKIP -DLV_FONT_MONTSERRAT_16=1 -DLV_FONT_MONTSERRAT_20=1 -DLV_FONT_MONTSERRAT_32=1 -DLV_FONT_MONTSERRAT_48=1 -DLV_COLOR_16_SWAP=0"
export lvgl
find "$lvgl/src" -name '*.c' -print0 | xargs -0 -n 1 -P 4 sh -c 'cc $CFLAGS -I"$lvgl" -c "$1"' sh
c++ -std=c++11 -Wall -Wextra -Werror -Wno-undefined-internal $CFLAGS -I"$root/test/ui_host" -I"$root/include" \
  -I"$lvgl" "$root/src/ui.cpp" "$root/src/thermal_engine.cpp" "$root/src/relay_autotune.cpp" "$root/test/ui_host_test.cpp" \
  ./*.o -o ui_test
./ui_test "$out"
printf 'UI captures: %s\n' "$out"
