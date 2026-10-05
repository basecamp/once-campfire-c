#!/usr/bin/env bash
# F00 yyjson probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/yyjson-filc.json's build_command from committed
# inputs with the pinned Fil-C 0.685 pizfix compiler (vendor/README.md
# section 2): the probe source is vendor/probes/src/yyjson/probe.c and
# yyjson.c is compiled from vendor/src/yyjson/src (created by
# vendor/scripts/yyjson.sh). Paths resolve from this script's location; the
# output directory is created here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/yyjson/src"
OUT="$ROOT/vendor/build/yyjson-filc"
PROBE="$ROOT/vendor/probes/src/yyjson/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "yyjson-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRC/yyjson.c" ] || { echo "yyjson-filc: missing $SRC; run vendor/scripts/yyjson.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "yyjson-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$FILC" --version | head -1)"

"$FILC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -c "$SRC/yyjson.c" -o "$OUT/yyjson.o"
ar rcs "$OUT/libyyjson.a" "$OUT/yyjson.o"
"$FILC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$SRC" "$OUT/libyyjson.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
