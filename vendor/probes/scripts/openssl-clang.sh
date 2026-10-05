#!/usr/bin/env bash
# F00 OpenSSL probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/openssl.json's build_command from committed inputs:
# the probe source is vendor/probes/src/openssl/openssl_probe.c and OpenSSL is
# built out-of-tree from vendor/src/openssl (created by the pinned fetch script
# vendor/scripts/openssl.sh) with the recorded options no-asm no-shared; the
# static archive is installed under vendor/build/openssl-clang/install and the
# probe links libcrypto.a only. Paths resolve from this script's location; the
# build/probe output directories are created here. No network access and no
# fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRCT="$ROOT/vendor/src/openssl"
OUT="$ROOT/vendor/build/openssl-clang"
BUILD="$OUT/build"
PREFIX="$OUT/install"
PROBE="$ROOT/vendor/probes/src/openssl/openssl_probe.c"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "openssl-clang: compiler '$CLANG' not found" >&2; exit 2; }
command -v perl >/dev/null 2>&1 || { echo "openssl-clang: perl not found (OpenSSL's Configure is Perl)" >&2; exit 2; }
[ -f "$SRCT/Configure" ] || { echo "openssl-clang: missing $SRCT; run vendor/scripts/openssl.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "openssl-clang: missing $PROBE" >&2; exit 2; }

mkdir -p "$BUILD" "$OUT/probe"
echo "== compiler: $("$CLANG" --version | head -1)"

cd "$BUILD"
CC="$CLANG" perl "$SRCT/Configure" no-asm no-shared \
    --prefix="$PREFIX" --openssldir="$PREFIX/ssl" --libdir=lib
make -j"$(nproc)"
make install_sw

"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$PREFIX/include" -o "$OUT/probe/openssl_probe_clang" \
    "$PROBE" "$PREFIX/lib/libcrypto.a" -ldl -pthread

echo "== probe run =="
"$OUT/probe/openssl_probe_clang"
