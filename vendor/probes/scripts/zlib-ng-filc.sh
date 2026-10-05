#!/usr/bin/env bash
# F00 zlib-ng probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/zlib-ng-filc.json's build_command from committed
# inputs with the pinned Fil-C 0.685 pizfix compiler (vendor/README.md
# section 2): the probe source is vendor/probes/src/zlib-ng/probe.c and libz.a
# is built from vendor/src/zlib-ng (created by vendor/scripts/zlib-ng.sh) with
# the recorded CMake options (zlib compat static, no tests). Paths resolve
# from this script's location; the output directory is created by CMake. No
# network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/zlib-ng"
OUT="$ROOT/vendor/build/zlib-ng-filc"
PROBE="$ROOT/vendor/probes/src/zlib-ng/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "zlib-ng-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "zlib-ng-filc: cmake not found" >&2; exit 2; }
[ -f "$SRC/CMakeLists.txt" ] || { echo "zlib-ng-filc: missing $SRC; run vendor/scripts/zlib-ng.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "zlib-ng-filc: missing $PROBE" >&2; exit 2; }
echo "== compiler: $("$FILC" --version | head -1)"

cmake -S "$SRC" -B "$OUT" \
    -DCMAKE_C_COMPILER="$FILC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZLIB_COMPAT=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF
cmake --build "$OUT" --parallel "$(nproc)"

"$FILC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$OUT" -I"$SRC" "$OUT/libz.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
