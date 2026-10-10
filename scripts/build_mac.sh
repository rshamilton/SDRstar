#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# build_mac.sh -- build SDRstar.app and the .dmg for THIS Mac, end to end.
#
#   scripts/build_mac.sh [--skip-tests] [--no-dmg] [--smoke] [--clean]
#
# * Works from any directory; everything stays inside the repository (.deps/, build/, dist/).
# * Idempotent: finished steps are skipped on a re-run; only what changed is rebuilt.
# * Needs no sudo and never installs anything outside the repository.  If a required tool is missing it stops and
#   tells you exactly what to install (scripts/check_tools.sh) -- it will not install it for you.
# * Builds for the CPU of the Mac it runs on (Apple Silicon -> SDRstar-AppleSilicon.dmg, Intel -> SDRstar-Intel.dmg).
#
#   --skip-tests  skip the helper functional tests and the core unit/integration tests (faster)
#   --no-dmg      stop after SDRstar.app
#   --smoke       also launch the app for ~15 s at the end (opens a window briefly)
#   --clean       delete .deps/<arch>, build/<arch> and dist/ first (full rebuild, ~30-60 minutes)
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

run_tests=1 make_dmg=1 smoke=0 clean=0
for arg in "$@"; do
    case "$arg" in
        --skip-tests) run_tests=0 ;;
        --no-dmg) make_dmg=0 ;;
        --smoke) smoke=1 ;;
        --clean) clean=1 ;;
        -h | --help)
            sed -n '4,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) die "unknown option: $arg (try --help)" ;;
    esac
done

[ "$(id -u)" -ne 0 ] || die "do not run this as root / with sudo: it needs no special privileges"

"$SCRIPTS/check_tools.sh"

if [ "$clean" = 1 ]; then
    log "--clean: removing $DEPS_PREFIX, $DEPS_WORK, $BUILD_DIR and $DIST_DIR"
    rm -rf "$DEPS_PREFIX" "$DEPS_WORK" "$BUILD_DIR" "$DIST_DIR"
fi

# Use ccache automatically when the user already has it.
if have ccache; then export SDRSTAR_USE_CCACHE=1; fi

t0=$SECONDS
step() { # name command...
    local name=$1
    shift
    log "STEP: $name"
    "$@"
}

step "native libraries (from source, macOS >= $MACOS_DEPLOYMENT_TARGET)" "$SCRIPTS/build_deps.sh"
step "helper decoders (dsd-neo, multimon-ng, rtl_433)" "$SCRIPTS/build_helpers.sh"
step "SDR++ + SDRstar modules" "$SCRIPTS/build_sdrpp.sh"
step "assemble and sign SDRstar.app" "$SCRIPTS/bundle_mac.sh"
step "verify the bundle" "$SCRIPTS/verify_bundle.sh"
if [ "$run_tests" = 1 ]; then
    step "helper functional tests" "$SCRIPTS/test_helpers.sh"
    step "core library tests (native build)" "$SCRIPTS/test_core.sh"
fi
if [ "$make_dmg" = 1 ]; then
    step "disk image" "$SCRIPTS/make_dmg.sh"
fi
# The launch test comes last on purpose: running the app can touch its bundle (extended attributes), and the disk
# image must be made from the pristine, freshly signed bundle.
if [ "$smoke" = 1 ]; then
    step "launch test" "$SCRIPTS/smoke_launch.sh"
fi

log "finished in $(((SECONDS - t0) / 60)) min $(((SECONDS - t0) % 60)) s"
note "app: $APP_DIR"
if [ "$make_dmg" = 1 ]; then note "dmg: $DIST_DIR/SDRstar-$DMG_ARCH.dmg"; fi
note "to try it: open \"$APP_DIR\"   (the first launch of a locally built, unsigned app may show the Gatekeeper prompt; see packaging/README-dmg.txt)"
