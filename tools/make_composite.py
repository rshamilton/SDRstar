#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a SYNTHETIC wideband test recording from real narrowband recordings.

Real 48 kHz recordings (the DSD-neo fixtures: P25 control channel, DMR, D-STAR, NFM voice, ...) are placed at
chosen absolute frequencies inside one 2.4 MS/s capture, noise is added at a calibrated level and, optionally,
broadcast-FM-like wideband signals are synthesised.  The result is written as a 16-bit stereo WAV that SDR++'s File
Source plays like a real baseband recording.

THIS IS SYNTHETIC: it exists so the waterfall / detection / trunk-following logic can be tested without hardware
and with known ground truth.  It is not a substitute for real over-the-air recordings.

usage: make_composite.py OUT.wav --center 851.4e6 --rate 2.4e6 --seconds 12 \
           --sig p25p1_cqpsk_cc@851.1125e6 --sig dmr_voice@852.0e6 --fm 2e5@850.4e6 ...
"""
import argparse, os, sys, wave
import numpy as np
from scipy.signal import resample_poly, firwin

FIXTURES = os.environ.get("SDRSTAR_FIXTURES", "/home/user/ref/dsd-neo/tests/fixtures/iq")

LPF_HZ = 0.0

def load_fixture(name):
    raw = np.fromfile(os.path.join(FIXTURES, name + ".iq"), dtype=np.uint8).astype(np.float32) - 127.5
    x = (raw[0::2] + 1j * raw[1::2]).astype(np.complex64)
    if LPF_HZ > 0:
        # Some fixtures are re-modulated with a large deviation and occupy ~21 kHz; real 12.5 kHz-channel signals are
        # ~8-10 kHz wide.  Band-limit them so the scene looks like real signals on the waterfall.
        h = firwin(255, LPF_HZ, fs=48000.0)
        x = np.convolve(np.concatenate([x[-300:], x, x[:300]]), h, mode="same")[300:-300].astype(np.complex64)
    x /= np.sqrt(np.mean(np.abs(x) ** 2)) + 1e-9
    return x

def place(x48, n, offset_hz, rate, gain, loop=True, start=0):
    """Upsample a 48 kHz recording to `rate`, shift it by offset_hz, loop/pad to n samples."""
    up = int(round(rate / 48000.0))
    if abs(up * 48000.0 - rate) > 1:
        raise SystemExit("rate must be a multiple of 48 kHz (e.g. 2.4e6, 2.88e6)")
    y = resample_poly(x48, up, 1).astype(np.complex64)
    if loop:
        reps = int(np.ceil(n / len(y))) + 1
        y = np.tile(y, reps)[:n]
    else:
        y = np.concatenate([np.zeros(start, np.complex64), y])[:n]
        y = np.pad(y, (0, n - len(y)))
    t = np.arange(n) / rate
    return (gain * y * np.exp(2j * np.pi * offset_hz * t)).astype(np.complex64)

def fm_station(n, offset_hz, rate, dev=60e3, seed=0):
    """Broadcast-FM-like wideband signal: music-ish audio (band-limited noise + tones) FM modulated."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / rate
    a = rng.standard_normal(n // 25 + 1)
    a = np.convolve(a, np.ones(8) / 8, "same")
    a = np.repeat(a, 25)[:n]
    a /= np.max(np.abs(a)) + 1e-9
    audio = 0.6 * a + 0.3 * np.sin(2 * np.pi * 440 * t) + 0.1 * np.sin(2 * np.pi * 1200 * t)
    audio += 0.12 * np.sin(2 * np.pi * 19000 * t)  # stereo pilot
    phase = 2 * np.pi * dev * np.cumsum(audio) / rate
    return (np.exp(1j * (2 * np.pi * offset_hz * t + phase))).astype(np.complex64)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--center", type=float, required=True)
    ap.add_argument("--rate", type=float, default=2.4e6)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--sig", action="append", default=[], help="FIXTURE@FREQ_HZ[:gain_db]  (looped)")
    ap.add_argument("--once", action="append", default=[], help="FIXTURE@FREQ_HZ:start_s  (plays once starting at start_s)")
    ap.add_argument("--fm", action="append", default=[], help="BW@FREQ_HZ[:gain_db]  broadcast-FM-like signal (BW unused, 180 kHz)")
    ap.add_argument("--snr", type=float, default=35.0, help="SNR (dB) of a unit-RMS signal in 12.5 kHz")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--lpf", type=float, default=0.0, help="band-limit every placed recording to this cutoff (Hz) for realism")
    a = ap.parse_args()
    global LPF_HZ
    LPF_HZ = a.lpf

    n = int(a.rate * a.seconds)
    rng = np.random.default_rng(a.seed)
    # noise sigma so that a unit-RMS (0 dB gain) signal has `snr` dB in 12.5 kHz
    sigma2 = (12500.0 / a.rate) * 10 ** (-a.snr / 10)
    sigma = np.sqrt(sigma2 / 2)
    out = (rng.standard_normal(n) + 1j * rng.standard_normal(n)).astype(np.complex64) * np.float32(sigma)
    for s in a.sig:
        spec, _, g = s.partition(":")
        name, _, f = spec.partition("@")
        gain = 10 ** (float(g) / 20) if g else 1.0
        out += place(load_fixture(name), n, float(f) - a.center, a.rate, gain)
    for s in a.once:
        spec, _, st = s.partition(":")
        name, _, f = spec.partition("@")
        out += place(load_fixture(name), n, float(f) - a.center, a.rate, 1.0, loop=False, start=int(float(st) * a.rate))
    for s in a.fm:
        spec, _, g = s.partition(":")
        bw, _, f = spec.partition("@")
        gain = 10 ** (float(g) / 20) if g else 1.0
        # a broadcast carrier is far stronger than a 12.5 kHz signal's power density; scale to ~+20 dB over unit
        out += 4.0 * gain * fm_station(n, float(f) - a.center, a.rate, seed=len(out) % 97)
    peak = np.max(np.abs(np.concatenate([out.real, out.imag])))
    scale = 0.7 * 32767 / peak
    iq = np.empty(2 * n, dtype="<i2")
    iq[0::2] = np.round(out.real * scale)
    iq[1::2] = np.round(out.imag * scale)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(a.rate))
        w.writeframes(iq.tobytes())
    print(f"wrote {a.out}: {a.seconds}s at {a.rate/1e6} MS/s, centre {a.center/1e6} MHz, scale {scale:.1f}")

if __name__ == "__main__":
    main()
