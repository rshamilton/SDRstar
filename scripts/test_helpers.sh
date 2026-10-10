#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# test_helpers.sh [helpers-dir] -- prove the helper decoders actually work.
#
#   (a) every helper starts and prints its usage text with ALL dyld environment variables unset, from a copy of the
#       app moved to a fresh directory (so nothing can be resolved through the build tree or the private prefix);
#   (b) functional test with DSD-neo's own recorded capture p25p1_c4fm_cc.iq (48 kHz unsigned-8-bit I/Q, 2 s): it is
#       converted to FM-discriminator PCM exactly like the SDRstar module does and fed to the helper on stdin:
#           dsd-neo --frontend none -f1 -i - -s 48000 -o null < pcm
#       whose output must contain "NAC/CC: 140" and "Identifier Update";
#   (c) multimon-ng decodes a synthetic DTMF tone sequence.
#
# With no argument the helpers inside the freshly built app (dist/<arch>/SDRstar.app) are used (relocated copy).
# Pass a directory containing dsd-neo, multimon-ng and rtl_433 to test those directly (used by the Linux CI job).
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sdrstar-helpers.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

if [ $# -ge 1 ]; then
    H="$1"
else
    [ -d "$APP_DIR" ] || die "no app at $APP_DIR; run scripts/bundle_mac.sh first"
    # Relocation test: run the helpers from a copy of the app in a brand-new directory.
    log "copying the app to a fresh location to prove it is relocatable"
    mkdir -p "$TMP/relocated"
    ditto "$APP_DIR" "$TMP/relocated/SDRstar.app"
    H="$TMP/relocated/SDRstar.app/Contents/Resources/helpers"
fi
FIX="${SDRSTAR_FIXTURES:-$FIXTURES_DIR}"
[ -f "$FIX/p25p1_c4fm_cc.iq" ] || die "fixture $FIX/p25p1_c4fm_cc.iq not found (run scripts/build_helpers.sh, or set SDRSTAR_FIXTURES)"

failures=0
fail() {
    printf '  FAIL: %s\n' "$*" >&2
    failures=$((failures + 1))
}

# A helper must not be run with any dyld override that could mask a broken install name.
clean_env() {
    # (On Linux -- the CI dry run -- the private prefix is found through LD_LIBRARY_PATH, so keep that one.)
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH -u DYLD_FALLBACK_FRAMEWORK_PATH \
        -u DYLD_INSERT_LIBRARIES -u DYLD_VERSIONED_LIBRARY_PATH -u DYLD_IMAGE_SUFFIX "$@"
}

# ---------------------------------------------------------------------------------------------------------------
log "(a) helpers start (-h) with DYLD_* unset"
for name in dsd-neo multimon-ng rtl_433; do
    exe="$H/$name"
    [ -x "$exe" ] || {
        fail "$name is missing or not executable: $exe"
        continue
    }
    rc=0
    out="$(run_with_timeout 30 clean_env "$exe" -h 2>&1 </dev/null)" || rc=$?
    if printf '%s' "$out" | grep -qiE 'dyld|Library not loaded|image not found|Symbol not found'; then
        fail "$name: dynamic loader error: $(printf '%s' "$out" | head -n 3)"
    elif [ "$rc" -gt 2 ]; then
        fail "$name -h exited with status $rc: $(printf '%s' "$out" | head -n 3)"
    elif ! printf '%s' "$out" | grep -qiE 'usage|options|help|rtl_433|multimon|dsd'; then
        fail "$name -h printed nothing recognisable: $(printf '%s' "$out" | head -n 3)"
    else
        note "$name: OK (exit $rc, $(printf '%s' "$out" | wc -l | tr -d ' ') lines of help)"
    fi
done

# ---------------------------------------------------------------------------------------------------------------
log "(b) DSD-neo decodes the P25 control-channel capture"
python3 "$PACKAGING/fm_pcm.py" "$FIX/p25p1_c4fm_cc.iq" "$TMP/p25.pcm"
rc=0
run_with_timeout 90 clean_env "$H/dsd-neo" --frontend none -f1 -i - -s 48000 -o null <"$TMP/p25.pcm" 2>&1 | strip_ansi >"$TMP/dsd.log" || rc=$?
if grep -q 'NAC/CC: 140' "$TMP/dsd.log" && grep -q 'Identifier Update' "$TMP/dsd.log"; then
    note "dsd-neo: OK ($(grep -c 'NAC/CC: 140' "$TMP/dsd.log") sync lines with NAC/CC: 140, Identifier Update present)"
else
    fail "dsd-neo output lacks 'NAC/CC: 140' and/or 'Identifier Update'. Output follows:"
    head -n 60 "$TMP/dsd.log" >&2
fi

# ---------------------------------------------------------------------------------------------------------------
log "(c) multimon-ng decodes DTMF"
python3 - "$TMP/dtmf.raw" <<'PY'
import array, math, sys
rate = 22050
rows = {"1": 697, "2": 697, "3": 697, "4": 770, "5": 770, "6": 770}
cols = {"1": 1209, "2": 1336, "3": 1477, "4": 1209, "5": 1336, "6": 1477}
out = array.array("h")
for digit in "123456":
    for n in range(int(rate * 0.12)):
        t = n / rate
        out.append(int(9000 * (math.sin(2 * math.pi * rows[digit] * t) + math.sin(2 * math.pi * cols[digit] * t))))
    out.extend([0] * int(rate * 0.08))
with open(sys.argv[1], "wb") as fh:
    out.tofile(fh)
PY
rc=0
run_with_timeout 30 clean_env "$H/multimon-ng" -q -t raw -a DTMF "$TMP/dtmf.raw" >"$TMP/mm.log" 2>&1 || rc=$?
decoded="$(grep -o 'DTMF: .' "$TMP/mm.log" | sed 's/DTMF: //' | tr -d '\n')"
if [ "$decoded" = "123456" ]; then
    note "multimon-ng: OK (decoded DTMF digits $decoded)"
else
    fail "multimon-ng decoded '$decoded' instead of 123456 (exit $rc):"
    head -n 20 "$TMP/mm.log" >&2
fi

if [ "$failures" -ne 0 ]; then
    die "$failures helper check(s) failed"
fi
log "all helper checks passed"
