#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# test_core.sh [dsd-neo-executable] [fixtures-dir] -- build the pure-C++ core library natively with its tests and
# run every test, including the integration tests that spawn the REAL dsd-neo, open loopback sockets and use
# socketpairs.  On macOS this validates that our supervisor/bridge code is POSIX-portable.
#
# Defaults: the dsd-neo inside dist/<arch>/SDRstar.app and the fixtures fetched by build_helpers.sh.
# Fails if the integration tests were skipped (they skip silently when their environment is missing).
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

DSD="${1:-$APP_DIR/Contents/Resources/helpers/dsd-neo}"
FIX="${2:-${SDRSTAR_FIXTURES:-$FIXTURES_DIR}}"
[ -x "$DSD" ] || die "dsd-neo not found/executable: $DSD"
[ -f "$FIX/p25p1_c4fm_cc.iq" ] || die "fixtures not found in $FIX"

pick_generator
CORE_BUILD="$BUILD_DIR/core"
# -O2 -g: optimised like the shipped code but with symbols, so a crash backtrace names functions and lines.
args=(-G "$CMAKE_GENERATOR_NAME" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g -DNDEBUG")
if [ "$IS_DARWIN" = 1 ]; then
    args+=("-DCMAKE_OSX_ARCHITECTURES=$ARCH" "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET")
fi
if [ -n "${SDRSTAR_USE_CCACHE:-}" ] && have ccache; then
    args+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$(command -v ccache)")
fi

group_start "build core + tests ($CORE_BUILD)"
cmake -S "$ROOT/core" -B "$CORE_BUILD" "${args[@]}"
cmake --build "$CORE_BUILD" --parallel "$JOBS"
group_end

log "running the tests (SDRSTAR_DSD_NEO=$DSD)"
export SDRSTAR_DSD_NEO="$DSD" SDRSTAR_FIXTURES="$FIX"
# Tests must not see dyld overrides either: dsd-neo has to resolve its libraries through its own rpath.
unset DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES
ctest_rc=0
(cd "$CORE_BUILD" && ctest --output-on-failure --timeout 600 2>&1 | tee "$CORE_BUILD/ctest.log") || ctest_rc=$?
if [ "$ctest_rc" -ne 0 ]; then
    # Help whoever has to fix it: for every test that crashed or failed, re-run it under a debugger (up to 5 times,
    # in case the crash is timing dependent) and print the backtrace of all threads, plus macOS's own crash report.
    log "ctest failed (status $ctest_rc); collecting diagnostics for the failing tests"
    failed="$(grep -E 'Test +#[0-9]+: .*\*\*\*' "$CORE_BUILD/ctest.log" | sed -E 's/.*Test +#[0-9]+: ([A-Za-z0-9_]+).*/\1/' || true)"
    for t in $failed; do
        group_start "diagnostics for $t"
        exe="$CORE_BUILD/$t"
        for attempt in 1 2 3 4 5; do
            if have lldb; then
                run_with_timeout 300 lldb --batch -o run -o 'bt all' -o 'register read pc' -o quit -- "$exe" >"$CORE_BUILD/$t.debug.log" 2>&1 || true
            elif have gdb; then
                run_with_timeout 300 gdb -batch -ex run -ex 'thread apply all bt' --args "$exe" >"$CORE_BUILD/$t.debug.log" 2>&1 || true
            else
                echo "(no debugger available)" >"$CORE_BUILD/$t.debug.log"
                break
            fi
            if grep -qE 'stop reason|SIGSEGV|SIGABRT|received signal' "$CORE_BUILD/$t.debug.log"; then
                note "crash reproduced on attempt $attempt"
                break
            fi
        done
        tail -n 150 "$CORE_BUILD/$t.debug.log"
        if [ "$IS_DARWIN" = 1 ]; then
            report="$(find "$HOME/Library/Logs/DiagnosticReports" -name "$t*.ips" 2>/dev/null | sort | tail -n 1 || true)"
            if [ -n "$report" ]; then
                note "macOS crash report $report"
                head -c 12000 "$report"
                echo
            fi
        fi
        group_end
    done
    die "core tests failed"
fi

# The integration suite prints "(skipped ...)" when its env is missing; make that a failure, not a silent pass.
log "re-running the DSD-neo integration suite verbosely to confirm nothing was skipped"
dsd_test="$CORE_BUILD/test_dsd_session"
[ -x "$dsd_test" ] || die "integration test binary $dsd_test was not built"
"$dsd_test" | tee "$CORE_BUILD/test_dsd_session.log"
if grep -qi 'skipped' "$CORE_BUILD/test_dsd_session.log"; then
    die "the DSD-neo integration tests were skipped"
fi
for t in real_dsd_p25_control_channel_is_identified real_dsd_dmr_color_code_and_voice_audio; do
    grep -q "\[ OK \] dsd_session::$t" "$CORE_BUILD/test_dsd_session.log" || die "integration test $t did not pass"
done
log "core tests passed (integration tests really ran)"
