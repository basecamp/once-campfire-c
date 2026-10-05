#!/usr/bin/env bash
# F00 picohttpparser probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/picohttpparser.json's build_command from committed
# inputs: the probe source is vendor/probes/src/picohttpparser/probe.c and the
# library is compiled from vendor/src/picohttpparser (created by the pinned
# fetch script vendor/scripts/picohttpparser.sh). All paths are resolved from
# this script's location; the output directory is created here. No network
# access and no fetching: ordinary builds never fetch.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/picohttpparser"
OUT="$ROOT/vendor/build/picohttpparser-clang"
PROBE="$ROOT/vendor/probes/src/picohttpparser/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "picohttpparser-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRC/picohttpparser.c" ] || { echo "picohttpparser-clang: missing $SRC; run vendor/scripts/picohttpparser.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "picohttpparser-clang: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$CLANG" --version | head -1)"

"$CLANG" -O2 -Wall -c "$SRC/picohttpparser.c" -o "$OUT/picohttpparser.o"
ar rcs "$OUT/libpicohttpparser.a" "$OUT/picohttpparser.o"
"$CLANG" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$SRC" "$OUT/libpicohttpparser.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
