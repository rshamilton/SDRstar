#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# build_sdrpp.sh -- prepare the patched SDR++ source tree (scripts/prepare_sdrpp.sh), configure it for a macOS .app
# and build it, linking only against the private dependency prefix (.deps/<arch>).
#
# Only the modules SDRstar wants are built; everything that needs a vendor SDK or an extra library we do not build
# (Airspy, HackRF, BladeRF, LimeSDR, SDRplay, PlutoSDR, Discord, M17 ...) is switched off to keep the dependency
# closure small.  The sdrstar_smart module is picked up automatically when modules/sdrstar_smart exists.
#
# Output:  build/<arch>/sdrpp-src (prepared sources)   build/<arch>/sdrpp-build (CMake build tree)
# Usage:   scripts/build_sdrpp.sh
set -euo pipefail
# shellcheck source=lib/buildenv.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/buildenv.sh"

setup_build_env
[ -f "$DEPS_PREFIX/lib/pkgconfig/volk.pc" ] || die "dependencies are missing; run scripts/build_deps.sh first"

MODULE_SRC="$ROOT/modules/sdrstar_smart"
HAVE_MODULE=no
if [ -f "$MODULE_SRC/CMakeLists.txt" ]; then HAVE_MODULE=yes; fi

# ---------------------------------------------------------------------------------------------------------------
# 1. Patched SDR++ tree.  Re-cloning SDR++ would give every file a new timestamp and force a full rebuild, so when
#    only the sdrstar_smart sources changed we just re-sync that module directory (rsync keeps unchanged files).
# ---------------------------------------------------------------------------------------------------------------
patch_hash="$(cat "$ROOT"/patches/sdrpp/*.patch "$SCRIPTS/prepare_sdrpp.sh" 2>/dev/null | shasum -a 256 | cut -c1-16)" || true
prep_key="$SDRPP_SHA|$patch_hash|module=$HAVE_MODULE"
if [ -f "$SDRPP_SRC/.sdrstar-prepared" ] && [ "$(cat "$SDRPP_SRC/.sdrstar-prepared")" = "$prep_key" ]; then
    log "SDR++ tree already prepared ($SDRPP_SHA)"
    if [ "$HAVE_MODULE" = yes ]; then
        rsync -a --delete "$MODULE_SRC/" "$SDRPP_SRC/misc_modules/sdrstar_smart/"
    fi
else
    group_start "prepare SDR++ $SDRPP_SHA"
    mkdir -p "$BUILD_DIR"
    "$SCRIPTS/prepare_sdrpp.sh" "$SDRPP_SRC"
    printf '%s' "$prep_key" >"$SDRPP_SRC/.sdrstar-prepared"
    group_end
fi

# ---------------------------------------------------------------------------------------------------------------
# 2. Configure.  The module list below is explicit: a module not named here is OFF.
#    (CMAKE_INSTALL_NAME_DIR is cleared: it is meant for the installed libraries in .deps/, SDR++'s own libraries
#    must keep their @rpath/ names because bundle_app.py resolves them through the build tree.)
# ---------------------------------------------------------------------------------------------------------------
smart_args=()
if [ "$HAVE_MODULE" = yes ]; then smart_args+=(-DOPT_BUILD_SDRSTAR_SMART=ON); fi

group_start "configure SDR++"
if [ -n "${SDRSTAR_USE_CCACHE:-}" ] && have ccache; then
    export CCACHE_BASEDIR="$ROOT" CCACHE_NOHASHDIR=1 CCACHE_SLOPPINESS=time_macros
    ccache --zero-stats >/dev/null 2>&1 || true
fi
cmake -S "$SDRPP_SRC" -B "$SDRPP_BUILD" "${CMAKE_COMMON_ARGS[@]}" \
    -DUSE_BUNDLE_DEFAULTS=ON \
    -DCMAKE_INSTALL_NAME_DIR= \
    -DOPT_BUILD_AIRSPY_SOURCE=OFF -DOPT_BUILD_AIRSPYHF_SOURCE=OFF -DOPT_BUILD_AUDIO_SOURCE=OFF \
    -DOPT_BUILD_BADGESDR_SOURCE=OFF -DOPT_BUILD_BLADERF_SOURCE=OFF -DOPT_BUILD_DRAGONLABS_SOURCE=OFF \
    -DOPT_BUILD_FILE_SOURCE=ON -DOPT_BUILD_FOBOSSDR_SOURCE=OFF -DOPT_BUILD_HACKRF_SOURCE=OFF \
    -DOPT_BUILD_HAROGIC_SOURCE=OFF -DOPT_BUILD_HERMES_SOURCE=OFF -DOPT_BUILD_HYDRASDR_SOURCE=OFF \
    -DOPT_BUILD_KCSDR_SOURCE=OFF -DOPT_BUILD_LIMESDR_SOURCE=OFF -DOPT_BUILD_NETWORK_SOURCE=ON \
    -DOPT_BUILD_PERSEUS_SOURCE=OFF -DOPT_BUILD_PLUTOSDR_SOURCE=OFF -DOPT_BUILD_RFNM_SOURCE=OFF \
    -DOPT_BUILD_RFSPACE_SOURCE=OFF -DOPT_BUILD_RTL_SDR_SOURCE=ON -DOPT_BUILD_RTL_TCP_SOURCE=ON \
    -DOPT_BUILD_SDRPP_SERVER_SOURCE=ON -DOPT_BUILD_SDRPLAY_SOURCE=OFF -DOPT_BUILD_SOAPY_SOURCE=OFF \
    -DOPT_BUILD_SPECTRAN_SOURCE=OFF -DOPT_BUILD_SPECTRAN_HTTP_SOURCE=OFF -DOPT_BUILD_SPYSERVER_SOURCE=ON \
    -DOPT_BUILD_USRP_SOURCE=OFF \
    -DOPT_BUILD_ANDROID_AUDIO_SINK=OFF -DOPT_BUILD_AUDIO_SINK=OFF -DOPT_BUILD_NETWORK_SINK=ON \
    -DOPT_BUILD_PORTAUDIO_SINK=ON -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON \
    -DOPT_BUILD_ATV_DECODER=ON -DOPT_BUILD_DAB_DECODER=OFF -DOPT_BUILD_FALCON9_DECODER=OFF \
    -DOPT_BUILD_KG_SSTV_DECODER=OFF -DOPT_BUILD_M17_DECODER=OFF -DOPT_BUILD_METEOR_DEMODULATOR=ON \
    -DOPT_BUILD_PAGER_DECODER=ON -DOPT_BUILD_RADIO=ON -DOPT_BUILD_RYFI_DECODER=OFF \
    -DOPT_BUILD_VOR_RECEIVER=OFF -DOPT_BUILD_WEATHER_SAT_DECODER=OFF \
    -DOPT_BUILD_DISCORD_PRESENCE=OFF -DOPT_BUILD_FREQUENCY_MANAGER=ON -DOPT_BUILD_IQ_EXPORTER=ON \
    -DOPT_BUILD_RECORDER=ON -DOPT_BUILD_RIGCTL_CLIENT=ON -DOPT_BUILD_RIGCTL_SERVER=ON \
    -DOPT_BUILD_SCANNER=ON -DOPT_BUILD_SCHEDULER=OFF \
    ${smart_args[@]+"${smart_args[@]}"}
group_end

# ---------------------------------------------------------------------------------------------------------------
# 3. Build
# ---------------------------------------------------------------------------------------------------------------
group_start "build SDR++"
cmake --build "$SDRPP_BUILD" --parallel "$JOBS"
group_end

if [ -n "${SDRSTAR_USE_CCACHE:-}" ] && have ccache; then ccache --show-stats 2>/dev/null || true; fi

[ -f "$SDRPP_BUILD/sdrpp" ] || die "build finished but $SDRPP_BUILD/sdrpp is missing"
log "SDR++ built: $SDRPP_BUILD ($(find "$SDRPP_BUILD" \( -name '*.dylib' -o -name '*.so' \) -not -path '*/CMakeFiles/*' | wc -l | tr -d ' ') shared libraries)"
