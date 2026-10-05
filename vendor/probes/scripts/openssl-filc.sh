#!/usr/bin/env bash
# F00 OpenSSL probe (Fil-C): committed-source build+run recipe.
#
# Reproduces vendor/probes/openssl-filc.json's build_command from committed
# inputs with the pinned Fil-C 0.685 pizfix compiler (vendor/README.md
# section 2): the probe source is vendor/probes/src/openssl/openssl_probe.c and
# OpenSSL is built out-of-tree from vendor/src/openssl (created by
# vendor/scripts/openssl.sh) with the recorded options no-asm no-shared; the
# static archive is installed under vendor/build/openssl-filc/install and the
# probe links the Fil-C-built libcrypto.a only. Paths resolve from this
# script's location; the build/probe output directories are created here. No
# network access and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRCT="$ROOT/vendor/src/openssl"
OUT="$ROOT/vendor/build/openssl-filc"
BUILD="$OUT/build"
PREFIX="$OUT/install"
PROBE="$ROOT/vendor/probes/src/openssl/openssl_probe.c"
FILC="${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}"

[ -x "$FILC" ] || { echo "openssl-filc: Fil-C 0.685 pizfix compiler not found at $FILC; run vendor/scripts/filc.sh" >&2; exit 2; }
command -v perl >/dev/null 2>&1 || { echo "openssl-filc: perl not found (OpenSSL's Configure is Perl)" >&2; exit 2; }
[ -f "$SRCT/Configure" ] || { echo "openssl-filc: missing $SRCT; run vendor/scripts/openssl.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "openssl-filc: missing $PROBE" >&2; exit 2; }

mkdir -p "$BUILD" "$OUT/probe"
echo "== compiler: $("$FILC" --version | head -1)"

cd "$BUILD"
CC="$FILC" perl "$SRCT/Configure" no-asm no-shared \
    --prefix="$PREFIX" --openssldir="$PREFIX/ssl" --libdir=lib
make -j"$(nproc)"
make install_sw

"$FILC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$PREFIX/include" -o "$OUT/probe/openssl_probe_filc" \
    "$PROBE" "$PREFIX/lib/libcrypto.a" -ldl -pthread

echo "== probe run =="
"$OUT/probe/openssl_probe_filc"
