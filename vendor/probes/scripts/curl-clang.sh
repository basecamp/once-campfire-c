#!/usr/bin/env bash
# F00 libcurl probe (clang): committed-source build+run recipe.
#
# Reproduces vendor/probes/curl.json's build_command from committed inputs:
# the probe source is vendor/probes/src/curl/curl_probe.c, libcurl is built
# out-of-tree from vendor/src/curl (created by the pinned fetch script
# vendor/scripts/curl.sh) with the recorded configure flags, and the probe GET
# is served by the committed loopback fixture
# vendor/probes/src/curl/http_server.sh. The recorded clang recipe ran
# `autoreconf -fi` first (the git checkout ships no configure); that step is
# kept and runs only when configure is absent. Requires the OpenSSL prefix
# built by vendor/probes/scripts/openssl-clang.sh. Paths resolve from this
# script's location; output directories are created here. No network access
# and no fetching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRCT="$ROOT/vendor/src/curl"
OUT="$ROOT/vendor/build/curl-clang"
BUILD="$OUT/build"
PREFIX="$OUT/install"
OSSL_PREFIX="$ROOT/vendor/build/openssl-clang/install"
PROBE="$ROOT/vendor/probes/src/curl/curl_probe.c"
FIXTURE="$ROOT/vendor/probes/src/curl/http_server.sh"
WWW="$ROOT/vendor/probes/src/curl/www"
CLANG="${CLANG:-clang}"

command -v "$CLANG" >/dev/null 2>&1 || { echo "curl-clang: compiler '$CLANG' not found" >&2; exit 2; }
[ -f "$SRCT/configure.ac" ] || { echo "curl-clang: missing $SRCT; run vendor/scripts/curl.sh" >&2; exit 2; }
[ -f "$PROBE" ] || { echo "curl-clang: missing $PROBE" >&2; exit 2; }
[ -f "$OSSL_PREFIX/lib/libcrypto.a" ] || { echo "curl-clang: missing $OSSL_PREFIX/lib/libcrypto.a; run vendor/probes/scripts/openssl-clang.sh first" >&2; exit 2; }

if [ ! -x "$SRCT/configure" ]; then
    echo "== generating configure (vendor/src/curl/autoreconf -fi, recorded recipe)"
    ( cd "$SRCT" && autoreconf -fi )
fi

mkdir -p "$BUILD" "$OUT/probe"
echo "== compiler: $("$CLANG" --version | head -1)"

cd "$BUILD"
CC="$CLANG" PKG_CONFIG_PATH="$OSSL_PREFIX/lib/pkgconfig" "$SRCT/configure" \
    --prefix="$PREFIX" --with-openssl="$OSSL_PREFIX" \
    --disable-shared --enable-static \
    --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
    --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-ldap \
    --disable-file --disable-ftp --disable-smb --disable-ipfs --disable-manual \
    --without-brotli --without-zstd --without-libpsl --without-nghttp2 \
    --without-libidn2 --without-librtmp --without-libssh2 --without-libgsasl
make -j"$(nproc)"
make install

"$CLANG" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    -I"$PREFIX/include" -o "$OUT/probe/curl_probe_clang" \
    "$PROBE" "$PREFIX/lib/libcurl.a" \
    "$OSSL_PREFIX/lib/libssl.a" "$OSSL_PREFIX/lib/libcrypto.a" -lz -ldl -pthread

echo "== probe run (committed loopback fixture) =="
CAMPFIRE_HTTP_SERVER_LOG="$OUT/probe/http-server.log" \
    "$FIXTURE" "$WWW" "$OUT/probe/curl_probe_clang" \
    "http://127.0.0.1:@PORT@/probe.txt" campfire-f00-curl-probe-body
