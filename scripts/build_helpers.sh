#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# build_helpers.sh -- build the three helper decoder programs that SDRstar runs as child processes:
#
#   dsd-neo       digital voice / control-channel decoder (P25, DMR, NXDN, D-STAR, YSF, dPMR, EDACS, ProVoice, M17)
#   multimon-ng   pagers (POCSAG/FLEX), AFSK/APRS, DTMF, EAS ...
#   rtl_433       433/868/915 MHz ISM sensors
#
# All three are compiled WITHOUT any SDR hardware backend (no RTL-SDR, SoapySDR or Airspy code at all), so they can
# never open or reconfigure the USB dongle: they only read samples/PCM from a pipe.  They are linked against the
# private dependency prefix produced by build_deps.sh, which must have run first.
#
# Results:  .deps/<arch>/helpers/{dsd-neo,multimon-ng,rtl_433}   and   .deps/<arch>/helpers/fixtures/iq/*.iq
#           (DSD-neo's recorded test captures, used by the verification scripts).
#
# Idempotent, no sudo, repo-local.  Usage: scripts/build_helpers.sh
set -euo pipefail
# shellcheck source=lib/buildenv.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/buildenv.sh"

# shellcheck disable=SC2034  # read by build_step in lib/common.sh
RECIPE_REV=1

# Helper components keep their own stamps (and live in their own CI cache), separate from the libraries.
STAMP_DIR="$HELPERS_DIR/.stamps"
setup_build_env
[ -f "$DEPS_PREFIX/lib/pkgconfig/libusb-1.0.pc" ] || die "dependencies are missing; run scripts/build_deps.sh first"
mkdir -p "$HELPERS_DIR"

# cmake_build_stage <name> <src> [cmake args...]: configure, build, and install into a throw-away staging prefix
# (so helper files never mix into the library prefix).  Prints nothing; the staged tree is $DEPS_WORK/stage/<name>.
cmake_build_stage() {
    local name=$1 src=$2
    shift 2
    local bdir="$DEPS_WORK/$name" stage="$DEPS_WORK/stage/$name"
    rm -rf "$bdir" "$stage"
    cmake -S "$src" -B "$bdir" "${CMAKE_COMMON_ARGS[@]}" "$@"
    cmake --build "$bdir" --parallel "$JOBS"
    cmake --install "$bdir" --prefix "$stage"
}

# ---------------------------------------------------------------------------------------------------------------
# DSD-neo
#   * no RTL-SDR / SoapySDR / Airspy backends  -> cannot touch USB.  (DSD_FORCE_RADIO_PIPELINE stays OFF: PCM on
#     stdin decodes without it -- verified with the P25 fixture -- and OFF leaves out the rtl_tcp client as well.)
#   * no terminal UI, no audio backend ("none") -> cannot open a sound device (so no microphone permission prompt);
#     decoded voice is emitted on stdout for SDRstar to play through SDR++'s audio sink
#   * libcurl and expat disabled -> no network code (they only serve RadioReference import / rdio uploads)
#   * Codec2 REQUIRED so M17 voice is never silently lost
# ---------------------------------------------------------------------------------------------------------------
build_dsdneo() {
    local src
    src="$(fetch_git dsd-neo "$DSDNEO_REPO" "$DSDNEO_SHA")"
    cmake_build_stage dsd-neo "$src" \
        -DDSD_WARNINGS_AS_ERRORS=OFF \
        -DDSD_ENABLE_RTLSDR=OFF -DDSD_ENABLE_SOAPYSDR=OFF -DDSD_ENABLE_AIRSPY=OFF \
        -DDSD_ENABLE_TERMINAL_UI=OFF -DDSD_ENABLE_QT_UI=OFF -DDSD_AUDIO_BACKEND=none \
        -DDSD_ENABLE_API=ON -DDSD_ENABLE_LTO=OFF -DDSD_ENABLE_NATIVE=OFF \
        -DDSD_REQUIRE_CODEC2=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON -DCMAKE_DISABLE_FIND_PACKAGE_EXPAT=ON \
        -DBUILD_TESTING=OFF "-DOPENSSL_ROOT_DIR=$DEPS_PREFIX"
    install -m 0755 "$DEPS_WORK/stage/dsd-neo/bin/dsd-neo" "$HELPERS_DIR/dsd-neo"
    # DSD-neo's recorded 48 kHz I/Q captures: test input for the verification scripts and the core integration tests.
    rm -rf "$FIXTURES_DIR"
    mkdir -p "$(dirname "$FIXTURES_DIR")"
    cp -R "$src/tests/fixtures/iq" "$FIXTURES_DIR"
}

# ---------------------------------------------------------------------------------------------------------------
# multimon-ng.  (On macOS it optionally compiles a CoreAudio "system audio tap" for macOS 14.2+; it is guarded by a
# run-time availability check and only used when asked for, so it is harmless on older systems.)
# ---------------------------------------------------------------------------------------------------------------
build_multimon() {
    local src
    src="$(fetch_git multimon-ng "$MULTIMON_REPO" "$MULTIMON_SHA")"
    cmake_build_stage multimon-ng "$src" \
        -DBUILD_GEN_NG=OFF -DX11_SUPPORT=OFF -DPULSE_AUDIO_SUPPORT=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_X11=ON -DCMAKE_DISABLE_FIND_PACKAGE_PulseAudio=ON -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON
    install -m 0755 "$DEPS_WORK/stage/multimon-ng/bin/multimon-ng" "$HELPERS_DIR/multimon-ng"
}

# ---------------------------------------------------------------------------------------------------------------
# rtl_433 without librtlsdr / SoapySDR / OpenSSL: reads I/Q from stdin ("-r cu8:-" style input) only.
# ---------------------------------------------------------------------------------------------------------------
build_rtl433() {
    local src
    src="$(fetch_git rtl_433 "$RTL433_REPO" "$RTL433_SHA")"
    cmake_build_stage rtl_433 "$src" \
        -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF \
        -DBUILD_TESTING=OFF -DBUILD_DOCUMENTATION=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON
    install -m 0755 "$DEPS_WORK/stage/rtl_433/bin/rtl_433" "$HELPERS_DIR/rtl_433"
}

build_step dsd-neo "$DSDNEO_SHA|$MBELIB_NEO_SHA|$CODEC2_SHA" build_dsdneo
build_step multimon-ng "$MULTIMON_SHA" build_multimon
build_step rtl_433 "$RTL433_SHA" build_rtl433

log "helper programs ready in $HELPERS_DIR"
for f in dsd-neo multimon-ng rtl_433; do
    note "$f  ($(du -h "$HELPERS_DIR/$f" | cut -f1))"
done
