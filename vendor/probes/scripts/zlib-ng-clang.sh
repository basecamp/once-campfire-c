#!/usr/bin/env bash
# F00 zlib-ng probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/zlib-ng.json's build_command from committed inputs:
# the probe source is vendor/probes/src/zlib-ng/probe.c and libz.a is built
# from vendor/src/zlib-ng (created by the pinned fetch script
# vendor/scripts/zlib-ng.sh) with the recorded CMake options (zlib compat
# static, no tests). Paths resolve from this script's location; the output
# directory is created by CMake. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/zlib-ng"
OUT="$ROOT/vendor/build/zlib-ng-clang"
PROBE="$ROOT/vendor/probes/src/zlib-ng/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "zlib-ng-clang: compiler '$CLANG' not found" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "zlib-ng-clang: cmake not found" >&2; exit 2; }
[ -f "$SRC/CMakeLists.txt" ] || { echo "zlib-ng-clang: missing $SRC; run vendor/scripts/zlib-ng.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "zlib-ng-clang: missing $PROBE" >&2; exit 2; }
CC_ABS="$(command -v "$CLANG")"
echo "== compiler: $("$CC_ABS" --version | head -1)"

cmake -S "$SRC" -B "$OUT" \
    -DCMAKE_C_COMPILER="$CC_ABS" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZLIB_COMPAT=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF
cmake --build "$OUT" --parallel "$(nproc)"

"$CLANG" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$OUT" -I"$SRC" "$OUT/libz.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
