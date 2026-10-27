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
python3 - "$TMP/c4.wav" <<'PY'
import struct, sys, wave
p = sys.argv[1]
w = wave.open(p)
assert w.getframerate() == 44100 and w.getnchannels() == 2 and w.getsampwidth() == 2, w.getparams()
n = w.getnframes()
d = w.readframes(n)
w.close()
assert n == 44100, n
s = struct.unpack("<%dh" % (len(d) // 2), d)
peak = max(abs(v) for v in s) / 32768.0
assert peak > 0.005, peak
print("c4.wav: %d frames, peak %.3f" % (n, peak))
PY

echo "== wasm =="
if command -v emcc >/dev/null 2>&1; then
    ./web/build.sh
    node web/test_wasm.mjs
else
    echo "emcc not found; skipping wasm build"
fi

echo "all checks passed"
