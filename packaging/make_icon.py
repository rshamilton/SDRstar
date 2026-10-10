#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw the SDRstar app icon: a five-pointed star above a waterfall-display gradient.

Original artwork, generated procedurally with the Python standard library only (no PIL / numpy), so it runs
anywhere a python3 exists, including a stock CI runner.

    make_icon.py --png icon.png [--size 1024]          one PNG
    make_icon.py --iconset SDRstar.iconset             the ten PNGs iconutil expects
    make_icon.py --icns SDRstar.icns                   iconset + `iconutil -c icns` (macOS only)
"""

import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

# ---------------------------------------------------------------------------------------------------------------
# Tiny helpers
# ---------------------------------------------------------------------------------------------------------------


def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else hi if x > hi else x


def smoothstep(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0))
    return t * t * (3.0 - 2.0 * t)


def mix(a, b, t):
    return a + (b - a) * t


def hash01(ix, iy):
    """Deterministic pseudo-random number in [0, 1) for an integer cell."""
    h = (ix * 374761393 + iy * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFFFF) / float(0x1000000)


# Waterfall colour map (similar in spirit to SDR++'s "Classic"): dark blue -> blue -> cyan -> yellow -> white
_STOPS = [
    (0.00, (6, 10, 34)),
    (0.30, (10, 34, 120)),
    (0.55, (0, 150, 210)),
    (0.78, (250, 214, 70)),
    (1.00, (255, 255, 255)),
]


def colormap(t):
    t = clamp(t)
    for (t0, c0), (t1, c1) in zip(_STOPS, _STOPS[1:]):
        if t <= t1:
            k = (t - t0) / (t1 - t0)
            return (mix(c0[0], c1[0], k), mix(c0[1], c1[1], k), mix(c0[2], c1[2], k))
    return _STOPS[-1][1]


# Signals drawn into the waterfall: (centre u, width, strength, drift amplitude, drift frequency)
_SIGNALS = [
    (0.18, 0.016, 0.85, 0.010, 7.0),
    (0.33, 0.010, 0.60, 0.006, 11.0),
    (0.50, 0.024, 1.00, 0.012, 5.0),
    (0.67, 0.012, 0.70, 0.008, 9.0),
    (0.83, 0.018, 0.80, 0.010, 6.0),
]


def waterfall(u, v, rows):
    """Waterfall intensity in [0, 1] at body coordinates (u, v), v = 0 at the top (newest line)."""
    row = int(v * rows)
    colbin = int(u * rows * 1.0)
    floor = 0.10 + 0.14 * hash01(colbin, row)
    level = floor
    for cu, w, a, drift, freq in _SIGNALS:
        centre = cu + drift * math.sin(v * freq + cu * 20.0)
        d = (u - centre) / w
        if abs(d) < 4.0:
            burst = 0.55 + 0.45 * hash01(row, int(cu * 100))
            level += a * burst * math.exp(-d * d)
    return clamp(level * (0.95 - 0.30 * v))


# ---------------------------------------------------------------------------------------------------------------
# Signed distance functions
# ---------------------------------------------------------------------------------------------------------------


def sd_round_box(px, py, half, radius):
    qx = abs(px) - half + radius
    qy = abs(py) - half + radius
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    inside = min(max(qx, qy), 0.0)
    return outside + inside - radius


def _star_polygon(cx, cy, r_outer, r_inner):
    pts = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        r = r_outer if i % 2 == 0 else r_inner
        pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    return pts


def sd_polygon(px, py, pts):
    """Signed distance to a simple polygon (negative inside)."""
    d = (px - pts[0][0]) ** 2 + (py - pts[0][1]) ** 2
    s = 1.0
    n = len(pts)
    j = n - 1
    for i in range(n):
        ex, ey = pts[j][0] - pts[i][0], pts[j][1] - pts[i][1]
        wx, wy = px - pts[i][0], py - pts[i][1]
        t = clamp((wx * ex + wy * ey) / (ex * ex + ey * ey))
        bx, by = wx - ex * t, wy - ey * t
        d = min(d, bx * bx + by * by)
        c1 = py >= pts[i][1]
        c2 = py < pts[j][1]
        c3 = ex * wy > ey * wx
        if (c1 and c2 and c3) or (not c1 and not c2 and not c3):
            s = -s
        j = i
    return s * math.sqrt(d)


# ---------------------------------------------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------------------------------------------


def render(size):
    """Return RGBA bytes (size*size*4) for a square icon."""
    body = 0.805  # macOS icon grid: the artwork fills ~80 % of the canvas
    half = body / 2.0
    radius = 0.225 * body
    star_c = (0.0, -0.095)
    star = _star_polygon(star_c[0], star_c[1], 0.235, 0.235 * 0.40)
    rows = 56
    aa = 1.0 / size  # one pixel in canvas units
    out = bytearray(size * size * 4)
    for y in range(size):
        py = (y + 0.5) / size - 0.5
        base = y * size * 4
        for x in range(size):
            px = (x + 0.5) / size - 0.5
            d_body = sd_round_box(px, py, half, radius)
            cov = clamp(0.5 - d_body / aa)
            if cov <= 0.0:
                continue  # fully transparent margin (bytearray is zero-initialised)
            u = (px + half) / body
            v = (py + half) / body
            r, g, b = colormap(waterfall(u, v, rows))
            # darken the top so the star stands out, add a soft vertical vignette
            shade = 0.80 * (1.0 - smoothstep(0.0, 0.62, v)) + 0.18 * smoothstep(0.80, 1.0, v)
            r, g, b = r * (1.0 - shade), g * (1.0 - shade), b * (1.0 - shade * 0.9)
            # glow behind the star
            ds = sd_polygon(px, py, star)
            glow = math.exp(-max(ds, 0.0) * 14.0) * 0.55
            r, g, b = mix(r, 255.0, glow * 0.85), mix(g, 232.0, glow * 0.85), mix(b, 160.0, glow * 0.55)
            # the star itself: warm white at the top fading to gold at the bottom, anti-aliased edge
            sc = clamp(0.5 - ds / aa)
            if sc > 0.0:
                k = clamp((py - (star_c[1] - 0.235)) / 0.47)
                sr, sg, sb = mix(255.0, 255.0, k), mix(250.0, 196.0, k), mix(225.0, 58.0, k)
                r, g, b = mix(r, sr, sc), mix(g, sg, sc), mix(b, sb, sc)
            # thin lighter rim on the body edge
            rim = smoothstep(-0.012, -0.002, d_body) * (1.0 - smoothstep(-0.002, 0.0, d_body))
            r, g, b = mix(r, 255.0, rim * 0.18), mix(g, 255.0, rim * 0.18), mix(b, 255.0, rim * 0.18)
            i = base + x * 4
            out[i] = int(clamp(r, 0, 255))
            out[i + 1] = int(clamp(g, 0, 255))
            out[i + 2] = int(clamp(b, 0, 255))
            out[i + 3] = int(cov * 255.0 + 0.5)
    return bytes(out)


def write_png(path, size, rgba):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)  # filter type 0
        raw += rgba[y * stride:(y + 1) * stride]
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as fh:
        fh.write(png)


ICONSET_FILES = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]


def write_iconset(directory):
    os.makedirs(directory, exist_ok=True)
    cache = {}
    for name, size in ICONSET_FILES:
        target = os.path.join(directory, name)
        if size in cache:
            shutil.copyfile(cache[size], target)
        else:
            write_png(target, size, render(size))
            cache[size] = target


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--png", help="write a single PNG")
    ap.add_argument("--size", type=int, default=1024, help="size for --png (default 1024)")
    ap.add_argument("--iconset", help="write an .iconset directory")
    ap.add_argument("--icns", help="write an .icns file (needs macOS iconutil)")
    args = ap.parse_args()
    if not (args.png or args.iconset or args.icns):
        ap.error("give --png, --iconset or --icns")
    if args.png:
        write_png(args.png, args.size, render(args.size))
        print("wrote", args.png)
    if args.iconset:
        write_iconset(args.iconset)
        print("wrote", args.iconset)
    if args.icns:
        if not shutil.which("iconutil"):
            sys.exit("ERROR: iconutil not found (it ships with macOS); cannot create %s" % args.icns)
        with tempfile.TemporaryDirectory() as tmp:
            iconset = os.path.join(tmp, "SDRstar.iconset")
            write_iconset(iconset)
            subprocess.run(["iconutil", "-c", "icns", "-o", args.icns, iconset], check=True)
        print("wrote", args.icns)


if __name__ == "__main__":
    main()
