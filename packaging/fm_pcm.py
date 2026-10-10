#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert a 48 kHz unsigned-8-bit interleaved I/Q capture into FM-discriminator PCM (signed 16-bit little endian).

This is exactly what the SDRstar module feeds to DSD-neo (compare core/include/sdrstar/dsp_lite.h):

    x   = I + jQ,  with I,Q = (byte - 127.5) / 127.5
    d   = angle(x[n] * conj(x[n-1])) * (48000 / (2 * pi * 6250))      # 6.25 kHz deviation == 1.0
    pcm = clip(round(d * 32767))

Pure standard library (array, cmath) so it works on a stock macOS python3 with no extra packages.
Usage: fm_pcm.py <input.iq> <output.pcm>
"""
import array
import cmath
import math
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as fh:
        raw = fh.read()
    if len(raw) < 4:
        sys.exit("ERROR: %s is too short to be an I/Q capture" % sys.argv[1])
    scale = 48000.0 / (2.0 * math.pi * 6250.0)
    out = array.array("h")
    prev = complex((raw[0] - 127.5) / 127.5, (raw[1] - 127.5) / 127.5)
    for i in range(2, len(raw) - 1, 2):
        cur = complex((raw[i] - 127.5) / 127.5, (raw[i + 1] - 127.5) / 127.5)
        d = cmath.phase(cur * prev.conjugate()) * scale
        prev = cur
        v = int(round(d * 32767.0))
        out.append(32767 if v > 32767 else -32768 if v < -32768 else v)
    if sys.byteorder == "big":
        out.byteswap()
    with open(sys.argv[2], "wb") as fh:
        out.tofile(fh)


if __name__ == "__main__":
    main()
