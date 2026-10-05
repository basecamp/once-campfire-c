#!/usr/bin/env bash
# F00 Gumbo probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/gumbo.json's build_command from committed inputs:
# the probe source is vendor/probes/src/gumbo/probe.c and the 17 upstream
# source files are compiled from vendor/src/gumbo/gumbo-parser/src (created by
# the pinned fetch script vendor/scripts/gumbo.sh). Paths resolve from this
# script's location; the output directory is created here. No network access
# and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/gumbo/gumbo-parser/src"
OUT="$ROOT/vendor/build/gumbo-clang"
PROBE="$ROOT/vendor/probes/src/gumbo/probe.c"
CLANG="${CLANG:-clang}"
FILES="ascii attribute char_ref error foreign_attrs parser string_buffer string_piece svg_attrs svg_tags tag tag_lookup token_buffer tokenizer utf8 util vector"

command -v "$CLANG" >/dev/null 2>&1 || { echo "gumbo-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRC/parser.c" ] || { echo "gumbo-clang: missing $SRC; run vendor/scripts/gumbo.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "gumbo-clang: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT/obj"
echo "== compiler: $("$CLANG" --version | head -1)"

for f in $FILES; do
    "$CLANG" -std=c99 -Wall -O2 -fPIC -I"$SRC" -c "$SRC/$f.c" -o "$OUT/obj/$f.o"
done
ar rcs "$OUT/libgumbo.a" "$OUT"/obj/*.o
"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC" "$PROBE" "$OUT/libgumbo.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
