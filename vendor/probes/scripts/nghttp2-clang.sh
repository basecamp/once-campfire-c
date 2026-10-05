#!/usr/bin/env bash
# F00 nghttp2 probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/nghttp2.json's build_command from committed inputs:
# the probe source is vendor/probes/src/nghttp2/probe.c and the static library
# is built from vendor/src/nghttp2 (created by the pinned fetch script
# vendor/scripts/nghttp2.sh) with the recorded CMake options (library only,
# static, no tests, no -Werror). Paths resolve from this script's location; the
# output directory is created by CMake. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/nghttp2"
OUT="$ROOT/vendor/build/nghttp2-clang"
PROBE="$ROOT/vendor/probes/src/nghttp2/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "nghttp2-clang: compiler '$CLANG' not found" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "nghttp2-clang: cmake not found" >&2; exit 2; }
[ -f "$SRC/CMakeLists.txt" ] || { echo "nghttp2-clang: missing $SRC; run vendor/scripts/nghttp2.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "nghttp2-clang: missing $PROBE" >&2; exit 2; }
CC_ABS="$(command -v "$CLANG")"
echo "== compiler: $("$CC_ABS" --version | head -1)"

cmake -S "$SRC" -B "$OUT" \
    -DCMAKE_C_COMPILER="$CC_ABS" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIB_ONLY=ON \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_WERROR=OFF
cmake --build "$OUT" -j"$(nproc)"

"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC/lib/includes" -I"$OUT/lib/includes" \
    "$PROBE" "$OUT/lib/libnghttp2.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
