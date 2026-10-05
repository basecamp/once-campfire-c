#!/usr/bin/env bash
# S01 storage test runner (direct clang/filcc until the integrator adds
# src/storage/{files,process}.c and tests/storage to the Makefile lists).
#
#   MODE=dev       clang -O1 -g                    -> build/s01
#   MODE=sanitize  clang ASan+UBSan (leaks)        -> build/s01/sanitize
#   MODE=filc      Fil-C 0.685 pizfix driver       -> build/s01/filc
#
# test_files links libcrypto (OpenSSL 4.0.3 EVP MD5/base64) from the pinned
# artifacts; test_process links core clock/buffer only and spawns the fixed
# ffmpeg/ffprobe paths from the S01 executable enum.
set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE=${MODE:-dev}
CC=${CC:-clang}

case "$MODE" in
dev)
    OUT=build/s01
    OPT="-O1 -g"
    RUN_ENV=""
    OPENSSL_INC="-Ivendor/build/openssl-clang/install/include"
    OPENSSL_LIB="vendor/build/openssl-clang/install/lib/libcrypto.a"
    ;;
sanitize)
    OUT=build/s01/sanitize
    OPT="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    RUN_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1"
    OPENSSL_INC="-Ivendor/build/openssl-clang/install/include"
    OPENSSL_LIB="vendor/build/openssl-clang/install/lib/libcrypto.a"
    ;;
filc)
    CC=${FILC:-/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc}
    OUT=build/s01/filc
    OPT="-O2"
    RUN_ENV=""
    OPENSSL_INC="-Ivendor/build/openssl-filc/install/include"
    OPENSSL_LIB="vendor/build/openssl-filc/install/lib/libcrypto.a"
    ;;
*)
    echo "unknown MODE '$MODE' (dev|sanitize|filc)" >&2
    exit 2
    ;;
esac

STRICT="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread"
CORE="src/core/alloc.c src/core/buffer.c src/core/clock.c src/core/random.c"
mkdir -p "$OUT"

# shellcheck disable=SC2086
"$CC" $STRICT $OPT -Isrc -Itests $OPENSSL_INC \
    tests/storage/test_files.c src/storage/files.c $CORE \
    $OPENSSL_LIB -ldl -o "$OUT/test_files"
# shellcheck disable=SC2086
"$CC" $STRICT $OPT -Isrc -Itests \
    tests/storage/test_process.c src/storage/process.c \
    src/core/alloc.c src/core/buffer.c src/core/clock.c \
    -o "$OUT/test_process"

fail=0
for name in test_files test_process; do
    echo "-- $OUT/$name"
    # shellcheck disable=SC2086
    env $RUN_ENV "$OUT/$name" || fail=1
done
exit "$fail"
