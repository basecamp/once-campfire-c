#!/usr/bin/env bash
# F00 qrcodegen probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/qrcodegen.json's build_command from committed
# inputs: the probe source is vendor/probes/src/qrcodegen/probe.c and only
# c/qrcodegen.c is compiled from vendor/src/qrcodegen (created by the pinned
# fetch script vendor/scripts/qrcodegen.sh). Paths resolve from this script's
# location; the output directory is created here. No network access and no
# fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/qrcodegen"
OUT="$ROOT/vendor/build/qrcodegen-clang"
PROBE="$ROOT/vendor/probes/src/qrcodegen/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "qrcodegen-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRC/c/qrcodegen.c" ] || { echo "qrcodegen-clang: missing $SRC; run vendor/scripts/qrcodegen.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "qrcodegen-clang: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$CLANG" --version | head -1)"

"$CLANG" -std=c99 -O2 -Wall -fPIC -c "$SRC/c/qrcodegen.c" -o "$OUT/qrcodegen.o"
ar rcs "$OUT/libqrcodegen.a" "$OUT/qrcodegen.o"
"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC/c" "$PROBE" "$OUT/libqrcodegen.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
