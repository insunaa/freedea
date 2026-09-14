#!/bin/sh
# Host-side unit tests for lib/MideaAC (pure C++, no Arduino/ESP-IDF).
# Builds every test_*.cpp next to this script with the host compiler and
# runs each binary. Exits non-zero if any build or run fails.
set -eu

cd "$(dirname "$0")"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++20 -Wall -Wextra -I../../include"
# Host AES backend: software mbedtls (device uses the ESP-IDF HW port).
LDLIBS="-lmbedcrypto"
BUILD_DIR=build

tests=""
for f in test_*.cpp; do
  [ -e "$f" ] || continue
  tests="$tests $f"
done
[ -n "$tests" ] || { echo "run.sh: no test_*.cpp found" >&2; exit 1; }

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

srcs=""
for f in ../../src/*.cpp; do
  [ -e "$f" ] || continue
  srcs="$srcs $f"
done

status=0
for src in $tests; do
  bin="$BUILD_DIR/${src%.cpp}"
  echo "== [build] $src"
  if $CXX $CXXFLAGS -o "$bin" "$src" $srcs $LDLIBS; then
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
