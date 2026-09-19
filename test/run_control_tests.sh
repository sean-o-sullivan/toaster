#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/toaster-control-test.XXXXXX")
cd "$root"
cxx=${CXX:-c++}
flags="-std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude"
$cxx $flags src/thermal_engine.cpp src/relay_autotune.cpp src/max31855_decode.cpp \
  test/thermal_engine_test.cpp -o "$out/engine"
"$out/engine"
$cxx $flags src/thermal_engine.cpp src/relay_autotune.cpp test/autotune_test.cpp -o "$out/tune"
"$out/tune"
$cxx $flags src/max31855_decode.cpp test/max31855_decode_test.cpp -o "$out/probe"
"$out/probe"
$cxx $flags src/process_validation.cpp test/process_validation_test.cpp -o "$out/assessment"
"$out/assessment"
$cxx $flags src/thermal_engine.cpp src/relay_autotune.cpp src/process_validation.cpp \
  src/validation_runtime.cpp test/validation_runtime_test.cpp -o "$out/runtime"
"$out/runtime"
$cxx $flags test/display_settings_test.cpp -o "$out/display-settings"
"$out/display-settings"
$cxx $flags test/display_telemetry_test.cpp -o "$out/display-telemetry"
"$out/display-telemetry"
