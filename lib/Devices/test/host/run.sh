#!/bin/sh
# Host-side unit tests for lib/Devices (pure C++; no Arduino/ESP-IDF).
# ArduinoJson comes from PlatformIO's libdeps (populated by `pio run -e x4`);
# override with ARDUINOJSON_INC=/path/to/ArduinoJson/src.
set -eu

cd "$(dirname "$0")"

CXX=${CXX:-g++}
ARDUINOJSON_INC=${ARDUINOJSON_INC:-../../../../.pio/libdeps/x4/ArduinoJson/src}
if [ ! -f "$ARDUINOJSON_INC/ArduinoJson.h" ]; then
  echo "run.sh: ArduinoJson not found at '$ARDUINOJSON_INC'." >&2
  echo "Run 'pio run -e x4' once, or set ARDUINOJSON_INC=/path/to/ArduinoJson/src." >&2
  exit 1
fi
CXXFLAGS="-std=c++20 -Wall -Wextra -I../../include -I$ARDUINOJSON_INC"
BUILD_DIR=build

tests=""
for f in test_*.cpp; do
  [ -e "$f" ] || continue
  tests="$tests $f"
done
[ -n "$tests" ] || { echo "run.sh: no test_*.cpp found" >&2; exit 1; }

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

status=0
for src in $tests; do
  bin="$BUILD_DIR/${src%.cpp}"
  echo "== [build] $src"
  if $CXX $CXXFLAGS -o "$bin" "$src" ../../src/Devices.cpp; then
    echo "== [run] $bin"
    if "$bin"; then
      echo "== [pass] $bin"
    else
      echo "== [fail] $bin"
      status=1
    fi
  else
    echo "== [build fail] $src"
    status=1
  fi
done
echo "==============================="
if [ "$status" -eq 0 ]; then
  echo "ALL TESTS PASSED"
else
  echo "TESTS FAILED"
fi
exit "$status"
