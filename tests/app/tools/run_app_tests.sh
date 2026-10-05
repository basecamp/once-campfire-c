#!/usr/bin/env bash
# A00 app-test direct run (clang dev, clang ASan+UBSan, clang TSan, pinned
# Fil-C 0.685). The integrator owns the Makefile source/test lists; until
# src/context.c, src/routes.c, src/assets.c and tests/app are wired in, this
# script compiles the same application library directly.
#
# The five classic A00/F03 binaries use tests/app/support/route_double.c as
# the route-table stand-in. test_dispatch_wiring links the real 177-row
# table (src/routes.c) plus the static front mount (src/assets.c) and the
# CSRF path of A01's auth module; --gc-sections keeps auth's models/crypto
# dependencies out of that link.
#
#   MODE=dev       clang -O1 -g                    -> build/app
#   MODE=sanitize  clang ASan+UBSan (leaks)        -> build/app-sanitize
#   MODE=tsan      clang ThreadSanitizer           -> build/app-tsan
#   MODE=filc      Fil-C 0.685 pizfix driver       -> build/app-filc
set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE=${MODE:-dev}
CC=${CC:-clang}
LIBS=""
DEP_INC="-Ivendor/build/libxcrypt-clang"
PICO_FLAGS="-O2"
RUN_ENV=""

case "$MODE" in
dev)
    OUT=${OUT:-build/app}
    OPT="-O1 -g"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
sanitize)
    OUT=${OUT:-build/app-sanitize}
    OPT="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    RUN_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
tsan)
    OUT=${OUT:-build/app-tsan}
    OPT="-O1 -g -fsanitize=thread"
    RUN_ENV="TSAN_OPTIONS=halt_on_error=1"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
filc)
    CC=${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}
    OUT=${OUT:-build/app-filc}
    OPT="-O2"
    DEP_INC="-Ivendor/build/libxcrypt-filc"
    LIBS="vendor/build/sqlite-filc/libsqlite3.a \
          vendor/build/yyjson-filc/libyyjson.a \
          vendor/build/libxcrypt-filc/.libs/libcrypt.a"
    ;;
*)
    echo "unknown MODE '$MODE' (dev|sanitize|tsan|filc)" >&2
    exit 2
    ;;
esac

STRICT="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections"
INCS="-Isrc -Itests -Ivendor/src/yyjson/src -Ivendor/src/sqlite/sqlite-amalgamation-3530400 -Ivendor/src/picohttpparser $DEP_INC"
LIB_SRCS="src/core/alloc.c src/core/buffer.c src/core/clock.c src/core/error.c \
      src/core/random.c src/config.c src/app.c src/context.c src/assets.c \
      src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c \
      src/http/params.c src/http/loop.c src/http/request.c \
      src/http/response.c src/http/output.c src/views/escape.c"
HARNESS="tests/app/support/serve_harness.c"
DOUBLE="tests/app/support/route_double.c"
WIRING_SRCS="$LIB_SRCS src/routes.c src/auth/before.c src/auth/json.c"
GC="-Wl,--gc-sections"

mkdir -p "$OUT"
# Upstream single-file dependency: upstream-appropriate flags only, never the
# application's -Werror (01-foundation-http.md F00 build restriction).
"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread $PICO_FLAGS \
    -c vendor/src/picohttpparser/picohttpparser.c -o "$OUT/picohttpparser.o"

# name:source-extra. The classic binaries share the route double; the wiring
# binary replaces it with the real table and adds the harness.
CLASSIC="test_context test_formats test_app_pool test_app"
for name in $CLASSIC; do
    # shellcheck disable=SC2086
    "$CC" $STRICT $OPT $INCS "tests/app/$name.c" $LIB_SRCS $DOUBLE \
        "$OUT/picohttpparser.o" $LIBS $GC -lm -o "$OUT/$name"
done
# shellcheck disable=SC2086
"$CC" $STRICT $OPT $INCS tests/app/test_serve.c $LIB_SRCS $DOUBLE $HARNESS \
    "$OUT/picohttpparser.o" $LIBS $GC -lm -o "$OUT/test_serve"
# shellcheck disable=SC2086
"$CC" $STRICT $OPT $INCS tests/app/test_dispatch_wiring.c $WIRING_SRCS \
    $HARNESS "$OUT/picohttpparser.o" $LIBS $GC -lm \
    -o "$OUT/test_dispatch_wiring"

fail=0
for name in $CLASSIC test_serve test_dispatch_wiring; do
    echo "-- $OUT/$name"
    # shellcheck disable=SC2086
    env $RUN_ENV "$OUT/$name" || fail=1
done
exit "$fail"
