#!/usr/bin/env bash
#
# Build and run bench/compare_simdjson.cpp, which measures jsoc DOM parsing and
# simdjson DOM parsing in the same process with the same timing protocol.
#
# simdjson discovery order:
#   1. SIMDJSON_DIR=/path/to/simdjson  (uses singleheader/simdjson.cpp)
#   2. pkg-config simdjson             (uses the system/installed library)
#   3. /usr/include/simdjson.h + -lsimdjson
#
# Usage:
#   bench/compare.sh [file ...]
#   SIMDJSON_DIR=/tmp/opencode/simdjson bench/compare.sh file1.json file2.json
#
# Environment:
#   BENCH_ITERS=N   iterations per file (default 200)
#   CXX=...         C++ compiler (default c++)

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

cxx="${CXX:-c++}"
out="build/compare_simdjson"

sj_inc=()
sj_libs=()
sj_extra_src=()

if [[ -n "${SIMDJSON_DIR:-}" ]]; then
    if [[ ! -f "$SIMDJSON_DIR/singleheader/simdjson.h" ]]; then
        echo "SIMDJSON_DIR=$SIMDJSON_DIR does not contain singleheader/simdjson.h" >&2
        exit 1
    fi
    sj_inc=(-I"$SIMDJSON_DIR/singleheader")
    sj_extra_src=("$SIMDJSON_DIR/singleheader/simdjson.cpp")
elif pkg-config --exists simdjson 2>/dev/null; then
    # shellcheck disable=SC2207
    sj_inc=($(pkg-config --cflags simdjson))
    # shellcheck disable=SC2207
    sj_libs=($(pkg-config --libs simdjson))
elif [[ -f /usr/include/simdjson.h ]]; then
    sj_libs=(-lsimdjson)
else
    echo "simdjson not found." >&2
    echo "Set SIMDJSON_DIR to a simdjson checkout, or install simdjson." >&2
    exit 1
fi

echo "== building jsoc (release-native)"
make release-native >/dev/null

echo "== building comparison harness"
"$cxx" -std=c++17 -O3 -march=native -DNDEBUG \
    -Iinclude "${sj_inc[@]}" \
    bench/compare_simdjson.cpp "${sj_extra_src[@]}" \
    build/libjsoc.a "${sj_libs[@]}" \
    -o "$out"

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <json_file> [more_files...]" >&2
    exit 1
fi

"$out" "$@"
