#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# prepare_sdrpp.sh <output-dir>
#
# Produces a ready-to-build SDR++ source tree containing SDRstar:
#   1. fetch SDR++ at the pinned commit (versions.env)
#   2. apply every patches/sdrpp/*.patch in order
#   3. copy modules/sdrstar_smart into misc_modules/ and hook it into the SDR++ CMake build
#   4. apply SDRstar branding (name, icon) when assets exist
#
# Re-running is safe: an existing output dir is deleted first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../versions.env
source "$ROOT/versions.env"
OUT="${1:?usage: prepare_sdrpp.sh <output-dir>}"

echo "==> fetching SDR++ $SDRPP_SHA"
rm -rf "$OUT"
mkdir -p "$OUT"
git -C "$OUT" init -q
git -C "$OUT" remote add origin "$SDRPP_REPO"
if ! git -C "$OUT" fetch -q --depth 1 origin "$SDRPP_SHA" 2>/dev/null; then
    echo "    (fetch by SHA refused; falling back to a full fetch)"
    git -C "$OUT" fetch -q origin
fi
git -C "$OUT" checkout -q "$SDRPP_SHA"
[ "$(git -C "$OUT" rev-parse HEAD)" = "$SDRPP_SHA" ] || { echo "SHA mismatch after checkout" >&2; exit 1; }

echo "==> applying patches"
shopt -s nullglob
for p in "$ROOT"/patches/sdrpp/*.patch; do
    echo "    $(basename "$p")"
    git -C "$OUT" apply --whitespace=nowarn "$p"
done

echo "==> adding SDRstar modules"
if [ -f "$ROOT/modules/sdrstar_smart/CMakeLists.txt" ]; then
    rm -rf "$OUT/misc_modules/sdrstar_smart"
    cp -R "$ROOT/modules/sdrstar_smart" "$OUT/misc_modules/sdrstar_smart"
    # The module compiles the pure-C++ core library straight from the SDRstar repo.
    echo "set(SDRSTAR_ROOT \"$ROOT\" CACHE PATH \"SDRstar repository root\")" > "$OUT/sdrstar_paths.cmake"
    if ! grep -q "OPT_BUILD_SDRSTAR_SMART" "$OUT/CMakeLists.txt"; then
        python3 - "$OUT/CMakeLists.txt" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
marker = 'option(OPT_BUILD_SCHEDULER "Build the scheduler" OFF)'
assert marker in s, "SDR++ CMakeLists layout changed; update scripts/prepare_sdrpp.sh"
s = s.replace(marker, marker + '\noption(OPT_BUILD_SDRSTAR_SMART "Build the SDRstar smart-signal module" ON)')
marker2 = 'if (OPT_BUILD_SCHEDULER)'
assert marker2 in s
s = s.replace(marker2, 'if (OPT_BUILD_SDRSTAR_SMART)\ninclude(${CMAKE_SOURCE_DIR}/sdrstar_paths.cmake)\nadd_subdirectory("misc_modules/sdrstar_smart")\nendif (OPT_BUILD_SDRSTAR_SMART)\n\n' + marker2, 1)
open(p, 'w').write(s)
PY
    fi
else
    echo "    (modules/sdrstar_smart not present yet; building plain SDR++ + patches)"
fi

echo "==> done: $OUT"
