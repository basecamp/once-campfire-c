#!/usr/bin/env bash
# F00 yyjson probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/yyjson.json's build_command from committed inputs:
# the probe source is vendor/probes/src/yyjson/probe.c and yyjson.c is
# compiled from vendor/src/yyjson/src (created by the pinned fetch script
# vendor/scripts/yyjson.sh). Paths resolve from this script's location; the
# output directory is created here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/yyjson/src"
OUT="$ROOT/vendor/build/yyjson-clang"
PROBE="$ROOT/vendor/probes/src/yyjson/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "yyjson-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRC/yyjson.c" ] || { echo "yyjson-clang: missing $SRC; run vendor/scripts/yyjson.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "yyjson-clang: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$CLANG" --version | head -1)"

"$CLANG" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -c "$SRC/yyjson.c" -o "$OUT/yyjson.o"
ar rcs "$OUT/libyyjson.a" "$OUT/yyjson.o"
"$CLANG" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$SRC" "$OUT/libyyjson.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
