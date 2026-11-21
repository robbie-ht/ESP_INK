#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh tests/test_store.sh
sh tests/test_settings.sh
sh tests/test_cache.sh
python3 tests/test_ble_protocol.py
python3 sdk/test_frame.py
python3 sdk/test_ble.py
node --experimental-transform-types --test sdk/test_ble.ts
