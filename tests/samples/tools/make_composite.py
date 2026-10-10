#!/usr/bin/env python3
"""make_composite.py - build SYNTHETIC wideband IQ from narrowband captures (and the reverse: extract a channel).

*** SYNTHETIC DATA WARNING ***
Everything produced by the `build` sub-command is a SYNTHETIC composite: real narrowband
recordings (e.g. the 48 kHz cu8 channelized dsd-neo fixtures) are resampled to the wideband
rate, frequency-shifted to chosen offsets, scaled to a chosen SNR, summed with white Gaussian
noise and quantized.  The result exercises detection / channelization / classification code
with *known ground truth* (the JSON sidecar lists every injected signal), but it is NOT an
over-the-air wideband capture: there is no front-end filter shape, no ADC non-linearity, no
adjacent-channel splatter, no frequency-dependent gain, no real-world interference.  Do NOT
use a composite as evidence that the real receiver chain works; use it for deterministic
unit/integration tests.  The sidecar says "synthetic": true and so does this docstring.

Sub-commands
  build    make a composite file from several narrowband inputs
  extract  channelize one narrowband slice out of a (real or synthetic) wideband file
           (used to feed decoders such as dsd-neo --iq-replay with a 48 kHz cu8 channel)
  slice    cut an RTL-SDR-like wideband slice (e.g. 2.4 MS/s cu8) out of a wider REAL capture
           (e.g. a 20 MS/s HackRF recording): shift, filter, resample, rescale, quantize.  The data
           stays real RF (not synthetic) but it is resampled/rescaled, so the sidecar says "derived".

build examples
  # 2.4 MS/s cu8 centred on 851.0 MHz with a P25 control channel at +300 kHz (SNR 30 dB) and a
  # DMR voice channel at -450 kHz (SNR 20 dB), 3 s long, noise floor -50 dBFS/total, then a
  # ground-truth sidecar comp.cu8.json
  make_composite.py build --out /tmp/comp.cu8 --format cu8 --rate 2400000 --duration 3 \
      --center 851000000 --noise-dbfs -45 --seed 1 \
      --add /home/user/ref/dsd-neo/tests/fixtures/iq/p25p1_c4fm_cc.iq:300e3:30:name=p25_cc:loop \
      --add /home/user/ref/dsd-neo/tests/fixtures/iq/dmr_voice.iq:-450e3:20:name=dmr_vc:loop

  --add SPEC     SPEC = path:offset_hz:snr_db[:name=NAME][:start=SEC][:loop][:swap]
                 path ending .iq / .cu8 -> cu8 narrowband capture; its rate comes from the
                 dsd-neo sidecar <path>.json (or <path>.iq.json) "sample_rate_hz", else --in-rate.
                 path ending .wav -> 2-channel WAV IQ (rate from header).
                 path ending .cf32 / .cs16 / .cs8 -> raw, rate must be given with --in-rate.
                 snr_db is the in-channel SNR: signal power (RMS over the source's active
                 samples) divided by the noise power that falls inside the source's own
                 bandwidth (= its sample rate) in the composite noise floor.
  --format       cu8 | cs8 | cs16 | cf32 | wav16 (2-ch WAV, 16 bit)

extract example
  make_composite.py extract --in /home/user/samples/foo/wide.cu8 --format cu8 --in-rate 2400000 \
      --offset -450e3 --out-rate 48000 --out /tmp/ch.iq --out-center 851000000 --dsd-sidecar
  Writes a cu8 (or cf32) narrowband file; with --dsd-sidecar also writes the dsd-neo "iq.json"
  sidecar so `dsd-neo --iq-replay /tmp/ch.iq.json` can decode it.

Run python3 -I when pointing this at untrusted recordings.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from fractions import Fraction

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from inspect_iq import load_iq  # noqa: E402

try:
    from scipy import signal as sps
except Exception:  # pragma: no cover
    sps = None


# ---------------------------------------------------------------- helpers

def _rate_from_sidecar(path: str):
    for cand in (path + ".json", os.path.splitext(path)[0] + ".iq.json", path.replace(".iq", ".iq.json")):
        if os.path.exists(cand):
            try:
                with open(cand) as f:
                    j = json.load(f)
                return float(j["sample_rate_hz"]), j
            except Exception:
                pass
    return None, None


def load_narrow(path: str, in_rate: float | None = None, swap: bool = False):
    """Load a narrowband capture as complex64 -> (x, rate_hz, sidecar_dict_or_None)."""
    ext = os.path.splitext(path)[1].lower().lstrip(".")
    side = None
    if ext in ("iq", "cu8", "u8"):
        rate, side = _rate_from_sidecar(path)
        rate = rate or in_rate
        if rate is None:
            raise ValueError(f"{path}: no sidecar json and no --in-rate")
        x, _ = load_iq(path, "cu8", rate, swap_iq=swap)
    elif ext == "wav":
        x, info = load_iq(path, "wav", swap_iq=swap)
        rate = info["sample_rate_hz"]
    elif ext in ("cf32", "cs16", "cs8", "raw", "cfile"):
        if in_rate is None:
            raise ValueError(f"{path}: raw format needs --in-rate")
        x, _ = load_iq(path, ext if ext not in ("raw", "cfile") else "cf32", in_rate, swap_iq=swap)
        rate = in_rate
    else:
        raise ValueError(f"{path}: unknown extension")
    return x, float(rate), side


def _ratio(fs_out: float, fs_in: float):
    fr = Fraction(fs_out / fs_in).limit_denominator(2000)
    return fr.numerator, fr.denominator


def resample(x: np.ndarray, fs_in: float, fs_out: float) -> np.ndarray:
    if abs(fs_in - fs_out) < 1e-6:
        return x.astype(np.complex64)
    up, down = _ratio(fs_out, fs_in)
    if sps is None:
        raise RuntimeError("scipy required (pip install --break-system-packages scipy)")
    y = sps.resample_poly(x, up, down).astype(np.complex64)
    return y


def freq_shift(x: np.ndarray, fs: float, offset_hz: float, phase0: float = 0.0, n0: int = 0) -> np.ndarray:
    n = np.arange(n0, n0 + len(x), dtype=np.float64)
    return (x * np.exp(1j * (2 * np.pi * offset_hz * n / fs + phase0))).astype(np.complex64)


def quantize(x: np.ndarray, fmt: str):
    """complex64 (full scale +-1.0) -> raw bytes. Returns (bytes, clip_fraction)."""
    iq = np.empty(2 * len(x), np.float32)
    iq[0::2], iq[1::2] = x.real, x.imag
    if fmt == "cf32":
        return iq.astype("<f4").tobytes(), float(np.mean(np.abs(iq) > 1.0))
    if fmt == "cu8":
        v = np.round(iq * 127.5 + 127.5)
        clip = float(np.mean((v < 0) | (v > 255)))
        return np.clip(v, 0, 255).astype(np.uint8).tobytes(), clip
    if fmt == "cs8":
        v = np.round(iq * 127.0)
        clip = float(np.mean(np.abs(v) > 127))
        return np.clip(v, -128, 127).astype(np.int8).tobytes(), clip
    if fmt in ("cs16", "wav16"):
        v = np.round(iq * 32767.0)
        clip = float(np.mean(np.abs(v) > 32767))
        return np.clip(v, -32768, 32767).astype("<i2").tobytes(), clip
    raise ValueError(fmt)


def write_wav16(path: str, raw: bytes, rate: int):
    import struct
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(raw)))
        f.write(raw)


# ---------------------------------------------------------------- build

def parse_add(spec: str):
    parts = spec.split(":")
    # path may contain ':'? (no) -> first field path
    d = {"path": parts[0], "offset_hz": float(parts[1]), "snr_db": float(parts[2]),
         "name": os.path.basename(parts[0]), "start_s": 0.0, "loop": False, "swap": False}
    for p in parts[3:]:
        if p == "loop":
            d["loop"] = True
        elif p == "swap":
            d["swap"] = True
        elif p.startswith("name="):
            d["name"] = p[5:]
        elif p.startswith("start="):
            d["start_s"] = float(p[6:])
        else:
            raise ValueError(f"unknown --add option {p!r}")
    return d


def build(args):
    rng = np.random.default_rng(args.seed)
    fs = float(args.rate)
    n = int(round(args.duration * fs))
    sigma = 10 ** (args.noise_dbfs / 20.0)  # complex noise RMS (total, full-band), re full-scale 1.0
    # complex Gaussian noise with total power sigma^2 spread over fs
    noise = (rng.standard_normal(n) + 1j * rng.standard_normal(n)).astype(np.complex64) * np.float32(sigma / math.sqrt(2))
    y = noise.copy()
    truth = []
    for spec in args.add:
        a = parse_add(spec)
        xs, rs, side = load_narrow(a["path"], args.in_rate, a["swap"])
        # remove the DC bias of 8-bit captures (mean of |x| window) so the shifted signal has no stray carrier
        xs = xs - xs.mean()
        # active-sample power: RMS over samples above 10% of the median-peak envelope (ignores gaps)
        env = np.abs(xs)
        thr = 0.1 * np.percentile(env, 99)
        act = env > thr
        p_sig = float(np.mean(env[act] ** 2)) if act.any() else float(np.mean(env ** 2))
        # noise power inside the source bandwidth (= rs) in the composite floor
        p_noise_in_bw = sigma ** 2 * (rs / fs)
        target_p = p_noise_in_bw * 10 ** (a["snr_db"] / 10.0)
        gain = math.sqrt(target_p / max(p_sig, 1e-30))
        xw = resample(xs * np.float32(gain), rs, fs)
        start = int(round(a["start_s"] * fs))
        need = n - start
        if need <= 0:
            continue
        if len(xw) < need and a["loop"]:
            reps = int(math.ceil(need / len(xw)))
            xw = np.tile(xw, reps)
        seg = xw[:need]
        seg = freq_shift(seg, fs, a["offset_hz"], n0=start)
        y[start:start + len(seg)] += seg
        truth.append({"name": a["name"], "source": os.path.abspath(a["path"]), "offset_hz": a["offset_hz"],
                      "snr_db_in_channel": a["snr_db"], "source_rate_hz": rs, "bandwidth_hz": rs,
                      "start_s": a["start_s"], "source_duration_s": len(xs) / rs, "looped": a["loop"],
                      "scale_gain": gain})
    if args.format in ("cu8", "cs8") and sigma / math.sqrt(2) * (127.5 if args.format == "cu8" else 127.0) < 1.5:
        print("WARNING: composite noise std < 1.5 LSB per rail: 8-bit quantization noise will dominate the floor; "
              "use --noise-dbfs >= -36 for realistic RTL-like cu8", file=sys.stderr)
    if args.dc_dbfs is not None:
        y += np.complex64(10 ** (args.dc_dbfs / 20.0))
    if args.gain_db:
        y *= np.float32(10 ** (args.gain_db / 20.0))
    raw, clip = quantize(y, args.format)
    out = args.out
    if args.format == "wav16":
        write_wav16(out, raw, int(fs))
    else:
        with open(out, "wb") as f:
            f.write(raw)
    side = {
        "synthetic": True,
        "generator": "make_composite.py build (SYNTHETIC composite of real narrowband captures + AWGN)",
        "file": os.path.basename(out), "format": args.format, "sample_rate_hz": fs,
        "center_freq_hz": args.center, "duration_s": n / fs, "noise_dbfs_total": args.noise_dbfs,
        "seed": args.seed, "quantization_clip_fraction": clip, "signals": truth,
    }
    with open(out + ".json", "w") as f:
        json.dump(side, f, indent=2)
    print(f"wrote {out} ({os.path.getsize(out)} bytes, {n/fs:.2f} s @ {fs:.0f} Hz, clip {clip:.2e}) + {out}.json")
    return 0


# ---------------------------------------------------------------- extract

def channelize(x: np.ndarray, fs: float, offset_hz: float, out_rate: float = 48000.0, agc: bool = True,
               block: int = 1 << 22) -> np.ndarray:
    """Shift `offset_hz` to DC, low-pass and decimate/resample to `out_rate` -> complex64 (REAL data, no synthesis).

    Used to hand a decoder (dsd-neo/dsd-fme/multimon-ng) one narrowband channel of a wideband capture.
    Memory-lean: the first (largest) decimation stage runs block-wise (a few-sample filter transient at each
    4M-sample block boundary is ignored; irrelevant for decoder feeding).
    """
    if sps is None:
        raise RuntimeError("scipy required")
    n = len(x)
    dc = complex(np.mean(x[: min(n, 1 << 24)]))
    dec = max(1, int(fs // (out_rate * 8)))
    facs, d = [], dec
    for p in (13, 11, 10, 8, 7, 5, 4, 3, 2):
        while d % p == 0 and d > 1:
            facs.append(p)
            d //= p
    if d > 1:
        facs.append(d)
    facs.sort(reverse=True)
    w = 2.0 * np.pi * offset_hz / fs
    parts = []
    q0 = facs[0] if facs else 1
    for pos in range(0, n, block):
        blk = x[pos: pos + block].astype(np.complex64) - np.complex64(dc)
        t = np.arange(pos, pos + len(blk), dtype=np.float64)
        blk = (blk * np.exp(-1j * np.mod(w * t, 2 * np.pi)).astype(np.complex64)).astype(np.complex64)
        if q0 > 1:
            blk = sps.decimate(blk, q0, ftype="fir", zero_phase=False).astype(np.complex64)
        parts.append(blk)
    x = np.concatenate(parts)
    fs2 = fs / q0
    for q in facs[1:]:
        x = sps.decimate(x, q, ftype="fir", zero_phase=False).astype(np.complex64)
        fs2 /= q
    x = resample(x, fs2, out_rate)
    h = sps.firwin(255, 0.45 * out_rate, fs=out_rate)
    x = sps.lfilter(h, [1.0], x).astype(np.complex64)
    if agc:
        pk = np.percentile(np.abs(x), 99.9)
        if pk > 0:
            x = x * np.float32(0.5 / pk)
    return x


def extract(args):
    fs = float(args.in_rate) if args.in_rate else None
    fmt = args.format
    x, info = load_iq(args.inp, fmt, fs, start_s=args.start, duration_s=args.duration, swap_iq=args.swap_iq)
    fs = info["sample_rate_hz"]
    out_rate = float(args.out_rate)
    x = channelize(x, fs, args.offset, out_rate, args.agc)
    raw, clip = quantize(x, args.out_format)
    with open(args.out, "wb") as f:
        f.write(raw)
    print(f"wrote {args.out}: {len(x)} samples @ {out_rate:.0f} Hz ({len(x)/out_rate:.2f} s), clip {clip:.2e}")
    if args.dsd_sidecar:
        side = {
            "format": "dsd-neo-iq", "version": 1, "sample_format": args.out_format, "iq_order": "IQ", "endianness": "none",
            "capture_stage": "post_mute_pre_widen", "sample_rate_hz": int(out_rate),
            "center_frequency_hz": int(args.out_center or 0), "capture_center_frequency_hz": int(args.out_center or 0),
            "ppm": 0, "tuner_gain_tenth_db": 270, "rtl_dsp_bw_khz": int(round(out_rate / 1000)), "base_decimation": 1,
            "post_downsample": 1, "demod_rate_hz": int(out_rate), "offset_tuning_enabled": False,
            "fs4_shift_enabled": False, "combine_rotate_enabled": False, "muted_bytes_excluded": True,
            "contains_retunes": False, "capture_retune_count": 0, "source_backend": "rtl", "source_args": "dev=0",
            "capture_started_utc": "2026-01-01T00:00:00Z", "data_file": os.path.basename(args.out),
            "data_bytes": len(raw), "capture_drops": 0, "capture_drop_blocks": 0, "input_ring_drops": 0,
            "notes": "extracted by make_composite.py extract",
        }
        sp = os.path.splitext(args.out)[0] + ".iq.json" if args.out.endswith(".iq") else args.out + ".json"
        with open(sp, "w") as f:
            json.dump(side, f, indent=2)
        print("wrote sidecar", sp)
    return 0


def slice_cmd(args):
    """REAL-data slicer: wide capture -> narrower RTL-like capture centred elsewhere."""
    fs = float(args.in_rate) if args.in_rate else None
    x, info = load_iq(args.inp, args.format, fs, start_s=args.start, duration_s=args.duration, swap_iq=args.swap_iq)
    fs = info["sample_rate_hz"]
    off = args.center - args.in_center
    y = channelize(x, fs, off, args.rate, agc=False)
    trim = int(args.trim * args.rate)
    if trim:
        y = y[trim:-trim]
    env = np.maximum(np.abs(y.real), np.abs(y.imag))
    pk = float(np.percentile(env, 99.99))
    target = args.peak_lsb / 127.5 if args.format_out == "cu8" else 0.78
    g = target / max(pk, 1e-12)
    y = (y * np.float32(g)).astype(np.complex64)
    raw, clip = quantize(y, args.format_out)
    with open(args.out, "wb") as f:
        f.write(raw)
    side = {
        "synthetic": False, "derived": True,
        "generator": "make_composite.py slice (real RF, resampled + rescaled)",
        "source_file": os.path.abspath(args.inp), "source_rate_hz": fs, "source_center_hz": args.in_center,
        "source_start_s": args.start, "file": os.path.basename(args.out), "format": args.format_out,
        "sample_rate_hz": args.rate, "center_freq_hz": args.center, "offset_from_source_center_hz": off,
        "duration_s": len(y) / args.rate, "scale_gain": float(g), "quantization_clip_fraction": float(clip),
    }
    with open(args.out + ".json", "w") as f:
        json.dump(side, f, indent=2)
    print(f"wrote {args.out}: {len(y)/args.rate:.2f} s @ {args.rate:.0f} Hz centre {args.center/1e6:.4f} MHz, clip {clip:.2e}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="SYNTHETIC composite builder")
    b.add_argument("--out", required=True)
    b.add_argument("--format", default="cu8", choices=["cu8", "cs8", "cs16", "cf32", "wav16"])
    b.add_argument("--rate", type=float, default=2.4e6)
    b.add_argument("--duration", type=float, default=2.0)
    b.add_argument("--center", type=float, default=None, help="label only (Hz)")
    b.add_argument("--noise-dbfs", type=float, default=-45.0, help="total complex noise power, dBFS (full-scale tone=0)")
    b.add_argument("--dc-dbfs", type=float, default=None, help="optional DC offset amplitude, dBFS")
    b.add_argument("--gain-db", type=float, default=0.0, help="final gain before quantizing")
    b.add_argument("--seed", type=int, default=0)
    b.add_argument("--in-rate", type=float, default=None, help="rate for raw inputs without sidecar")
    b.add_argument("--add", action="append", default=[], help="path:offset_hz:snr_db[:name=..][:start=..][:loop][:swap]")
    e = sub.add_parser("extract", help="channelize a narrowband slice out of a wideband file")
    e.add_argument("--in", dest="inp", required=True)
    e.add_argument("--format", required=False, default=None, help="cu8|cs8|cs16|cf32|wav (default from extension)")
    e.add_argument("--in-rate", type=float, default=None)
    e.add_argument("--offset", type=float, required=True, help="channel offset from the file centre, Hz")
    e.add_argument("--out-rate", type=float, default=48000)
    e.add_argument("--out", required=True)
    e.add_argument("--out-format", default="cu8", choices=["cu8", "cf32", "cs16"])
    e.add_argument("--out-center", type=float, default=None)
    e.add_argument("--start", type=float, default=0.0)
    e.add_argument("--duration", type=float, default=None)
    e.add_argument("--swap-iq", action="store_true")
    e.add_argument("--agc", action="store_true", default=True)
    e.add_argument("--no-agc", dest="agc", action="store_false")
    e.add_argument("--dsd-sidecar", action="store_true")
    sl = sub.add_parser("slice", help="cut an RTL-like slice out of a wider REAL capture (derived data)")
    sl.add_argument("--in", dest="inp", required=True)
    sl.add_argument("--format", default=None)
    sl.add_argument("--in-rate", type=float, default=None)
    sl.add_argument("--in-center", type=float, required=True, help="centre frequency of the input file, Hz")
    sl.add_argument("--center", type=float, required=True, help="new centre frequency, Hz")
    sl.add_argument("--rate", type=float, default=2.4e6)
    sl.add_argument("--out", required=True)
    sl.add_argument("--format-out", default="cu8", choices=["cu8", "cs16", "cf32"])
    sl.add_argument("--peak-lsb", type=float, default=100.0, help="cu8: map the 99.99th-pct |I|,|Q| to this many LSB")
    sl.add_argument("--start", type=float, default=0.0)
    sl.add_argument("--duration", type=float, default=None)
    sl.add_argument("--trim", type=float, default=0.02, help="seconds trimmed at both ends (filter transients)")
    sl.add_argument("--swap-iq", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "slice":
        return slice_cmd(a)
    if a.cmd == "build":
        if not a.add:
            ap.error("build needs at least one --add")
        return build(a)
    return extract(a)


if __name__ == "__main__":
    sys.exit(main())
