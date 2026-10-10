# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash source-path=SCRIPTDIR
# shellcheck disable=SC2034  # this file only defines variables that the scripts sourcing it consume
#
# Hermetic build environment and incremental-build helpers for the macOS dependency / helper / SDR++ builds.
# SOURCED after lib/common.sh by build_deps.sh, build_helpers.sh and build_sdrpp.sh.  The CI dependency-cache key
# hashes THIS file (together with versions.env, deps.env and build_deps.sh), so a change to the build flags here
# automatically invalidates cached libraries.

if [ -n "${SDRSTAR_BUILDENV_LOADED:-}" ]; then
    return 0
fi
SDRSTAR_BUILDENV_LOADED=1
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# ---------------------------------------------------------------------------------------------------------------
# Hermetic build environment
# ---------------------------------------------------------------------------------------------------------------
# Everything is compiled for the SAME minimum macOS (MACOS_DEPLOYMENT_TARGET from versions.env) and installed into a
# private prefix.  Homebrew libraries are never used: their bottles are stamped with the build machine's OS version
# and would make the finished app refuse to start on older Macs.
#
# After calling this, use "${CMAKE_COMMON_ARGS[@]}" for every CMake configure.
setup_build_env() {
    pick_generator
    mkdir -p "$DEPS_PREFIX/lib" "$DEPS_PREFIX/include" "$STAMP_DIR" "$DEPS_SRC" "$DEPS_WORK"

    # Anything inherited from a developer shell (Homebrew's shellenv, conda, ...) that could leak foreign
    # libraries into the build is dropped.
    unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH LIBRARY_PATH \
        PKG_CONFIG_PATH CMAKE_PREFIX_PATH CMAKE_INCLUDE_PATH CMAKE_LIBRARY_PATH CMAKE_FRAMEWORK_PATH \
        DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES LD_LIBRARY_PATH \
        CFLAGS CXXFLAGS CPPFLAGS LDFLAGS

    # pkg-config must see ONLY our prefix (PKG_CONFIG_LIBDIR replaces the built-in search path, unlike
    # PKG_CONFIG_PATH which merely prepends to it).
    export PKG_CONFIG_LIBDIR="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/share/pkgconfig"
    unset PKG_CONFIG_SYSROOT_DIR

    export CMAKE_POLICY_VERSION_MINIMUM=3.5 # CMake 4 refuses old projects (cmake_minimum_required < 3.5) otherwise

    local pkgcfg py
    pkgcfg="$(command -v pkg-config || command -v pkgconf || true)"
    py="$(command -v python3 || true)"

    # Flags shared by every configure step ("hermetic CMake").
    CMAKE_COMMON_ARGS=(
        -G "$CMAKE_GENERATOR_NAME"
        -DCMAKE_BUILD_TYPE=Release
        "-DCMAKE_INSTALL_PREFIX=$DEPS_PREFIX"
        "-DCMAKE_PREFIX_PATH=$DEPS_PREFIX"
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_FIND_FRAMEWORK=LAST
        -DCMAKE_FIND_APPBUNDLE=NEVER
    )
    if [ -n "$pkgcfg" ]; then CMAKE_COMMON_ARGS+=("-DPKG_CONFIG_EXECUTABLE=$pkgcfg"); fi
    if [ -n "$py" ]; then CMAKE_COMMON_ARGS+=("-DPython3_EXECUTABLE=$py" "-DPYTHON_EXECUTABLE=$py"); fi
    if [ "$CMAKE_GENERATOR_NAME" = "Ninja" ]; then CMAKE_COMMON_ARGS+=("-DCMAKE_MAKE_PROGRAM=$(command -v ninja)"); fi
    if [ -n "${SDRSTAR_USE_CCACHE:-}" ] && have ccache; then
        CMAKE_COMMON_ARGS+=("-DCMAKE_C_COMPILER_LAUNCHER=$(command -v ccache)" "-DCMAKE_CXX_COMPILER_LAUNCHER=$(command -v ccache)")
    fi

    if [ "$IS_DARWIN" = 1 ]; then
        # shellcheck disable=SC2153  # MACOS_DEPLOYMENT_TARGET (with the S) is the name used in versions.env
        export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
        # Never pick up Homebrew (/opt/homebrew on Apple Silicon, /usr/local on Intel) by accident.
        CMAKE_COMMON_ARGS+=(
            "-DCMAKE_OSX_ARCHITECTURES=$ARCH"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET"
            "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local;/sw"
            # Installed libs get an absolute install name inside our prefix; the bundling step rewrites them to
            # @rpath/... when it copies them into the .app.
            "-DCMAKE_INSTALL_NAME_DIR=$DEPS_PREFIX/lib"
            # Leave room in the Mach-O headers so install_name_tool can later rewrite to longer names.
            "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-headerpad_max_install_names"
            "-DCMAKE_MODULE_LINKER_FLAGS=-Wl,-headerpad_max_install_names"
            "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-headerpad_max_install_names"
        )
        # Flags for autotools-style builds (libusb, FFTW) and OpenSSL.
        CFLAGS_BASE="-O2 -arch $ARCH -mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET"
        LDFLAGS_BASE="-arch $ARCH -mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names"
    else
        CFLAGS_BASE="-O2 -fPIC"
        LDFLAGS_BASE=""
    fi
}

# cmake_build_install <name> <source-dir> [extra cmake args...]
#   Configures into $DEPS_WORK/<name>, builds, installs into $DEPS_PREFIX.  The work dir is wiped first so a
#   rebuild after a recipe change can never reuse stale CMake state.
cmake_build_install() {
    local name=$1 src=$2
    shift 2
    local bdir="$DEPS_WORK/$name"
    rm -rf "$bdir"
    cmake -S "$src" -B "$bdir" "${CMAKE_COMMON_ARGS[@]}" "$@"
    cmake --build "$bdir" --parallel "$JOBS"
    cmake --install "$bdir"
}

# ---------------------------------------------------------------------------------------------------------------
# Idempotence: stamps
# ---------------------------------------------------------------------------------------------------------------
# A stamp records "<what was built>" for one component.  A component is rebuilt only when its stamp is missing or
# differs, so re-running a script is cheap and a pin bump rebuilds exactly the affected parts.
stamp_matches() { [ -f "$STAMP_DIR/$1" ] && [ "$(cat "$STAMP_DIR/$1")" = "$2" ]; }
stamp_write() {
    mkdir -p "$STAMP_DIR"
    printf '%s' "$2" >"$STAMP_DIR/$1"
}

# build_step <name> <key> <function>: run <function> unless the stamp for <name> already equals <key>.
build_step() {
    local name=$1 key=$2 fn=$3
    local full_key="$key|$ARCH|$MACOS_DEPLOYMENT_TARGET|rev$RECIPE_REV"
    if stamp_matches "$name" "$full_key"; then
        log "$name: up to date ($key)"
        return 0
    fi
    group_start "build $name ($key)"
    local t0=$SECONDS
    "$fn"
    stamp_write "$name" "$full_key"
    log "$name: built in $((SECONDS - t0))s"
    group_end
}

# Python interpreter that can import Mako (VOLK needs it at build time).  Uses the system python3 when it already
# has Mako, otherwise creates the repo-local virtualenv .deps/venv (no global side effects) and installs Mako there.
ensure_mako_python() {
    local sys_py
    sys_py="$(command -v python3)" || die "python3 not found"
    if "$sys_py" -c 'import mako' >/dev/null 2>&1; then
        echo "$sys_py"
        return 0
    fi
    if [ ! -x "$VENV_DIR/bin/python3" ] || ! "$VENV_DIR/bin/python3" -c 'import mako' >/dev/null 2>&1; then
        log "creating the local virtualenv $VENV_DIR with Mako $MAKO_VERSION (needed by VOLK; not installed globally)" >&2
        rm -rf "$VENV_DIR"
        "$sys_py" -m venv "$VENV_DIR" >&2 || die "python3 -m venv failed; install a complete python3 (see docs/MAC_BUILD.md)"
        retry 3 "$VENV_DIR/bin/python3" -m pip install --disable-pip-version-check --quiet "Mako==$MAKO_VERSION" >&2 ||
            die "could not install Mako into $VENV_DIR"
    fi
    echo "$VENV_DIR/bin/python3"
}
