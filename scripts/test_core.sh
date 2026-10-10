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
args=(-G "$CMAKE_GENERATOR_NAME" -DCMAKE_BUILD_TYPE=Release)
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
(cd "$CORE_BUILD" && ctest --output-on-failure --timeout 600 2>&1 | tee "$CORE_BUILD/ctest.log")

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
