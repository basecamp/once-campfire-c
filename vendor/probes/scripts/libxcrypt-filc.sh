#!/usr/bin/env bash
# F00 libxcrypt probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/libxcrypt-filc.json's build_command (and the
# recorded vendor/build/libxcrypt-filc/build.sh) from committed inputs with
# the pinned Fil-C 0.685 pizfix compiler (vendor/README.md section 2): the
# probe source is vendor/probes/src/libxcrypt/probe.c and the static library
# is built out-of-tree from vendor/src/libxcrypt (created by
# vendor/scripts/libxcrypt.sh) with --disable-shared --enable-static
# --disable-werror. The upstream autogen.sh is run only when the fetched tree
# ships no configure script. Paths resolve from this script's location; the
# output directory is created here. No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRCT="$ROOT/vendor/src/libxcrypt"
OUT="$ROOT/vendor/build/libxcrypt-filc"
PROBE="$ROOT/vendor/probes/src/libxcrypt/probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "libxcrypt-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
[ -f "$SRCT/configure.ac" ] || { echo "libxcrypt-filc: missing $SRCT; run vendor/scripts/libxcrypt.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "libxcrypt-filc: missing $PROBE" >&2; exit 2; }

if [ ! -x "$SRCT/configure" ]; then
    echo "== generating configure (vendor/src/libxcrypt/autogen.sh, recorded recipe)"
    ( cd "$SRCT" && ./autogen.sh )
fi

mkdir -p "$OUT"
echo "== compiler: $("$FILC" --version | head -1)"

cd "$OUT"
CC="$FILC" "$SRCT/configure" --disable-shared --enable-static --disable-werror \
    --prefix="$OUT/prefix"
make -j"$(nproc)"

"$FILC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$OUT" "$PROBE" "$OUT/.libs/libcrypt.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
