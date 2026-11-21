#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_bin="$(mktemp /tmp/moink-store-test.XXXXXX)"
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic -g \
    ${TEST_CFLAGS:--fsanitize=address,undefined -fno-omit-frame-pointer} \
    -Itests/store_stubs -Isrc src/store.c tests/test_store.c -o "$test_bin"
"$test_bin"
