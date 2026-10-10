#!/usr/bin/env python3
"""inspect_iq.py - load, sanity-check and plot IQ recordings from /home/user/samples.

Loads ANY manifest entry (or a bare file) as a complex64 numpy array and can
print health statistics, an averaged (Welch) power spectrum and a waterfall.

Supported containers / sample formats
    cu8   interleaved unsigned 8-bit  (I,Q,I,Q,...)  RTL-SDR / rtl_433 native, 127.5 = zero
    cs8   interleaved signed 8-bit    (HackRF / hackrf_transfer)
    cs16  interleaved signed 16-bit little-endian
    cf32  interleaved float32 little-endian (GNU Radio "cfile", gqrx .raw)
    wav   2-channel RIFF/WAVE: PCM 8 (unsigned) / 16 / 24 / 32 bit, or float32.
          Channel 0 (left) is taken as I and channel 1 (right) as Q, which is
          what SDR#, HDSDR, SDRuno and SDR++ write.  Use --swap-iq if a
          recording turns out mirrored (see README "lessons").

Usage examples
    # bare file, explicit format
    inspect_iq.py --file x.cu8 --format cu8 --rate 2400000 --center 1090e6 --plot out.png
    # a manifest entry (rate/format/center come from MANIFEST.json)
    inspect_iq.py --id fm_106100_sdrsharp --plot /tmp/fm.png --waterfall
    # only the first 2 s, print numbers, no plot
    inspect_iq.py --id adsb_modes1 --duration 2 --stats-only

Library use
    import sys; sys.path.insert(0, '/home/user/samples/tools')
    from inspect_iq import load_entry, load_iq, welch_psd
    x, meta = load_entry('fm_106100_sdrsharp')           # complex64, meta dict
    f, p_db = welch_psd(x, meta['sample_rate_hz'])

Everything here only READS recordings; it never executes anything from them.
Run it as `python3 -I inspect_iq.py ...` when pointing it at untrusted files.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

import numpy as np

SAMPLES_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
MANIFEST = os.path.join(SAMPLES_ROOT, "MANIFEST.json")

# --------------------------------------------------------------------------
# RIFF/WAVE parsing (own parser: wave.py cannot do 24-bit float, WAVE_FORMAT_EXTENSIBLE
# or truncated/zero-length data chunks, and SDR recorders often produce those)
# --------------------------------------------------------------------------


def parse_wav_header(path: str) -> dict:
    """Return dict(sample_rate, channels, bits, fmt_tag, data_offset, data_bytes, auxi_center_hz ...)."""
    info = {}
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        hdr = f.read(12)
        if hdr[:4] not in (b"RIFF", b"RF64") or hdr[8:12] != b"WAVE":
            raise ValueError(f"{path}: not a RIFF/WAVE file (magic={hdr[:4]!r})")
        pos = 12
        while pos + 8 <= size:
            f.seek(pos)
            ch = f.read(8)
            if len(ch) < 8:
                break
            cid, csz = ch[:4], struct.unpack("<I", ch[4:8])[0]
            if cid == b"fmt ":
                b = f.read(min(csz, 40))
                tag, nch, sr, _br, _ba, bits = struct.unpack("<HHIIHH", b[:16])
                if tag == 0xFFFE and len(b) >= 26:  # EXTENSIBLE: real tag = first 2 bytes of GUID
                    tag = struct.unpack("<H", b[24:26])[0]
                info.update(fmt_tag=tag, channels=nch, sample_rate=sr, bits=bits)
            elif cid == b"auxi":  # SpectraVue/HDSDR/SDR# auxiliary chunk: 2xSYSTEMTIME, then center freq
                b = f.read(min(csz, 64))
                if len(b) >= 36:
                    info["auxi_center_hz"] = struct.unpack("<I", b[32:36])[0]
                    st = struct.unpack("<8H", b[:16])
                    info["auxi_start_utc"] = "%04d-%02d-%02dT%02d:%02d:%02d" % (st[0], st[1], st[3], st[4], st[5], st[6])
            elif cid == b"data":
                avail = size - (pos + 8)
                info["data_offset"] = pos + 8
                info["data_bytes_header"] = csz
                # streaming recorders leave 0 / 0xFFFFFFFF here -> trust file length
                info["data_bytes"] = avail if (csz == 0 or csz == 0xFFFFFFFF or csz > avail) else csz
                info["data_truncated"] = bool(csz > avail and csz != 0xFFFFFFFF)
                break
            pos += 8 + csz + (csz & 1)
    if "data_offset" not in info or "sample_rate" not in info:
        raise ValueError(f"{path}: no fmt/data chunk found")
    return info


def _wav_frames(path: str, h: dict, start: int, count: int | None) -> np.ndarray:
    nch, bits, tag = h["channels"], h["bits"], h["fmt_tag"]
    if nch != 2:
        raise ValueError(f"{path}: {nch}-channel WAV; need 2 channels (I,Q) for IQ loading")
    bps = bits // 8
    frame = nch * bps
    total = h["data_bytes"] // frame
    start = min(start, total)
    n = total - start if count is None else min(count, total - start)
    off = h["data_offset"] + start * frame
    if bits == 8:
        a = np.fromfile(path, dtype=np.uint8, count=n * 2, offset=off).astype(np.float32)
        a = (a - 127.5) / 127.5
    elif bits == 16:
        a = np.fromfile(path, dtype="<i2", count=n * 2, offset=off).astype(np.float32) / 32768.0
    elif bits == 24:
        raw = np.fromfile(path, dtype=np.uint8, count=n * 2 * 3, offset=off).reshape(-1, 3).astype(np.int32)
        v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        a = v.astype(np.float32) / 8388608.0
    elif bits == 32 and tag == 3:
        a = np.fromfile(path, dtype="<f4", count=n * 2, offset=off)
    elif bits == 32:
        a = np.fromfile(path, dtype="<i4", count=n * 2, offset=off).astype(np.float32) / 2147483648.0
    elif bits == 64 and tag == 3:
        a = np.fromfile(path, dtype="<f8", count=n * 2, offset=off).astype(np.float32)
    else:
        raise ValueError(f"{path}: unsupported WAV {bits}-bit tag {tag}")
    return a


# --------------------------------------------------------------------------
# Generic loader
# --------------------------------------------------------------------------

RAW_SPECS = {  # fmt -> (numpy dtype, bytes per complex sample)
    "cu8": (np.uint8, 2),
    "cs8": (np.int8, 2),
    "cs16": ("<i2", 4),
    "cf32": ("<f4", 8),
}


def load_iq(path: str, fmt: str | None = None, rate: float | None = None, *, start_s: float = 0.0,
            duration_s: float | None = None, swap_iq: bool = False, skip_bytes: int = 0):
    """Load an IQ file -> (complex64 ndarray, info dict).

    fmt: cu8|cs8|cs16|cf32|wav (guessed from the extension when None).
    rate: needed for raw formats (wav carries its own rate).
    start_s/duration_s: read only a window (the file is not fully read).
    swap_iq: exchange I and Q (use for recordings that turn out spectrally mirrored).
    skip_bytes: header bytes to skip for raw formats (e.g. a stray 44-byte header).
    Samples are scaled to roughly +-1.0 full scale; cu8/cs8/cs16 DC offsets are NOT removed.
    """
    fmt = (fmt or os.path.splitext(path)[1].lstrip(".")).lower()
    fmt = {"u8": "cu8", "iq": "cu8", "raw": "cf32", "cfile": "cf32", "fc": "cf32", "wave": "wav"}.get(fmt, fmt)
    info = {"path": path, "format": fmt}
    if fmt == "wav":
        h = parse_wav_header(path)
        info.update(h)
        sr = h["sample_rate"] if rate is None else rate
        start = int(round(start_s * sr))
        cnt = None if duration_s is None else int(round(duration_s * sr))
        a = _wav_frames(path, h, start, cnt)
        # rate= overrides the header: some recorders write a bogus WAV rate (e.g. 44100 for a 1.05 MS/s file)
        info["sample_rate_hz"] = float(sr)
        info["wav_header_rate_hz"] = float(h["sample_rate"])
    elif fmt in RAW_SPECS:
        if rate is None:
            raise ValueError("raw formats need rate=")
        dt, bps = RAW_SPECS[fmt]
        sr = float(rate)
        start = int(round(start_s * sr))
        cnt = None if duration_s is None else int(round(duration_s * sr))
        fsize = os.path.getsize(path) - skip_bytes
        total = fsize // bps
        if fsize % bps:
            info["trailing_bytes"] = fsize % bps
        start = min(start, total)
        n = total - start if cnt is None else min(cnt, total - start)
        a = np.fromfile(path, dtype=dt, count=n * 2, offset=skip_bytes + start * bps)
        if fmt == "cu8":
            a = (a.astype(np.float32) - 127.5) / 127.5
        elif fmt == "cs8":
            a = a.astype(np.float32) / 128.0
        elif fmt == "cs16":
            a = a.astype(np.float32) / 32768.0
        else:
            a = a.astype(np.float32)
        info["sample_rate_hz"] = sr
    else:
        raise ValueError(f"unknown format {fmt!r}")
    i, q = (a[1::2], a[0::2]) if swap_iq else (a[0::2], a[1::2])
    x = (i + 1j * q).astype(np.complex64)
    info["n_samples"] = int(x.size)
    info["duration_s"] = x.size / info["sample_rate_hz"]
    info["swap_iq"] = bool(swap_iq)
    return x, info


def load_manifest(path: str = MANIFEST) -> list:
    with open(path) as f:
        m = json.load(f)
    return m["samples"] if isinstance(m, dict) else m


def find_entry(sample_id: str, manifest: str = MANIFEST) -> dict:
    for e in load_manifest(manifest):
        if e["id"] == sample_id:
            return e
    raise KeyError(f"no manifest entry with id {sample_id!r}")


def entry_path(e: dict) -> str:
    p = e["path"]
    return p if os.path.isabs(p) else os.path.join(SAMPLES_ROOT, p)


def load_entry(sample_id_or_entry, *, start_s: float = 0.0, duration_s: float | None = None,
               swap_iq: bool | None = None, manifest: str = MANIFEST):
    """Load a manifest entry (id string or the dict) as complex64.

    The returned meta dict is the manifest entry updated with the actual load info
    (so meta['sample_rate_hz'], meta['center_freq_hz'] are always present).
    swap_iq=None -> use the entry's optional "iq_swap" flag (default False).
    """
    e = find_entry(sample_id_or_entry, manifest) if isinstance(sample_id_or_entry, str) else sample_id_or_entry
    sw = bool(e.get("iq_swap", False)) if swap_iq is None else swap_iq
    fmt = "wav" if e.get("container") == "wav" else e.get("sample_format")
    x, info = load_iq(entry_path(e), fmt=fmt,
                      rate=e.get("sample_rate_hz"), start_s=start_s, duration_s=duration_s, swap_iq=sw,
                      skip_bytes=int(e.get("skip_bytes", 0)))
    meta = dict(e)
    meta.update({k: v for k, v in info.items() if k not in ("path",)})
    meta["center_freq_hz"] = e.get("center_freq_hz")
    return x, meta


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------


def welch_psd(x: np.ndarray, fs: float, nfft: int = 4096, max_segments: int = 4000, overlap: float = 0.5,
              window: str = "hann", return_db: bool = True):
    """Averaged power spectrum of complex samples.

    Returns (freqs_hz [-fs/2..fs/2), psd) where psd is dB relative to full scale
    (dBFS per bin, +-1.0 full scale complex => 0 dB for a full-scale tone) if return_db.
    Mean of |FFT|^2 over up to max_segments segments, DC bin not removed.
    """
    x = np.asarray(x, dtype=np.complex64)
    nfft = int(min(nfft, max(256, 2 ** int(np.floor(np.log2(max(len(x), 256)))))))
    step = max(1, int(nfft * (1 - overlap)))
    nseg = max(1, (len(x) - nfft) // step + 1)
    if nseg > max_segments:
        step = max(step, (len(x) - nfft) // max_segments)
        nseg = (len(x) - nfft) // step + 1
    w = np.hanning(nfft).astype(np.float32) if window == "hann" else np.ones(nfft, np.float32)
    acc = np.zeros(nfft, np.float64)
    B = 256
    for s in range(0, nseg, B):
        idx = (np.arange(s, min(nseg, s + B))[:, None] * step) + np.arange(nfft)[None, :]
        seg = x[idx] * w
        acc += (np.abs(np.fft.fft(seg, axis=1)) ** 2).sum(axis=0)
    p = acc / nseg / (w.astype(np.float64) ** 2).sum() / nfft  # => full-scale tone = 1/nfft... normalised below
    p = np.fft.fftshift(p)
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / fs))
    if return_db:
        p = 10 * np.log10(p + 1e-20)
    return f, p


def waterfall(x: np.ndarray, fs: float, nfft: int = 1024, rows: int = 400):
    """Return (freqs, times, db[rows, nfft]) waterfall using non-overlapping FFT blocks, decimated in time."""
    n = len(x) // nfft
    rows = min(rows, n)
    blk = n // rows
    w = np.hanning(nfft).astype(np.float32)
    out = np.zeros((rows, nfft), np.float32)
    for r in range(rows):
        seg = x[r * blk * nfft:r * blk * nfft + blk * nfft].reshape(blk, nfft) * w
        out[r] = (np.abs(np.fft.fft(seg, axis=1)) ** 2).mean(axis=0)
    out = np.fft.fftshift(out, axes=1)
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / fs))
    t = np.arange(rows) * blk * nfft / fs
    return f, t, 10 * np.log10(out / (w ** 2).sum() + 1e-20)


def health_stats(x: np.ndarray, fmt: str | None = None) -> dict:
    """Quick sanity numbers: DC offset, power, clipping, I/Q balance, all-zero detection."""
    i, q = x.real, x.imag
    pw = float(np.mean(np.abs(x) ** 2))
    st = {
        "n": int(x.size),
        "mean_i": float(i.mean()), "mean_q": float(q.mean()),
        "std_i": float(i.std()), "std_q": float(q.std()),
        "rms_dbfs": float(10 * np.log10(pw + 1e-20)),
        "peak_abs": float(np.max(np.abs(x))) if x.size else 0.0,
        "frac_zero": float(np.mean(x == 0)),
        "iq_corr": float(np.mean(i * q) / (i.std() * q.std() + 1e-20)),
    }
    # clipping: fraction of samples at the extreme codes (+-0.99 of full scale on either rail)
    st["frac_clip_i"] = float(np.mean(np.abs(i) > 0.985))
    st["frac_clip_q"] = float(np.mean(np.abs(q) > 0.985))
    st["iq_std_ratio"] = st["std_i"] / (st["std_q"] + 1e-20)
    return st


def occupied_peaks(f: np.ndarray, p_db: np.ndarray, snr_db: float = 8.0, smooth_bins: int = 7, max_peaks: int = 12):
    """Crude list of spectral humps well above the noise floor: [(offset_hz, width_hz, peak_snr_db)]."""
    k = np.ones(smooth_bins) / smooth_bins
    pad = smooth_bins // 2
    s = np.convolve(np.pad(p_db, pad, mode="edge"), k, mode="valid")  # edge-pad: no false humps at +-fs/2
    floor = np.percentile(s, 20)
    mask = s > floor + snr_db
    peaks = []
    i = 0
    n = len(s)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            seg = s[i:j]
            pk = int(np.argmax(seg)) + i
            df = f[1] - f[0]
            peaks.append((float(f[pk]), float((j - i) * df), float(s[pk] - floor)))
            i = j
        else:
            i += 1
    peaks.sort(key=lambda t: -t[2])
    return peaks[:max_peaks], float(floor)


def plot_spectrum(x, fs, out_png, title="", center_hz=None, nfft=4096, with_waterfall=False, annotate=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    f, p = welch_psd(x, fs, nfft=nfft)
    rows = 2 if with_waterfall else 1
    fig, ax = plt.subplots(rows, 1, figsize=(12, 4.2 * rows), squeeze=False)
    ax0 = ax[0, 0]
    sc = 1e3 if fs < 5e6 else 1e6
    unit = "kHz" if sc == 1e3 else "MHz"
    xs = f / sc
    ax0.plot(xs, p, lw=0.7)
    ax0.set_xlim(xs[0], xs[-1])
    ax0.set_xlabel(f"offset from center [{unit}]" + (f"   (center {center_hz/1e6:.4f} MHz)" if center_hz else ""))
    ax0.set_ylabel("dBFS / bin (Welch)")
    ax0.grid(alpha=0.3)
    ax0.set_title(title or os.path.basename(str(out_png)))
    if annotate:
        for off, label in annotate:
            ax0.axvline(off / sc, color="r", ls=":", lw=0.8)
            ax0.text(off / sc, ax0.get_ylim()[1], label, rotation=90, va="top", ha="right", fontsize=7, color="r")
    if with_waterfall:
        ff, tt, db = waterfall(x, fs)
        lo, hi = np.percentile(db, 2), np.percentile(db, 99.8)
        ax[1, 0].imshow(db, aspect="auto", origin="upper", cmap="viridis", vmin=lo, vmax=hi,
                        extent=[ff[0] / sc, ff[-1] / sc, tt[-1], 0])
        ax[1, 0].set_xlabel(f"offset [{unit}]")
        ax[1, 0].set_ylabel("time [s]")
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--id", help="manifest entry id")
    src.add_argument("--file", help="bare IQ file")
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--format", help="cu8|cs8|cs16|cf32|wav (bare files)")
    ap.add_argument("--rate", type=float, help="sample rate Hz (raw formats)")
    ap.add_argument("--center", type=float, help="center frequency Hz (labels only)")
    ap.add_argument("--start", type=float, default=0.0, help="start offset in seconds")
    ap.add_argument("--duration", type=float, help="seconds to load (default: all)")
    ap.add_argument("--swap-iq", action="store_true", help="swap I and Q before analysis")
    ap.add_argument("--nfft", type=int, default=4096)
    ap.add_argument("--plot", metavar="PNG", help="write spectrum PNG")
    ap.add_argument("--waterfall", action="store_true", help="add a waterfall panel to the PNG")
    ap.add_argument("--stats-only", action="store_true")
    a = ap.parse_args(argv)

    if a.id:
        x, meta = load_entry(a.id, start_s=a.start, duration_s=a.duration, swap_iq=True if a.swap_iq else None,
                             manifest=a.manifest)
        title = a.id
    else:
        x, meta = load_iq(a.file, a.format, a.rate, start_s=a.start, duration_s=a.duration, swap_iq=a.swap_iq)
        meta["center_freq_hz"] = a.center
        title = os.path.basename(a.file)
    fs = meta["sample_rate_hz"]
    st = health_stats(x, meta.get("format"))
    print(f"{title}: {meta['n_samples']} samples @ {fs:.0f} Hz = {meta['duration_s']:.3f} s"
          f" center={meta.get('center_freq_hz')}")
    for k, v in st.items():
        print(f"  {k:14s} {v:.6g}" if isinstance(v, float) else f"  {k:14s} {v}")
    if not a.stats_only:
        f, p = welch_psd(x, fs, nfft=a.nfft)
        peaks, floor = occupied_peaks(f, p)
        print(f"  noise floor (20th pct, smoothed) {floor:.1f} dBFS/bin; strongest humps (offset_Hz, width_Hz, SNR_dB):")
        for off, bw, snr in peaks:
            print(f"    {off:+12.0f} {bw:10.0f} {snr:6.1f}")
        if a.plot:
            plot_spectrum(x, fs, a.plot, title=title, center_hz=meta.get("center_freq_hz"), nfft=a.nfft,
                          with_waterfall=a.waterfall)
            print("  wrote", a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())
