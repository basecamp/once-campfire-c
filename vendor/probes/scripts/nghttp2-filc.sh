#!/usr/bin/env bash
# F00 nghttp2 probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/nghttp2-filc.json's build_command (and the recorded
# vendor/build/nghttp2-filc/build.sh) from committed inputs with the pinned
# Fil-C 0.685 pizfix compiler (vendor/README.md section 2): the probe source is
# vendor/probes/src/nghttp2/probe.c and the static library is built from
# vendor/src/nghttp2 (created by vendor/scripts/nghttp2.sh) with the recorded
# CMake options. Paths resolve from this script's location; the output
# directory is created by CMake. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/nghttp2"
OUT="$ROOT/vendor/build/nghttp2-filc"
PROBE="$ROOT/vendor/probes/src/nghttp2/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "nghttp2-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "nghttp2-filc: cmake not found" >&2; exit 2; }
[ -f "$SRC/CMakeLists.txt" ] || { echo "nghttp2-filc: missing $SRC; run vendor/scripts/nghttp2.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "nghttp2-filc: missing $PROBE" >&2; exit 2; }
echo "== compiler: $("$FILC" --version | head -1)"

cmake -S "$SRC" -B "$OUT" \
    -DCMAKE_C_COMPILER="$FILC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIB_ONLY=ON \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DENABLE_WERROR=OFF
cmake --build "$OUT" -j"$(nproc)"

"$FILC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC/lib/includes" -I"$OUT/lib/includes" \
    "$PROBE" "$OUT/lib/libnghttp2.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
