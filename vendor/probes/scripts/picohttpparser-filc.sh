#!/usr/bin/env bash
# F00 picohttpparser probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/picohttpparser-filc.json's build_command from
# committed inputs using the pinned Fil-C 0.685 pizfix compiler
# (vendor/README.md section 2): the probe source is
# vendor/probes/src/picohttpparser/probe.c and the library is compiled from
# vendor/src/picohttpparser (created by vendor/scripts/picohttpparser.sh).
# All paths are resolved from this script's location; the output directory is
# created here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/picohttpparser"
OUT="$ROOT/vendor/build/picohttpparser-filc"
PROBE="$ROOT/vendor/probes/src/picohttpparser/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "picohttpparser-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRC/picohttpparser.c" ] || { echo "picohttpparser-filc: missing $SRC; run vendor/scripts/picohttpparser.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "picohttpparser-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$FILC" --version | head -1)"

"$FILC" -O2 -Wall -c "$SRC/picohttpparser.c" -o "$OUT/picohttpparser.o"
ar rcs "$OUT/libpicohttpparser.a" "$OUT/picohttpparser.o"
"$FILC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$SRC" "$OUT/libpicohttpparser.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
