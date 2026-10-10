#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# ci_tsan.sh <dsd-neo-executable> <fixtures-dir> -- run the core tests under ThreadSanitizer (Linux).
#
# Why: a use-after-free on teardown (a late sample block reaching a destroyed object) passed every Linux run and
# crashed on macOS.  ThreadSanitizer reports that kind of bug deterministically, so the core is kept clean of it.
#
# Only sanitizer REPORTS fail this script.  The tests are real-time (they play recorded radio signals against
# deadlines) and sanitizer slow-down can make one time out on a small CI machine, which says nothing about races, so
# an ordinary test failure is logged and ignored here (the normal test run already covers it).
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

[ "$IS_DARWIN" = 0 ] || die "ci_tsan.sh is Linux-only"
DSD="${1:?usage: ci_tsan.sh <dsd-neo> <fixtures-dir>}"
FIX="${2:?usage: ci_tsan.sh <dsd-neo> <fixtures-dir>}"
[ -x "$DSD" ] || die "dsd-neo not found/executable: $DSD"

pick_generator
OUT="$BUILD_DIR/core-tsan"
group_start "build core + tests with ThreadSanitizer ($OUT)"
cmake -S "$ROOT/core" -B "$OUT" -G "$CMAKE_GENERATOR_NAME" -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -O1 -g" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread"
cmake --build "$OUT" --parallel "$JOBS"
group_end

export SDRSTAR_DSD_NEO="$DSD" SDRSTAR_FIXTURES="$FIX"
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1"
bad=0
for t in test_fuzz test_adsb test_subprocess test_rigctl_bridge test_dsd_session test_probe_manager test_trunk_follower test_helper_decoder test_signal_detector test_resampler; do
    [ -x "$OUT/$t" ] || { warn "$t was not built"; continue; }
    log "TSAN: $t"
    rc=0
    run_with_timeout 900 "$OUT/$t" >"$OUT/$t.log" 2>&1 || rc=$?
    reports="$(grep -c 'WARNING: ThreadSanitizer' "$OUT/$t.log" || true)"
    if [ "$reports" != "0" ]; then
        bad=$((bad + 1))
        echo "::error::ThreadSanitizer found $reports problem(s) in $t"
        grep -m1 -A40 'WARNING: ThreadSanitizer' "$OUT/$t.log" | cut -c1-200
    elif [ "$rc" -ne 0 ]; then
        note "$t exited with status $rc but no sanitizer report (likely a timing failure under the slow-down); ignored here"
    else
        note "$t: clean"
    fi
done
[ "$bad" = 0 ] || die "ThreadSanitizer reported problems in $bad test program(s)"
log "ThreadSanitizer: no reports"
