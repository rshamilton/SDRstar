#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# ci_linux.sh -- the cheap (1x cost) Linux check that runs on every push.
#
# Builds mbelib-neo and DSD-neo at the pinned commits against the distribution's own libraries (apt packages:
# libsndfile1-dev libssl-dev libpulse-dev libncurses-dev libcodec2-dev), then builds the pure-C++ core library with
# its tests and runs ALL of them, including the integration tests that spawn the real dsd-neo on DSD-neo's recorded
# fixtures.  This is the same configuration that was verified by hand in the development sandbox.
#
# Everything is installed under .deps/linux-ci (repo-local).  Usage: scripts/ci_linux.sh
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

[ "$IS_DARWIN" = 0 ] || die "ci_linux.sh is the Linux-only check"
PREFIX="$DEPS_ROOT/linux-ci"
WORK="$DEPS_ROOT/work-linux-ci"
mkdir -p "$PREFIX" "$WORK" "$DEPS_SRC"
pick_generator

# --- mbelib-neo (pinned) ---------------------------------------------------------------------------------------
if [ ! -f "$PREFIX/.mbelib-$MBELIB_NEO_SHA" ]; then
    group_start "mbelib-neo $MBELIB_NEO_SHA"
    src="$(fetch_git mbelib-neo "$MBELIB_NEO_REPO" "$MBELIB_NEO_SHA")"
    rm -rf "$WORK/mbelib-neo"
    cmake -S "$src" -B "$WORK/mbelib-neo" -G "$CMAKE_GENERATOR_NAME" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF \
        -DMBELIB_WARNINGS_AS_ERRORS=OFF "-DCMAKE_INSTALL_PREFIX=$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
    cmake --build "$WORK/mbelib-neo" --parallel "$JOBS"
    cmake --install "$WORK/mbelib-neo"
    touch "$PREFIX/.mbelib-$MBELIB_NEO_SHA"
    group_end
else
    log "mbelib-neo: cached"
fi

# --- DSD-neo (pinned) ------------------------------------------------------------------------------------------
if [ ! -f "$PREFIX/.dsdneo-$DSDNEO_SHA-$MBELIB_NEO_SHA" ]; then
    group_start "DSD-neo $DSDNEO_SHA"
    src="$(fetch_git dsd-neo "$DSDNEO_REPO" "$DSDNEO_SHA")"
    rm -rf "$WORK/dsd-neo"
    cmake -S "$src" -B "$WORK/dsd-neo" -G "$CMAKE_GENERATOR_NAME" -DCMAKE_BUILD_TYPE=Release \
        -DDSD_WARNINGS_AS_ERRORS=OFF -DDSD_ENABLE_SOAPYSDR=OFF -DDSD_ENABLE_AIRSPY=OFF -DBUILD_TESTING=OFF \
        "-DCMAKE_PREFIX_PATH=$PREFIX" "-DCMAKE_INSTALL_PREFIX=$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
    cmake --build "$WORK/dsd-neo" --parallel "$JOBS"
    mkdir -p "$PREFIX/bin"
    install -m 0755 "$WORK/dsd-neo/apps/dsd-cli/dsd-neo" "$PREFIX/bin/dsd-neo"
    rm -rf "$PREFIX/fixtures"
    cp -R "$src/tests/fixtures/iq" "$PREFIX/fixtures"
    touch "$PREFIX/.dsdneo-$DSDNEO_SHA-$MBELIB_NEO_SHA"
    group_end
else
    log "DSD-neo: cached"
fi

# --- core + all tests ------------------------------------------------------------------------------------------
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$SCRIPTS/test_core.sh" "$PREFIX/bin/dsd-neo" "$PREFIX/fixtures"

# --- safety audit + ThreadSanitizer --------------------------------------------------------------------------
"$SCRIPTS/safety_audit.sh"
"$SCRIPTS/ci_tsan.sh" "$PREFIX/bin/dsd-neo" "$PREFIX/fixtures"
