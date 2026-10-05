#!/bin/sh
# D01 db-core test driver. Compiles each test with the application flags and
# the locked SQLite artifact (vendor/DEPS.json) and runs it. A missing
# dependency or a build failure fails the run; nothing is skipped.
#
# Usage: tests/db/run.sh [plain|sanitize]
set -eu

mode=${1:-plain}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cc=${CC:-clang}
sqlite_dir="$root/vendor/src/sqlite/sqlite-amalgamation-3530400"
sqlite_lib="$root/vendor/build/sqlite-clang/libsqlite3.a"

case "$mode" in
plain) out="$root/build/d01/plain" ;;
sanitize) out="$root/build/d01/sanitize" ;;
*)
    echo "usage: $0 [plain|sanitize]" >&2
    exit 2
    ;;
esac

if [ ! -f "$sqlite_lib" ]; then
    echo "missing locked SQLite artifact $sqlite_lib" >&2
    echo "build it with vendor/scripts/sqlite.sh" >&2
    exit 1
fi
if [ ! -f "$sqlite_dir/sqlite3.h" ]; then
    echo "missing SQLite include dir $sqlite_dir" >&2
    exit 1
fi

cflags="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread -I$root/src -I$root/tests -I$sqlite_dir"
if [ "$mode" = sanitize ]; then
    cflags="$cflags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
fi

srcs="$root/src/core/alloc.c $root/src/core/buffer.c $root/src/core/clock.c $root/src/core/random.c $root/src/core/error.c $root/src/db/schema.c $root/src/db/reader.c $root/src/db/statements.c"
mkdir -p "$out"

for t in test_schema test_reader test_statements test_time; do
    # shellcheck disable=SC2086
    $cc $cflags "$root/tests/db/$t.c" $srcs "$sqlite_lib" -lm -o "$out/$t"
    if [ "$mode" = sanitize ]; then
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
            UBSAN_OPTIONS=print_stacktrace=1 "$out/$t"
    else
        "$out/$t"
    fi
done

echo "db tests ($mode): all passed"
