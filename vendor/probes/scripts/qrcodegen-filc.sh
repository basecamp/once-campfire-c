#!/usr/bin/env bash
# F00 qrcodegen probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/qrcodegen-filc.json's build_command from committed
# inputs with the pinned Fil-C 0.685 pizfix compiler (vendor/README.md
# section 2): the probe source is vendor/probes/src/qrcodegen/probe.c and only
# c/qrcodegen.c is compiled from vendor/src/qrcodegen (created by
# vendor/scripts/qrcodegen.sh). Paths resolve from this script's location; the
# output directory is created here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/qrcodegen"
OUT="$ROOT/vendor/build/qrcodegen-filc"
PROBE="$ROOT/vendor/probes/src/qrcodegen/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "qrcodegen-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRC/c/qrcodegen.c" ] || { echo "qrcodegen-filc: missing $SRC; run vendor/scripts/qrcodegen.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "qrcodegen-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$FILC" --version | head -1)"

"$FILC" -std=c99 -O2 -Wall -fPIC -c "$SRC/c/qrcodegen.c" -o "$OUT/qrcodegen.o"
ar rcs "$OUT/libqrcodegen.a" "$OUT/qrcodegen.o"
"$FILC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC/c" "$PROBE" "$OUT/libqrcodegen.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
