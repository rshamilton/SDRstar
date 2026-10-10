#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# bundle_mac.sh -- turn the SDR++ build tree + helper programs into dist/<arch>/SDRstar.app, self-contained and
# ad-hoc signed.  The real work is in scripts/bundle_app.py; this wrapper gathers the inputs.
#
# Needs: build_deps.sh, build_helpers.sh and build_sdrpp.sh to have run.   Usage: scripts/bundle_mac.sh
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

[ "$IS_DARWIN" = 1 ] || die "bundling needs the macOS toolchain (otool, install_name_tool, codesign)"
[ -f "$SDRPP_BUILD/sdrpp" ] || die "SDR++ has not been built; run scripts/build_sdrpp.sh first"
for h in dsd-neo multimon-ng rtl_433; do
    [ -x "$HELPERS_DIR/$h" ] || die "helper $h is missing; run scripts/build_helpers.sh first"
done

mkdir -p "$BUILD_DIR" "$DIST_DIR/$ARCH"

# --- icon -----------------------------------------------------------------------------------------------------
ICNS="$BUILD_DIR/SDRstar.icns"
if [ ! -f "$ICNS" ] || [ "$PACKAGING/make_icon.py" -nt "$ICNS" ]; then
    log "drawing the app icon"
    python3 "$PACKAGING/make_icon.py" --icns "$ICNS"
fi

# --- build info (what exactly is inside this app: needed for the GPL "source offer") ------------------------------
BUILD_NUMBER="${GITHUB_RUN_NUMBER:-$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)}"
COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
INFO="$BUILD_DIR/BUILD-INFO.txt"
{
    echo "SDRstar $VERSION (build $BUILD_NUMBER)"
    echo "SDRstar source commit: $COMMIT"
    echo "Architecture: $ARCH    Minimum macOS: $MACOS_DEPLOYMENT_TARGET"
    echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "Exact upstream sources (every component is built from these pinned revisions):"
    echo "  SDR++            $SDRPP_REPO @ $SDRPP_SHA"
    echo "  DSD-neo          $DSDNEO_REPO @ $DSDNEO_SHA"
    echo "  mbelib-neo       $MBELIB_NEO_REPO @ $MBELIB_NEO_SHA"
    echo "  librtlsdr        $RTLSDR_REPO @ $RTLSDR_SHA"
    echo "  multimon-ng      $MULTIMON_REPO @ $MULTIMON_SHA (tag $MULTIMON_TAG)"
    echo "  rtl_433          $RTL433_REPO @ $RTL433_SHA (tag $RTL433_TAG)"
    echo "  VOLK             $VOLK_REPO @ $VOLK_SHA (tag $VOLK_TAG)"
    echo "  libusb           $LIBUSB_TAG"
    echo "  FFTW             $FFTW_VERSION"
    echo "  GLFW             $GLFW_REPO @ $GLFW_SHA (tag $GLFW_TAG)"
    echo "  zstd             $ZSTD_REPO @ $ZSTD_SHA (tag $ZSTD_TAG)"
    echo "  PortAudio        $PORTAUDIO_REPO @ $PORTAUDIO_SHA (tag $PORTAUDIO_TAG)"
    echo "  libsndfile       $LIBSNDFILE_REPO @ $LIBSNDFILE_SHA (tag $LIBSNDFILE_TAG)"
    echo "  OpenSSL          $OPENSSL_VERSION"
    echo "  Codec2           $CODEC2_REPO @ $CODEC2_SHA"
} >"$INFO"

# --- assemble, relocate, sign ----------------------------------------------------------------------------------
extra=()
if [ -f "$ROOT/modules/sdrstar_smart/CMakeLists.txt" ]; then
    extra+=(--expect-plugin sdrstar_smart)
fi
python3 "$SCRIPTS/bundle_app.py" \
    --app "$APP_DIR" \
    --build-dir "$SDRPP_BUILD" \
    --sdrpp-src "$SDRPP_SRC" \
    --helpers-dir "$HELPERS_DIR" \
    --search-dir "$DEPS_PREFIX/lib" \
    --allowed-root "$DEPS_PREFIX" --allowed-root "$SDRPP_BUILD" \
    --plist-template "$PACKAGING/Info.plist.in" \
    --version "$VERSION" --build-number "$BUILD_NUMBER" --min-os "$MACOS_DEPLOYMENT_TARGET" \
    --icon "$ICNS" \
    --license-file "$ROOT/LICENSE" --license-file "$ROOT/THIRD_PARTY.md" --license-file "$ROOT/versions.env" \
    --build-info "$INFO" \
    --sign

log "app ready: $APP_DIR ($(du -sh "$APP_DIR" | cut -f1))"
