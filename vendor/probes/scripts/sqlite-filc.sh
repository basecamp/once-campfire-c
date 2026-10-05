#!/usr/bin/env bash
# F00 SQLite probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/sqlite-filc.json's build_command from committed
# inputs with the pinned Fil-C 0.685 pizfix compiler (vendor/README.md
# section 2): the probe source is vendor/probes/src/sqlite/probe.c and the
# amalgamation is compiled from vendor/src/sqlite/sqlite-amalgamation-3530400
# (created by vendor/scripts/sqlite.sh). FTS5 + threads enabled, -lm linked.
# Paths resolve from this script's location; the output directory is created
# here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/vendor/src/sqlite/sqlite-amalgamation-3530400"
OUT="$ROOT/vendor/build/sqlite-filc"
PROBE="$ROOT/vendor/probes/src/sqlite/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "sqlite-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRC/sqlite3.c" ] || { echo "sqlite-filc: missing $SRC; run vendor/scripts/sqlite.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "sqlite-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$OUT"
echo "== compiler: $("$FILC" --version | head -1)"

"$FILC" -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -pthread \
    -c "$SRC/sqlite3.c" -o "$OUT/sqlite3.o"
ar rcs "$OUT/libsqlite3.a" "$OUT/sqlite3.o"
"$FILC" -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    "$PROBE" -I"$SRC" "$OUT/libsqlite3.a" -lm -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
