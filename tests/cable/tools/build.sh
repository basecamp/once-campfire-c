#!/usr/bin/env bash
# Build and run the C01 cable tests directly with clang (plain, ASan, TSan or
# Fil-C).
#
# Wiring into the Makefile (src/cable/**, tests/cable/**, zlib-ng in the
# dependency lists) is the integrator's; this script is the direct-clang
# evidence path recorded in docs/devel/evidence/C01.md.
#
# The link mirrors the Makefile's current lists: the R02 rich-text production
# sources (src/richtext/*.c) replace the deleted tests/models/support double,
# and libgumbo + zlib-ng are linked per mode (MODE_DEP_LIBS).
#
# Usage: tests/cable/tools/build.sh [plain|asan|tsan|filc] [test-name ...]
set -euo pipefail
cd "$(dirname "$0")/../../.."

MODE="${1:-plain}"
shift || true

case "$MODE" in
plain)
    SAN=
    OUT=build/c01/plain
    ZLIB=vendor/build/zlib-ng-clang/libz.a
    ZINCLUDE="-Ivendor/build/zlib-ng-clang"
    GUMBO=vendor/build/gumbo-clang/libgumbo.a
    GINCLUDE="-Ivendor/src/gumbo/gumbo-parser/src"
    MODE_INCLUDES="-Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include"
    ;;
asan)
    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
    OUT=build/c01/asan
    ZLIB=vendor/build/zlib-ng-clang/libz.a
    ZINCLUDE="-Ivendor/build/zlib-ng-clang"
    GUMBO=vendor/build/gumbo-clang/libgumbo.a
    GINCLUDE="-Ivendor/src/gumbo/gumbo-parser/src"
    MODE_INCLUDES="-Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include"
    ;;
tsan)
    SAN="-fsanitize=thread -fno-omit-frame-pointer -g"
    OUT=build/c01/tsan
    ZLIB=vendor/build/zlib-ng-clang/libz.a
    ZINCLUDE="-Ivendor/build/zlib-ng-clang"
    GUMBO=vendor/build/gumbo-clang/libgumbo.a
    GINCLUDE="-Ivendor/src/gumbo/gumbo-parser/src"
    MODE_INCLUDES="-Ivendor/build/libxcrypt-clang -Ivendor/build/openssl-clang/install/include"
    ;;
filc)
    SAN=
    OUT=build/c01/filc
    CC=/home/msaraiva/.local/fil-c/0.685/filc-0.685-linux-x86_64/build/bin/filcc
    ZLIB=vendor/build/zlib-ng-filc/libz.a
    ZINCLUDE="-Ivendor/build/zlib-ng-filc"
    GUMBO=vendor/build/gumbo-filc/libgumbo.a
    GINCLUDE="-Ivendor/src/gumbo/gumbo-parser/src"
    MODE_INCLUDES="-Ivendor/build/libxcrypt-filc -Ivendor/build/openssl-filc/install/include"
    ;;
*)
    echo "unknown mode $MODE (use plain, asan, tsan or filc)" >&2
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
CABLE="src/cable/socket.c src/cable/protocol.c src/cable/pubsub.c \
src/cable/channels.c src/cable/broadcasts.c src/cable/revocation.c"
# A02 production views/presenters: the C02 broadcast tests render the exact
# Turbo payloads through cf_broadcast_partials_views.
PRESENTERS="src/presenters/accounts.c src/presenters/layout.c \
src/presenters/messages.c src/presenters/rooms.c"
VIEWS="src/views/ctx.c src/views/first_run.c src/views/layout.c \
src/views/messages.c src/views/model.c src/views/pwa.c src/views/render.c \
src/views/rooms.c src/views/session.c src/views/translations.c \
src/views/view_assets.c src/views/welcome.c"
# R02 production rich-text sources (Makefile RICHTEXT_SRCS); the test-only
# tests/models/support/richtext.c double was deleted when this landed.
RICHTEXT="src/richtext/rt_attach.c src/richtext/rt_autolink.c src/richtext/rt_content.c \
src/richtext/rt_dom.c src/richtext/rt_pipeline.c src/richtext/rt_plain.c \
src/richtext/rt_resolver.c src/richtext/rt_richtext.c src/richtext/rt_sanitize.c \
src/richtext/rt_uri.c src/richtext/rt_util.c"
LIB_SRCS="$CORE src/config.c src/app.c src/assets.c src/db/schema.c src/db/reader.c src/db/statements.c src/db/writer.c src/http/params.c src/http/loop.c src/http/request.c src/http/response.c src/http/output.c src/views/escape.c src/context.c"
SUPPORT="$RICHTEXT tests/app/support/route_double.c"

CC="${CC:-clang}"

if [ "$MODE" = filc ]; then
    DEPS_OVERRIDE="vendor/build/yyjson-filc/libyyjson.a \
vendor/build/openssl-filc/install/lib/libcrypto.a \
vendor/build/libxcrypt-filc/.libs/libcrypt.a \
vendor/build/sqlite-filc/libsqlite3.a"
else
    DEPS_OVERRIDE="vendor/build/yyjson-clang/libyyjson.a \
vendor/build/openssl-clang/install/lib/libcrypto.a \
vendor/build/libxcrypt-clang/.libs/libcrypt.a \
vendor/build/sqlite-clang/libsqlite3.a"
fi

CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
-Isrc -Itests -Ivendor/src/yyjson/src -Ivendor/src/sqlite/sqlite-amalgamation-3530400 \
-Ivendor/src/picohttpparser $ZINCLUDE $GINCLUDE \
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
compile_all $LIB_SRCS $MODELS $AUTH $CABLE $PRESENTERS $VIEWS $SUPPORT

PICO="$OUT/obj/picohttpparser.o"
if [ ! -f "$PICO" ] || [ vendor/src/picohttpparser/picohttpparser.c -nt "$PICO" ]; then
    $CC -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -pthread -O1 -g $SAN \
        -c vendor/src/picohttpparser/picohttpparser.c -o "$PICO"
fi

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    tests=(test_cable_protocol test_cable_frames test_cable_deflate
            test_cable_queue test_cable_handshake test_cable_session
            test_cable_loop test_cable_channels test_cable_pubsub
            test_cable_broadcasts test_cable_live test_cable_revocation
            test_cable_wiring)
fi

failed=0
for name in "${tests[@]}"; do
    src="tests/cable/$name.c"
    obj="$OUT/obj/tests_cable_$name.o"
    $CC $CFLAGS -MMD -MP -c "$src" -o "$obj"
    bin="$OUT/$name"
    $CC $CFLAGS "$obj" "${objs[@]}" "$PICO" $DEPS_OVERRIDE "$ZLIB" "$GUMBO" \
        -lm -ldl -o "$bin"
    echo "-- $bin"
    "$bin" || failed=1
done
if [ $failed -ne 0 ]; then
    echo "C01 tests ($MODE): FAILED" >&2
    exit 1
fi
echo "C01 tests ($MODE): all passed"
