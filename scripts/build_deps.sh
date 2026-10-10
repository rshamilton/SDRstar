#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# build_deps.sh -- build every native library SDRstar needs, FROM SOURCE, into the private prefix .deps/<arch>.
#
# Why from source: Homebrew bottles are stamped with the OS version of the machine that built them, so linking the
# app against them would make it refuse to start on older Macs.  Everything here is compiled with
# MACOSX_DEPLOYMENT_TARGET = MACOS_DEPLOYMENT_TARGET (versions.env) instead.
#
# Idempotent: each component has a stamp; a component is rebuilt only when its pinned version/recipe changed.
# Needs no sudo and writes nothing outside the repository.
#
# Usage: scripts/build_deps.sh [--force]        (--force wipes the prefix first)
set -euo pipefail
# shellcheck source=lib/buildenv.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/buildenv.sh"

# Bump this whenever a recipe below changes in a way that must invalidate previously built components.
# (Read by build_step in lib/common.sh.)
# shellcheck disable=SC2034
RECIPE_REV=1

if [ "${1:-}" = "--force" ]; then
    log "--force: removing $DEPS_PREFIX and $DEPS_WORK"
    rm -rf "$DEPS_PREFIX" "$DEPS_WORK"
fi

setup_build_env
log "target: macOS >= $MACOS_DEPLOYMENT_TARGET, arch $ARCH, prefix $DEPS_PREFIX, $JOBS jobs, generator $CMAKE_GENERATOR_NAME"

# ---------------------------------------------------------------------------------------------------------------
# libusb: USB access for librtlsdr (autotools; the release tarball ships a generated ./configure)
# ---------------------------------------------------------------------------------------------------------------
build_libusb() {
    local src bdir extra=()
    src="$(fetch_tarball libusb "https://github.com/libusb/libusb/releases/download/$LIBUSB_TAG/libusb-${LIBUSB_TAG#v}.tar.bz2" "$LIBUSB_SHA256")"
    bdir="$DEPS_WORK/libusb"
    rm -rf "$bdir"
    mkdir -p "$bdir"
    if [ "$IS_DARWIN" = 0 ]; then extra+=(--disable-udev); fi
    (
        cd "$bdir"
        CFLAGS="$CFLAGS_BASE" LDFLAGS="$LDFLAGS_BASE" \
            "$src/configure" --prefix="$DEPS_PREFIX" --enable-shared --disable-static --disable-dependency-tracking ${extra[@]+"${extra[@]}"}
        make -j"$JOBS"
        make install
    )
}

# ---------------------------------------------------------------------------------------------------------------
# librtlsdr 2.0.3 (osmocom): the RTL-SDR Blog V4 aware driver.  Pinned commit from versions.env.
# ---------------------------------------------------------------------------------------------------------------
build_rtlsdr() {
    local src
    src="$(fetch_git rtl-sdr "$RTLSDR_REPO" "$RTLSDR_SHA")"
    cmake_build_install rtlsdr "$src" \
        -DINSTALL_UDEV_RULES=OFF -DDETACH_KERNEL_DRIVER=OFF -DENABLE_ZEROCOPY=OFF
}

# ---------------------------------------------------------------------------------------------------------------
# FFTW, single precision with threads.  SIMD kernels are chosen at run time, so one build runs on every CPU.
# ---------------------------------------------------------------------------------------------------------------
build_fftw() {
    local src bdir simd=()
    src="$(fetch_tarball fftw "$FFTW_URL" "$FFTW_SHA256")"
    bdir="$DEPS_WORK/fftw"
    rm -rf "$bdir"
    mkdir -p "$bdir"
    case "$ARCH" in
        x86_64) simd=(--enable-sse2 --enable-avx --enable-avx2) ;;
        # FFTW 3.3.10 ships a 2018 config.guess that calls Apple Silicon "arm-apple-darwin", which would make
        # configure demand an (invalid) -mfpu=neon flag.  Naming the build triple explicitly avoids guessing.
        arm64) simd=(--enable-neon --build=aarch64-apple-darwin) ;;
    esac
    (
        cd "$bdir"
        CFLAGS="$CFLAGS_BASE" LDFLAGS="$LDFLAGS_BASE" \
            "$src/configure" --prefix="$DEPS_PREFIX" --enable-single --enable-threads --enable-shared --disable-static \
            --disable-fortran --disable-doc --disable-dependency-tracking ${simd[@]+"${simd[@]}"}
        make -j"$JOBS"
        make install
    )
}

# ---------------------------------------------------------------------------------------------------------------
# VOLK: SIMD DSP kernels used by SDR++.  Needs python + Mako at build time (see ensure_mako_python).
# ---------------------------------------------------------------------------------------------------------------
build_volk() {
    local src py
    src="$(fetch_git volk "$VOLK_REPO" "$VOLK_SHA" recurse)"
    py="$(ensure_mako_python)"
    cmake_build_install volk "$src" \
        -DENABLE_TESTING=OFF -DENABLE_MODTOOL=OFF -DENABLE_PROFILING=OFF -DENABLE_ORC=OFF \
        "-DPython3_EXECUTABLE=$py" "-DPYTHON_EXECUTABLE=$py"
}

# ---------------------------------------------------------------------------------------------------------------
# GLFW (window + OpenGL context).  (On Linux -- CI dry run only -- the X11 backend is built.)
# ---------------------------------------------------------------------------------------------------------------
build_glfw() {
    local src extra=()
    src="$(fetch_git glfw "$GLFW_REPO" "$GLFW_SHA")"
    if [ "$IS_DARWIN" = 0 ]; then extra+=(-DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=ON); fi
    cmake_build_install glfw "$src" \
        -DBUILD_SHARED_LIBS=ON -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF -DGLFW_INSTALL=ON \
        ${extra[@]+"${extra[@]}"}
}

# ---------------------------------------------------------------------------------------------------------------
# zstd (shared library only; CMake project lives in build/cmake)
# ---------------------------------------------------------------------------------------------------------------
build_zstd() {
    local src
    src="$(fetch_git zstd "$ZSTD_REPO" "$ZSTD_SHA")"
    cmake_build_install zstd "$src/build/cmake" \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_CONTRIB=OFF -DZSTD_BUILD_TESTS=OFF \
        -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_SHARED=ON -DZSTD_LEGACY_SUPPORT=OFF
}

# ---------------------------------------------------------------------------------------------------------------
# PortAudio: the CoreAudio host API on macOS.  (On Linux -- CI dry run only -- ALSA.)
# ---------------------------------------------------------------------------------------------------------------
build_portaudio() {
    local src extra=()
    src="$(fetch_git portaudio "$PORTAUDIO_REPO" "$PORTAUDIO_SHA")"
    if [ "$IS_DARWIN" = 1 ]; then extra+=(-DPA_USE_COREAUDIO=ON); else extra+=(-DPA_USE_ALSA=ON -DPA_USE_JACK=OFF); fi
    cmake_build_install portaudio "$src" \
        -DPA_BUILD_SHARED=ON -DPA_BUILD_STATIC=OFF -DPA_BUILD_TESTS=OFF -DPA_BUILD_EXAMPLES=OFF \
        ${extra[@]+"${extra[@]}"}
}

# ---------------------------------------------------------------------------------------------------------------
# libsndfile without the optional Ogg/Vorbis/FLAC/Opus/MPEG libraries (DSD-neo only writes WAV)
# ---------------------------------------------------------------------------------------------------------------
build_libsndfile() {
    local src
    src="$(fetch_git libsndfile "$LIBSNDFILE_REPO" "$LIBSNDFILE_SHA")"
    cmake_build_install libsndfile "$src" \
        -DBUILD_SHARED_LIBS=ON -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
        -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DENABLE_CPACK=OFF -DINSTALL_PKGCONFIG_MODULE=ON
}

# ---------------------------------------------------------------------------------------------------------------
# OpenSSL 3.x: required by DSD-neo (encryption-key handling).  macOS's own libcrypto is LibreSSL and does not qualify.
# ---------------------------------------------------------------------------------------------------------------
build_openssl() {
    local src bdir target ossl_extra=()
    src="$(fetch_tarball openssl "$OPENSSL_URL" "$OPENSSL_SHA256")"
    bdir="$DEPS_WORK/openssl"
    rm -rf "$bdir"
    mkdir -p "$bdir"
    if [ "$IS_DARWIN" = 1 ]; then
        case "$ARCH" in
            arm64) target=darwin64-arm64-cc ;;
            x86_64) target=darwin64-x86_64-cc ;;
        esac
    else
        case "$ARCH" in
            arm64) target=linux-aarch64 ;;
            x86_64) target=linux-x86_64 ;;
        esac
    fi
    if [ "$IS_DARWIN" = 1 ]; then
        ossl_extra=("-mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET" "-Wl,-headerpad_max_install_names")
    fi
    (
        cd "$bdir"
        "$src/Configure" "$target" shared no-tests no-docs \
            "--prefix=$DEPS_PREFIX" "--openssldir=$DEPS_PREFIX/ssl" --libdir=lib \
            ${ossl_extra[@]+"${ossl_extra[@]}"}
        make -j"$JOBS"
        make install_sw
    )
}

# ---------------------------------------------------------------------------------------------------------------
# mbelib-neo: IMBE/AMBE vocoder for DSD-neo.  Pinned commit from versions.env.
# ---------------------------------------------------------------------------------------------------------------
build_mbelib() {
    local src
    src="$(fetch_git mbelib-neo "$MBELIB_NEO_REPO" "$MBELIB_NEO_SHA")"
    cmake_build_install mbelib-neo "$src" \
        -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF -DMBELIB_BUILD_DOCS=OFF \
        -DMBELIB_BUILD_BENCHMARKS=OFF -DMBELIB_BUILD_TOOLS=OFF -DMBELIB_WARNINGS_AS_ERRORS=OFF
}

# ---------------------------------------------------------------------------------------------------------------
# Codec2: M17 voice in DSD-neo.  Same commit DSD-neo's own macOS CI uses.
# ---------------------------------------------------------------------------------------------------------------
build_codec2() {
    local src
    src="$(fetch_git codec2 "$CODEC2_REPO" "$CODEC2_SHA")"
    cmake_build_install codec2 "$src" \
        -DBUILD_SHARED_LIBS=ON -DUNITTEST=OFF -DCODEC2_BUILD_PROGRAMS=OFF -DCODEC2_BUILD_DEMOS=OFF -DLPCNET=OFF
}

# ---------------------------------------------------------------------------------------------------------------
# Order matters (libusb before librtlsdr, ...).  The "key" is what the stamp compares: version/commit pins.
# ---------------------------------------------------------------------------------------------------------------
build_step libusb "$LIBUSB_TAG|$LIBUSB_SHA256" build_libusb
build_step librtlsdr "$RTLSDR_SHA" build_rtlsdr
build_step fftw "$FFTW_VERSION|$FFTW_SHA256" build_fftw
build_step volk "$VOLK_SHA|mako$MAKO_VERSION" build_volk
build_step glfw "$GLFW_SHA" build_glfw
build_step portaudio "$PORTAUDIO_SHA" build_portaudio
build_step zstd "$ZSTD_SHA" build_zstd
build_step libsndfile "$LIBSNDFILE_SHA" build_libsndfile
build_step openssl "$OPENSSL_VERSION|$OPENSSL_SHA256" build_openssl
build_step mbelib-neo "$MBELIB_NEO_SHA" build_mbelib
build_step codec2 "$CODEC2_SHA" build_codec2

log "dependency prefix ready: $DEPS_PREFIX"
for f in "$DEPS_PREFIX"/lib/*.dylib "$DEPS_PREFIX"/lib/*.so; do
    [ -e "$f" ] && note "$(basename "$f")"
done
true
