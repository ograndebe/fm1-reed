#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build and test everything that does not need the FM-1 hardware.
set -eu
cd "$(dirname "$0")/.."
TMP="${TMPDIR:-/tmp}/reed-tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

echo "== host engine =="
make test

echo "== renderer =="
make reed
./build/reed note --note 60 --vel 100 --seconds 1 --out "$TMP/c4.wav" >/dev/null
python3 tests/check_wav.py "$TMP/c4.wav" 44100

echo "== wasm =="
if command -v emcc >/dev/null 2>&1; then
    ./web/build.sh
    node web/test_wasm.mjs
else
    echo "emcc not found; skipping wasm build"
fi

echo "all checks passed"
