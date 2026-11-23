#!/bin/sh
# Build and run the extended benchmark suite (nativejson-benchmark +
# simdjson benchmark shapes).  Requires a simdjson checkout with
# build/libsimdjson.a; override SIMDJSON_DIR to point elsewhere.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SIMDJSON_DIR=${SIMDJSON_DIR:-$HOME/simdjson}
CXX=${CXX:-g++}
CXXFLAGS="-std=c++2b -O3 -march=native -DNDEBUG -I$ROOT/include -I$ROOT/src -I$SIMDJSON_DIR/include"
BIN="$ROOT/build/compare_extended"

if [ ! -f "$SIMDJSON_DIR/build/libsimdjson.a" ]; then
  echo "error: $SIMDJSON_DIR/build/libsimdjson.a not found" >&2
  exit 1
fi

mkdir -p "$ROOT/build"
$CXX $CXXFLAGS -o "$BIN" "$ROOT/bench/compare_extended.cpp" \
  "$ROOT/build/libjsoc.a" "$SIMDJSON_DIR/build/libsimdjson.a" -pthread

export BENCH_MODE=${BENCH_MODE:-all}
export BENCH_ITERS=${BENCH_ITERS:-50}
exec taskset -c 2 "$BIN" "$@"
