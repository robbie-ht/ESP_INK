#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_bin="$(mktemp /tmp/moink-cache-test.XXXXXX)"
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic -g \
    ${TEST_CFLAGS:--fsanitize=address,undefined -fno-omit-frame-pointer} \
    -Itests/cache_stubs -Isrc src/cache.c tests/test_cache.c -o "$test_bin"
"$test_bin"
