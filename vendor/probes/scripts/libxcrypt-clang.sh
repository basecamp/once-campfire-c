#!/usr/bin/env bash
# F00 libxcrypt probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/libxcrypt.json's build_command from committed
# inputs: the probe source is vendor/probes/src/libxcrypt/probe.c and the
# static library is built out-of-tree from vendor/src/libxcrypt (created by
# the pinned fetch script vendor/scripts/libxcrypt.sh) with the recorded
# autotools options --disable-shared --enable-static --disable-werror. The
# upstream autogen.sh is run only when the fetched tree ships no configure
# script (the git pin does not commit one); it regenerates build-system files
# inside vendor/src/libxcrypt, exactly as the recorded recipe did. Paths
# resolve from this script's location; the output directory is created here.
# No network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRCT="$ROOT/vendor/src/libxcrypt"
OUT="$ROOT/vendor/build/libxcrypt-clang"
PROBE="$ROOT/vendor/probes/src/libxcrypt/probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "libxcrypt-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRCT/configure.ac" ] || { echo "libxcrypt-clang: missing $SRCT; run vendor/scripts/libxcrypt.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "libxcrypt-clang: missing $PROBE" >&2; exit 2; }

if [ ! -x "$SRCT/configure" ]; then
    echo "== generating configure (vendor/src/libxcrypt/autogen.sh, recorded recipe)"
    ( cd "$SRCT" && ./autogen.sh )
fi

mkdir -p "$OUT"
echo "== compiler: $("$CLANG" --version | head -1)"

cd "$OUT"
CC="$CLANG" "$SRCT/configure" --disable-shared --enable-static --disable-werror \
    --prefix="$OUT/prefix"
make -j"$(nproc)"

"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$OUT" "$PROBE" "$OUT/.libs/libcrypt.a" -o "$OUT/probe"

echo "== probe run =="
"$OUT/probe"
