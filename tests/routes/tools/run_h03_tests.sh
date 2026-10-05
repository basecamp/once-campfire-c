#!/usr/bin/env bash
# H03's direct test run (clang dev, clang ASan+UBSan, clang TSan, and the
# pinned Fil-C 0.685 toolchain). The integrator owns the Makefile source/test
# lists; until src/routes.c, src/assets.c and tests/routes|assets are wired
# in, this script compiles the same application library with H03's two
# sources added (and A00's src/context.c, which the integrator has not wired
# either) and runs H03's four binaries.
#
#   MODE=dev       clang -O1 -g                    -> build/h03
#   MODE=tsan      clang ThreadSanitizer           -> build/h03-tsan
#   MODE=sanitize  clang ASan+UBSan                -> build/h03-sanitize
#   MODE=filc      Fil-C 0.685 pizfix driver       -> build/h03-filc
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
    OUT=${OUT:-build/h03}
    OPT="-O1 -g"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
sanitize)
    OUT=${OUT:-build/h03-sanitize}
    OPT="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    RUN_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
tsan)
    OUT=${OUT:-build/h03-tsan}
    OPT="-O1 -g -fsanitize=thread"
    RUN_ENV="TSAN_OPTIONS=halt_on_error=1"
    LIBS="vendor/build/sqlite-clang/libsqlite3.a \
          vendor/build/yyjson-clang/libyyjson.a \
          vendor/build/libxcrypt-clang/.libs/libcrypt.a"
    ;;
filc)
    CC=${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}
    OUT=${OUT:-build/h03-filc}
    OPT="-O2"
    DEP_INC="-Ivendor/build/libxcrypt-filc"
    LIBS="vendor/build/sqlite-filc/libsqlite3.a \
          vendor/build/yyjson-filc/libyyjson.a \
          vendor/build/libxcrypt-filc/.libs/libcrypt.a"
    ;;
*)
    echo "unknown MODE '$MODE' (dev|sanitize|filc)" >&2
    exit 2
    ;;
esac

STRICT="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections"
INCS="-Isrc -Itests -Ivendor/src/yyjson/src -Ivendor/src/sqlite/sqlite-amalgamation-3530400 -Ivendor/src/picohttpparser $DEP_INC"
# The POST conductor rows call A01's cf_check_csrf; --gc-sections keeps only
# before.c's CSRF path (and json.c's span helpers), so the auth module's
# models/crypto/OpenSSL dependencies stay out of H03's test binaries.
SRCS="src/core/alloc.c src/core/buffer.c src/core/clock.c src/core/error.c \
      src/core/random.c src/config.c src/app.c src/context.c src/routes.c \
      src/assets.c src/auth/before.c src/auth/json.c \
      src/db/schema.c src/db/reader.c src/db/statements.c \
      src/db/writer.c src/http/params.c src/http/loop.c src/http/request.c \
      src/http/response.c src/http/output.c src/views/escape.c"
GC="-Wl,--gc-sections"

echo "-- gen_routes.py --check src/routes.c"
python3 tests/routes/tools/gen_routes.py --check src/routes.c

mkdir -p "$OUT"
# Upstream single-file dependency: upstream-appropriate flags only, never the
# application's -Werror (01-foundation-http.md F00 build restriction).
"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread $PICO_FLAGS \
    -c vendor/src/picohttpparser/picohttpparser.c -o "$OUT/picohttpparser.o"

TESTS="tests/routes/test_routes_table.c \
       tests/routes/test_routes_recognition.c \
       tests/routes/test_builtins.c \
       tests/assets/test_assets.c"

for src in $TESTS; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    "$CC" $STRICT $OPT $INCS "$src" $SRCS "$OUT/picohttpparser.o" $LIBS \
        $GC -lm -o "$OUT/$name"
done

fail=0
if [ "$MODE" = dev ]; then
    # Differential matcher check against the reference regex semantics.
    # shellcheck disable=SC2086
    "$CC" $STRICT $OPT $INCS tests/routes/tools/match_harness.c $SRCS \
        "$OUT/picohttpparser.o" $LIBS $GC -lm -o "$OUT/match_harness"
    echo "-- diff_matcher.py (reference regex differential)"
    python3 tests/routes/tools/diff_matcher.py --harness "$OUT/match_harness" \
        || fail=1
fi

for name in test_routes_table test_routes_recognition test_builtins test_assets; do
    echo "-- $OUT/$name"
    # shellcheck disable=SC2086
    env $RUN_ENV "$OUT/$name" || fail=1
done
exit "$fail"
