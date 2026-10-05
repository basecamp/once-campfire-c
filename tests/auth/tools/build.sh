#!/usr/bin/env bash
# Build and run the A01 unit tests directly with clang (plain or ASan).
#
# Wiring into the Makefile (src/auth/** and tests/auth/** lists) is the
# integrator's; this script is the direct-clang evidence path recorded in
# docs/devel/evidence/A01.md.
#
# Usage: tests/auth/tools/build.sh [plain|asan] [test-name ...]
set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="${1:-plain}"
shift || true

case "$MODE" in
plain)
    SAN=
    OUT=build/a01/plain
    MODE_INCLUDES="-Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include"
    ;;
asan)
    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
    OUT=build/a01/asan
    MODE_INCLUDES="-Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include"
    ;;
filc)
    SAN=
    OUT=build/a01/filc
    CC=/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc
    MODE_INCLUDES="-Ivendor/build/libxcrypt-filc -Ivendor/build/openssl-filc/install/include"
    DEPS_OVERRIDE="vendor/build/yyjson-filc/libyyjson.a vendor/build/openssl-filc/install/lib/libcrypto.a vendor/build/libxcrypt-filc/.libs/libcrypt.a vendor/build/sqlite-filc/libsqlite3.a"
    ;;
*)
    echo "unknown mode $MODE (use plain, asan or filc)" >&2
    exit 2
    ;;
esac

CORE="src/core/alloc.c src/core/buffer.c src/core/clock.c src/core/error.c src/core/random.c"
DBCORE="src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c"
HTTP="src/http/params.c src/http/loop.c src/http/request.c src/http/response.c src/http/output.c"
MODELS="src/models/types.c src/models/account.c src/models/active_storage.c src/models/ban.c \
src/models/boost.c src/models/first_run.c src/models/membership.c src/models/message.c \
src/models/push_subscription.c src/models/rich_text_record.c src/models/room.c \
src/models/search.c src/models/session.c src/models/sound.c src/models/user.c \
src/models/webhook.c"
AUTH="src/auth/crypto.c src/auth/json.c src/auth/message.c src/auth/tokens.c \
src/auth/password.c src/auth/session.c src/auth/before.c src/auth/rate.c"
LIB_SRCS="$CORE src/config.c src/app.c src/assets.c src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c src/http/params.c src/http/loop.c src/http/request.c src/http/response.c src/http/output.c src/views/escape.c src/context.c"
SUPPORT="tests/models/support/richtext.c tests/app/support/route_double.c"

CC="${CC:-clang}"
MODE_INCLUDES="${MODE_INCLUDES:-}"
DEPS_OVERRIDE="${DEPS_OVERRIDE:-}"

CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
-Isrc -Itests -Ivendor/src/yyjson/src -Ivendor/src/sqlite/sqlite-amalgamation-3530400 \
-Ivendor/src/picohttpparser \
$MODE_INCLUDES -O1 -g $SAN"

mkdir -p "$OUT/obj"
objs=()
compile_all() {
    for src in "$@"; do
        obj="$OUT/obj/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
            $CC $CFLAGS -MMD -MP -c "$src" -o "$obj"
        fi
        objs+=("$obj")
    done
}
compile_all $LIB_SRCS $MODELS $AUTH $SUPPORT

DEPS="vendor/build/yyjson-clang/libyyjson.a \
vendor/build/openssl-clang/install/lib/libcrypto.a \
vendor/build/libxcrypt-clang/.libs/libcrypt.a \
vendor/build/sqlite-clang/libsqlite3.a"
LINK_DEPS=""
# picohttpparser + sqlite for the HTTP/db library translation units.
PICO="$OUT/obj/picohttpparser.o"
if [ ! -f "$PICO" ]; then
    $CC -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread -O1 -g $SAN \
        -c vendor/src/picohttpparser/picohttpparser.c -o "$PICO"
fi

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    tests=(test_crypto test_tokens test_password test_session test_before test_rate)
fi

failed=0
for name in "${tests[@]}"; do
    src="tests/auth/$name.c"
    obj="$OUT/obj/tests_auth_$name.o"
    $CC $CFLAGS -MMD -MP -c "$src" -o "$obj"
    bin="$OUT/$name"
    if [ -n "$DEPS_OVERRIDE" ]; then
        LINK_DEPS="$DEPS_OVERRIDE"
    else
        LINK_DEPS="$DEPS"
    fi
    $CC $CFLAGS "$obj" "${objs[@]}" "$PICO" $LINK_DEPS \
        -lm -ldl -o "$bin"
    echo "-- $bin"
    "$bin" || failed=1
done
if [ $failed -ne 0 ]; then
    echo "A01 tests ($MODE): FAILED" >&2
    exit 1
fi
echo "A01 tests ($MODE): all passed"
