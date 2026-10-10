#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# check_tools.sh -- verify that the tools needed to build SDRstar on a Mac are present.
#
# This script NEVER installs anything.  If something is missing it stops and prints exactly what to install and why,
# so you can decide (the project's rule: ask before installing anything global, such as Homebrew packages).
# Exit status: 0 = everything needed is present, 1 = something is missing.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

[ "$IS_DARWIN" = 1 ] || die "this checks a macOS build machine; the pipeline builds the app on macOS only (use CI for other systems)"

missing_xcode=()
missing_brew=()
reasons=()

need_xcode() { # tool, why
    if ! have "$1"; then
        missing_xcode+=("$1")
        reasons+=("  - $1: $2")
    fi
}
need_brew() { # tool, brew-package, why
    if ! have "$1"; then
        missing_brew+=("$2")
        reasons+=("  - $1 (Homebrew package '$2'): $3")
    fi
}

# --- Apple command line tools (compiler, make, git, otool, codesign, hdiutil, ...) ------------------------------
if ! xcode-select -p >/dev/null 2>&1; then
    reasons+=("  - Xcode Command Line Tools: provide the C/C++ compiler, make, git, otool, install_name_tool, codesign, hdiutil, iconutil")
    missing_xcode+=("Xcode Command Line Tools")
else
    for t in clang clang++ make git otool install_name_tool lipo codesign hdiutil iconutil ditto curl tar rsync perl shasum python3; do
        need_xcode "$t" "part of macOS / the Xcode Command Line Tools"
    done
fi

# --- tools Apple does not ship ------------------------------------------------------------------------------------
need_brew cmake cmake "configures and builds SDR++, DSD-neo, libraries ... (version 3.20 or newer)"
if ! have pkg-config && ! have pkgconf; then
    missing_brew+=(pkgconf)
    reasons+=("  - pkg-config (Homebrew package 'pkgconf'): lets SDR++ find the libraries built into .deps/")
fi

# python3 must be able to create a virtualenv (VOLK needs the Mako template library at build time; it is installed
# into .deps/venv inside this folder, never globally).
if have python3 && ! python3 -c 'import venv, ensurepip' >/dev/null 2>&1; then
    missing_brew+=(python)
    reasons+=("  - a complete python3 (with venv/pip): the one on this Mac cannot create a virtual environment")
fi

if have cmake; then
    cmake_ver="$(cmake --version | head -n1 | awk '{print $3}')"
    if [ "$(printf '%s\n3.20\n' "$cmake_ver" | sort -V | head -n1)" != "3.20" ]; then
        missing_brew+=(cmake)
        reasons+=("  - cmake is too old ($cmake_ver); need 3.20 or newer (brew upgrade cmake)")
    fi
fi

if [ "${#reasons[@]}" -gt 0 ]; then
    echo "SDRstar cannot be built yet: some tools are missing. NOTHING was installed."
    echo
    echo "Missing, and why it is needed:"
    printf '%s\n' "${reasons[@]}"
    echo
    echo "Please install them yourself, then run this script again:"
    if [ "${#missing_xcode[@]}" -gt 0 ]; then
        echo "  1) Apple's developer tools (free, about 1-2 GB, a window will ask for confirmation):"
        echo "       xcode-select --install"
    fi
    if [ "${#missing_brew[@]}" -gt 0 ]; then
        # de-duplicate
        pkgs="$(printf '%s\n' "${missing_brew[@]}" | sort -u | tr '\n' ' ')"
        echo "  2) With Homebrew (https://brew.sh; if you do not use Homebrew, any other way to get these tools works too):"
        echo "       brew install $pkgs"
    fi
    echo
    echo "Optional (faster rebuilds): brew install ninja ccache"
    exit 1
fi

log "all required tools are present"
note "cmake $(cmake --version | head -n1 | awk '{print $3}'), python3 $(python3 --version 2>&1 | awk '{print $2}'), $(clang --version | head -n1)"
if have ninja; then note "ninja $(ninja --version) (will be used)"; else note "ninja not found: using Makefiles (slower; optional: brew install ninja)"; fi
if have ccache; then note "ccache found (will be used)"; else note "ccache not found (optional: brew install ccache)"; fi
