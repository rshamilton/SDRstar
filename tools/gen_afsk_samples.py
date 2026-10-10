#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate SYNTHETIC test audio (22.05 kHz, signed 16-bit little-endian, mono) for the multimon-ng based decoders:

  aprs.raw  an AX.25 UI frame (APRS position report) as Bell-202 AFSK, 1200 baud (1200 Hz mark / 2200 Hz space)
  eas.raw   an EAS / SAME alert header as AFSK 520.83 baud (2083.33 Hz mark / 1562.5 Hz space), sent 3 times + EOM

These are *synthetic* (built from the published encodings), so they prove the decoder wiring and arguments, not RF
realism.  Usage: python3 -I tools/gen_afsk_samples.py <output-dir>
"""
import math
import os
import struct
import sys

FS = 22050


class Fsk:
    def __init__(self):
        self.phase = 0.0
        self.samples = []

    def tone(self, freq, nsamples, amp=0.5):
        inc = 2 * math.pi * freq / FS
        for _ in range(nsamples):
            self.samples.append(int(amp * 32767 * math.sin(self.phase)))
            self.phase += inc
        self.phase %= 2 * math.pi

    def bits(self, bits, baud, f_mark, f_space):
        """phase-continuous FSK with fractional samples per bit"""
        spb = FS / baud
        acc = 0.0
        for b in bits:
            acc += spb
            n = int(round(acc))
            acc -= n
            self.tone(f_mark if b else f_space, n)

    def silence(self, seconds):
        self.samples.extend([0] * int(FS * seconds))

    def write(self, path):
        with open(path, "wb") as f:
            f.write(struct.pack("<%dh" % len(self.samples), *self.samples))


def crc16_x25(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def ax25_addr(call, ssid, last):
    call = call.ljust(6)
    out = bytearray((ord(c) << 1) for c in call)
    out.append(0x60 | (ssid << 1) | (1 if last else 0))
    return bytes(out)


def lsb_bits(data):
    for byte in data:
        for i in range(8):
            yield (byte >> i) & 1


def aprs_audio():
    info = b"!4903.50N/07201.75W-SDRstar synthetic APRS test"
    frame = ax25_addr("APRS", 0, False) + ax25_addr("N0CALL", 9, False) + ax25_addr("WIDE1", 1, True) + bytes([0x03, 0xF0]) + info
    crc = crc16_x25(frame)
    frame += bytes([crc & 0xFF, crc >> 8])
    bits = []
    ones = 0
    for b in lsb_bits(frame):          # bit stuffing: a 0 after five 1s
        bits.append(b)
        ones = ones + 1 if b else 0
        if ones == 5:
            bits.append(0)
            ones = 0
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    raw = flag * 40 + bits + flag * 4
    # NRZI: a 0 toggles the tone, a 1 keeps it
    tone = 1
    nrzi = []
    for b in raw:
        if b == 0:
            tone ^= 1
        nrzi.append(tone)
    f = Fsk()
    f.silence(0.3)
    f.bits(nrzi, 1200, 1200.0, 2200.0)
    f.silence(0.3)
    return f


def eas_audio():
    header = b"ZCZC-WXR-RWT-012057-012081-012101-012103-012115+0030-2780415-WTSP/TB -"
    f = Fsk()
    f.silence(0.3)
    for _ in range(3):
        bits = list(lsb_bits(b"\xAB" * 16 + header))
        f.bits(bits, 520.83, 2083.33, 1562.5)
        f.silence(1.0)
    for _ in range(3):
        bits = list(lsb_bits(b"\xAB" * 16 + b"NNNN"))
        f.bits(bits, 520.83, 2083.33, 1562.5)
        f.silence(1.0)
    return f


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    aprs_audio().write(os.path.join(out, "aprs.raw"))
    eas_audio().write(os.path.join(out, "eas.raw"))
    print("wrote", os.path.join(out, "aprs.raw"), "and", os.path.join(out, "eas.raw"))
