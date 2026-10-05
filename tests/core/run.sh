#!/bin/sh
# F01 core test driver. Standalone C programs compiled directly with clang;
# does not depend on the F02 shared test runner.
#
# Usage: tests/core/run.sh [plain|sanitize]
set -eu

mode=${1:-plain}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cc=${CC:-clang}

case "$mode" in
plain) out="$root/build/f01/plain" ;;
sanitize) out="$root/build/f01/sanitize" ;;
*)
    echo "usage: $0 [plain|sanitize]" >&2
    exit 2
    ;;
esac

cflags="-std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pthread -I$root/src -I$root/tests/core"
if [ "$mode" = sanitize ]; then
    cflags="$cflags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
fi

srcs="$root/src/core/alloc.c $root/src/core/buffer.c $root/src/core/clock.c $root/src/core/random.c $root/src/core/error.c"
mkdir -p "$out"

for t in test_buffer test_clock test_random test_error; do
    # shellcheck disable=SC2086
    $cc $cflags "$root/tests/core/$t.c" $srcs -o "$out/$t"
    if [ "$mode" = sanitize ]; then
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
            UBSAN_OPTIONS=print_stacktrace=1 "$out/$t"
    else
        "$out/$t"
    fi
done

echo "core tests ($mode): all passed"
