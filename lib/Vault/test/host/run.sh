#!/bin/sh
# Host-side unit tests for lib/Vault (pure C++, no Arduino/ESP-IDF).
# The AES backend is software mbedtls (device uses the ESP-IDF HW port), and
# deriveKey reuses lib/MideaAC's portable Sha256.
# Requires libmbedtls-dev on the host (see MEMORY.md Tooling).
set -eu

cd "$(dirname "$0")"

CXX=${CXX:-g++}
CXXFLAGS="-std=c++20 -Wall -Wextra -I../../include -I../../../MideaAC/include"
LDLIBS="-lmbedcrypto"
SRCS="../../src/Vault.cpp ../../../MideaAC/src/Sha256.cpp"
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
  if $CXX $CXXFLAGS -o "$bin" "$src" $SRCS $LDLIBS; then
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
