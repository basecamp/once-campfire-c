#!/usr/bin/env bash
# F00 Gumbo probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/gumbo-filc.json's build_command (and the recorded
# vendor/build/gumbo-filc/build.sh) from committed inputs with the pinned
# Fil-C 0.685 pizfix compiler (vendor/README.md section 2): the probe source is
# vendor/probes/src/gumbo/probe.c and the 17 upstream source files are compiled
# from vendor/src/gumbo/gumbo-parser/src (created by vendor/scripts/gumbo.sh).
# Paths resolve from this script's location; the output directory is created
# here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/gumbo/gumbo-parser/src"
OUT="$ROOT/vendor/build/gumbo-filc"
PROBE="$ROOT/vendor/probes/src/gumbo/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"
FILES="ascii attribute char_ref error foreign_attrs parser string_buffer string_piece svg_attrs svg_tags tag tag_lookup token_buffer tokenizer utf8 util vector"

[ -x "$FILC" ] || { echo "gumbo-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRC/parser.c" ] || { echo "gumbo-filc: missing $SRC; run vendor/scripts/gumbo.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "gumbo-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT/obj"
echo "== compiler: $("$FILC" --version | head -1)"

for f in $FILES; do
    "$FILC" -std=c99 -Wall -O2 -fPIC -I"$SRC" -c "$SRC/$f.c" -o "$OUT/obj/$f.o"
done
ar rcs "$OUT/libgumbo.a" "$OUT"/obj/*.o
"$FILC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$SRC" "$PROBE" "$OUT/libgumbo.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
