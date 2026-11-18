#!/usr/bin/env bash
#
# Build and run bench/compare_serialization.cpp: serialization / deserialization
# benchmarks for jsoc vs simdjson in one process.
#
# simdjson discovery order:
#   1. SIMDJSON_DIR=/path/to/simdjson  (include/ + build/libsimdjson.a)
#   2. $HOME/simdjson                 (include/ + build/libsimdjson.a)
#   3. pkg-config simdjson
#
# Usage:
#   bench/compare_serialization.sh [file ...]
#   BENCH_MODE=dom_serialize,roundtrip,raw_minify,stream,typed_de,typed_ser \
#     bench/compare_serialization.sh file1.json ...
#
# The typed modes need C++26 static reflection (GCC 16 + -freflection).  When
# the compiler lacks it, the harness is built without those modes.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

cxx="${CXX:-g++}"
out="build/compare_serialization"

sj_inc=()
sj_lib=()

if [[ -n "${SIMDJSON_DIR:-}" ]]; then
    if [[ ! -f "$SIMDJSON_DIR/include/simdjson.h" ]]; then
        echo "SIMDJSON_DIR=$SIMDJSON_DIR does not contain include/simdjson.h" >&2
        exit 1
    fi
    sj_inc=(-I"$SIMDJSON_DIR/include")
    if [[ -f "$SIMDJSON_DIR/build/libsimdjson.a" ]]; then
        sj_lib=("$SIMDJSON_DIR/build/libsimdjson.a")
    elif pkg-config --exists simdjson 2>/dev/null; then
        # shellcheck disable=SC2207
        sj_lib=($(pkg-config --libs simdjson))
    else
        echo "No simdjson library found under $SIMDJSON_DIR/build" >&2
        exit 1
    fi
elif [[ -f "$HOME/simdjson/include/simdjson.h" &&
        -f "$HOME/simdjson/build/libsimdjson.a" ]]; then
    sj_inc=(-I"$HOME/simdjson/include")
    sj_lib=("$HOME/simdjson/build/libsimdjson.a")
elif pkg-config --exists simdjson 2>/dev/null; then
    # shellcheck disable=SC2207
    sj_inc=($(pkg-config --cflags simdjson))
    # shellcheck disable=SC2207
    sj_lib=($(pkg-config --libs simdjson))
else
    echo "simdjson not found." >&2
    echo "Set SIMDJSON_DIR to a checkout with include/ and build/libsimdjson.a." >&2
    exit 1
fi

echo "== building jsoc (release-native)" >&2
make release-native >/dev/null

extra=()
if printf '#include <meta>\nconsteval int f(){ auto m = ^^int; (void)m; return 0; }\nint main(){ return f(); }\n' |
    "$cxx" -std=c++26 -freflection -x c++ -c -o /dev/null - 2>/dev/null; then
    extra=(-std=c++26 -freflection)
else
    echo "== C++26 reflection unavailable: typed_de / typed_ser disabled" >&2
    extra=(-std=c++20 -DBENCH_NO_REFLECTION)
fi

echo "== building serialization harness" >&2
"$cxx" "${extra[@]}" -O3 -march=native -DNDEBUG \
    -Iinclude "${sj_inc[@]}" \
    bench/compare_serialization.cpp build/libjsoc.a "${sj_lib[@]}" \
    -pthread -o "$out"

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <json_file> [more_files...]" >&2
    exit 1
fi

"$out" "$@"
