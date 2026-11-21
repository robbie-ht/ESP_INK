#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
settings_test_dir=$(mktemp -d)
trap 'rm -rf "$settings_test_dir"' EXIT
cc -std=gnu11 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-pie -no-pie -Itests/settings_stubs -Itests/store_stubs -Isrc tests/test_settings.c src/settings.c -o "$settings_test_dir/settings"
"$settings_test_dir/settings"
