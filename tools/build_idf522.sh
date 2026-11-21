#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
idf_image=espressif/idf:v5.2.2@sha256:782e5a5dc615adffea73f8c9461573b6cb20e39464e7f928f3df68f5f65bdad9
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$project_dir:/project" -w /project "$idf_image" \
    idf.py -B build-idf522 -D SDKCONFIG=build-idf522/sdkconfig build size
