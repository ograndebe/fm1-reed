#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check a rendered WAV: 44.1 kHz, stereo, 16-bit, expected length, non-silent."""
import struct
import sys
import wave

path = sys.argv[1]
expect = int(sys.argv[2]) if len(sys.argv) > 2 else 44100
w = wave.open(path)
try:
    assert w.getframerate() == 44100, w.getframerate()
    assert w.getnchannels() == 2, w.getnchannels()
    assert w.getsampwidth() == 2, w.getsampwidth()
    n = w.getnframes()
    data = w.readframes(n)
finally:
    w.close()
assert n == expect, (n, expect)
samples = struct.unpack("<%dh" % (len(data) // 2), data)
peak = max(abs(v) for v in samples) / 32768.0
assert peak > 0.005, peak
print("%s: %d frames, peak %.3f" % (path, n, peak))
