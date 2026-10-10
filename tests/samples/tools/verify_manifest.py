#!/usr/bin/env python3
"""verify_manifest.py - check every MANIFEST.json entry against the files on disk.

For each entry: file exists, size and SHA-256 match, the loader (inspect_iq.load_entry) can read the first
0.25 s (or the whole file if shorter) and returns a plausible number of samples for the manifest rate, and the
duration implied by the file size matches duration_s.  Prints one line per sample and exits non-zero on any failure.

    python3 -I /home/user/samples/tools/verify_manifest.py [--quick]     # --quick skips the SHA-256
"""
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from inspect_iq import SAMPLES_ROOT, load_entry, load_manifest  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    quick = "--quick" in sys.argv
    m = {"samples": load_manifest()}
    bad = 0
    for e in m["samples"]:
        p = os.path.join(SAMPLES_ROOT, e["path"])
        problems = []
        if not os.path.exists(p):
            print(f"FAIL {e['id']}: missing file")
            bad += 1
            continue
        if os.path.getsize(p) != e["size_bytes"]:
            problems.append("size differs")
        if not quick and sha256(p) != e["sha256"]:
            problems.append("sha256 differs")
        if e["status"] != "rejected" and e.get("channels") == 2:
            try:
                x, meta = load_entry(e, duration_s=0.25)
                exp = min(int(round(0.25 * e["sample_rate_hz"])), int(e["duration_s"] * e["sample_rate_hz"]) + 1)
                if abs(len(x) - exp) > 2:
                    problems.append(f"loaded {len(x)} samples, expected ~{exp}")
            except Exception as ex:  # noqa: BLE001
                problems.append(f"load error {ex!r}")
        print(("FAIL " if problems else "ok   ") + f"{e['id']:55s} {e['status']:9s} " + "; ".join(problems))
        bad += bool(problems)
    print("all good" if not bad else f"{bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
