#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fetch_test_samples.sh [dest-dir] -- download the optional public test recordings that are not stored in the
# repository (licence not stated, so not redistributed here).  Each file is pinned to an exact upstream commit and
# its SHA-256 is verified; a mismatch deletes the file.  Default destination: core/tests/data/signals.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$ROOT/core/tests/data/signals}"
mkdir -p "$DEST"

fetch() { # <name> <url> <sha256>
    local name="$1" url="$2" want="$3" got
    if [ -f "$DEST/$name" ]; then
        got="$(shasum -a 256 "$DEST/$name" | cut -d' ' -f1)"
        if [ "$got" = "$want" ]; then echo "ok (already present): $name"; return 0; fi
    fi
    echo "downloading $name"
    curl -fsSL --retry 3 -o "$DEST/$name.part" "$url"
    got="$(shasum -a 256 "$DEST/$name.part" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        rm -f "$DEST/$name.part"
        echo "SHA-256 mismatch for $name (got $got, expected $want): refusing to use it" >&2
        return 1
    fi
    mv "$DEST/$name.part" "$DEST/$name"
    echo "ok: $name"
}

# Nexus-TH sensor, 433.92 MHz, 250 kS/s cu8 (rtl_433_tests, tests/nexus/03).  Expected decode: Nexus-TH id 201,
# channel 3, 29.4 C, 30 % humidity.
fetch nexus_g027_433.92M_250k.cu8 \
    "https://raw.githubusercontent.com/merbanan/rtl_433_tests/1aa8c29d6b9383739d1aeed495dbc5069b704392/tests/nexus/03/g027_433.92M_250k.cu8" \
    "c537b7416cc710dfd0d666cd8ff406ef24b5c57ea36a3d3a62cff4e12119002a"
