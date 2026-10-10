# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash source-path=SCRIPTDIR
# shellcheck disable=SC2034  # this file only defines variables that the scripts sourcing it consume
#
# Shared helpers for the SDRstar macOS build scripts.  This file is SOURCED, never executed.
#
# It provides:
#   * logging (log / warn / die, collapsible groups on GitHub Actions)
#   * the pinned versions (versions.env + scripts/deps.env) and the directory layout
#   * the build environment that keeps every compile hermetic (deployment target, private prefix, no Homebrew)
#   * fetch helpers (pinned git commits, checksummed tarballs) and "stamp" helpers that make each build step
#     idempotent
#
# Directory layout (all inside the repository, nothing outside it is ever written to):
#   .deps/src/            downloaded / cloned sources            (not cached in CI)
#   .deps/work-<arch>/    out-of-tree build directories           (not cached in CI)
#   .deps/<arch>/         private install prefix: lib, include, pkgconfig, .stamps/        (cached in CI)
#   .deps/<arch>/helpers/ dsd-neo, multimon-ng, rtl_433, fixtures/iq, .stamps/            (cached separately in CI)
#   .deps/venv/           python virtualenv holding Mako for VOLK (only if the system python lacks it)
#   build/<arch>/         SDR++ build tree, core test build
#   dist/<arch>/          SDRstar.app;  dist/*.dmg  the disk images

if [ -n "${SDRSTAR_COMMON_LOADED:-}" ]; then
    return 0
fi
SDRSTAR_COMMON_LOADED=1

# ---------------------------------------------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------------------------------------------
log() { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

# Collapsible log sections when running on GitHub Actions; plain headers elsewhere.
group_start() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then printf '::group::%s\n' "$*"; else printf '\n===== %s =====\n' "$*"; fi
}
group_end() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then printf '::endgroup::\n'; fi
}

# retry <attempts> <command...>: re-run a flaky network command with a growing pause.
retry() {
    local attempts=$1 n=1
    shift
    until "$@"; do
        if [ "$n" -ge "$attempts" ]; then
            return 1
        fi
        warn "command failed (attempt $n/$attempts): $*  -- retrying"
        sleep $((n * 5))
        n=$((n + 1))
    done
}

# ---------------------------------------------------------------------------------------------------------------
# Repository root, pinned versions, platform
# ---------------------------------------------------------------------------------------------------------------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPTS="$ROOT/scripts"
PACKAGING="$ROOT/packaging"

# shellcheck source=../../versions.env
source "$ROOT/versions.env"
# shellcheck source=../deps.env
source "$SCRIPTS/deps.env"

HOST_OS="$(uname -s)"
case "$HOST_OS" in
    Darwin) IS_DARWIN=1 ;;
    Linux)
        # The Linux branch exists only so the recipes can be dry-run in CI / on a dev box.  It never produces an app.
        IS_DARWIN=0
        ;;
    *) die "unsupported host OS: $HOST_OS" ;;
esac

# ARCH is the native CPU architecture as macOS spells it: arm64 or x86_64.
case "$(uname -m)" in
    arm64 | aarch64) ARCH=arm64 ;;
    x86_64 | amd64) ARCH=x86_64 ;;
    *) die "unsupported CPU architecture: $(uname -m)" ;;
esac
case "$ARCH" in
    arm64) DMG_ARCH=AppleSilicon ;;
    x86_64) DMG_ARCH=Intel ;;
esac

VERSION="$(tr -d '[:space:]' <"$ROOT/VERSION")"

JOBS="${SDRSTAR_JOBS:-$( (sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4) | head -n1)}"

# Directory layout
DEPS_ROOT="${SDRSTAR_DEPS_ROOT:-$ROOT/.deps}"
DEPS_SRC="$DEPS_ROOT/src"
DEPS_WORK="$DEPS_ROOT/work-$ARCH"
DEPS_PREFIX="$DEPS_ROOT/$ARCH"
HELPERS_DIR="$DEPS_PREFIX/helpers"
FIXTURES_DIR="$HELPERS_DIR/fixtures/iq"
STAMP_DIR="$DEPS_PREFIX/.stamps"
VENV_DIR="$DEPS_ROOT/venv"
BUILD_DIR="$ROOT/build/$ARCH"
SDRPP_SRC="$BUILD_DIR/sdrpp-src"
SDRPP_BUILD="$BUILD_DIR/sdrpp-build"
DIST_DIR="$ROOT/dist"
APP_DIR="$DIST_DIR/$ARCH/SDRstar.app"

# ---------------------------------------------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

sha256_of() {
    if have shasum; then shasum -a 256 "$1" | awk '{print $1}'; else sha256sum "$1" | awk '{print $1}'; fi
}

# Picks the CMake generator once: Ninja when available (faster), otherwise plain Makefiles.
pick_generator() {
    if have ninja; then
        CMAKE_GENERATOR_NAME="Ninja"
    else
        CMAKE_GENERATOR_NAME="Unix Makefiles"
    fi
}

# run_with_timeout <seconds> <command...>: portable replacement for coreutils' timeout (macOS has none).
# Returns the command's exit status, or 137 (SIGKILL) if it had to be killed.
run_with_timeout() {
    local secs=$1 pid watcher rc=0
    shift
    # "<&0": a background job of a non-interactive shell would otherwise get /dev/null as stdin.
    "$@" <&0 &
    pid=$!
    (
        sleep "$secs"
        kill -KILL "$pid" 2>/dev/null
    ) >/dev/null 2>&1 &
    watcher=$!
    wait "$pid" || rc=$?
    kill "$watcher" 2>/dev/null || true
    wait "$watcher" 2>/dev/null || true
    return "$rc"
}

# strip_ansi: remove terminal colour escape sequences from stdin.
strip_ansi() { LC_ALL=C sed $'s/\033\\[[0-9;]*m//g'; }

# ---------------------------------------------------------------------------------------------------------------
# Fetching sources
# ---------------------------------------------------------------------------------------------------------------
# fetch_git <name> <url> <pin> [recurse]
#   Fetches exactly <pin> (a commit sha, or the sha of an annotated tag object that peels to a commit) shallowly
#   into $DEPS_SRC/<name>-<pin12> and prints that directory on stdout (log lines go to stderr).
fetch_git() {
    local name=$1 url=$2 pin=$3 recurse=${4:-}
    local dest="$DEPS_SRC/$name-${pin:0:12}"
    if [ -f "$dest/.sdrstar-ok" ]; then
        echo "$dest"
        return 0
    fi
    log "fetching $name @ ${pin:0:12} from $url" >&2
    rm -rf "$dest"
    mkdir -p "$dest"
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$url"
    if ! retry 3 git -C "$dest" fetch -q --depth 1 origin "$pin" >&2; then
        warn "shallow fetch of $pin refused by $url; falling back to a full fetch"
        retry 3 git -C "$dest" fetch -q origin >&2 || die "cannot fetch $url"
    fi
    git -C "$dest" -c advice.detachedHead=false checkout -q "$pin" >&2 2>/dev/null || git -C "$dest" -c advice.detachedHead=false checkout -q FETCH_HEAD >&2
    local head peeled
    head="$(git -C "$dest" rev-parse HEAD)"
    peeled="$(git -C "$dest" rev-parse "$pin^{commit}" 2>/dev/null || echo none)"
    if [ "$head" != "$pin" ] && [ "$head" != "$peeled" ]; then
        die "$name: fetched commit $head does not match pinned $pin (peeled: $peeled)"
    fi
    if [ -n "$recurse" ]; then
        retry 3 git -C "$dest" submodule update --init --depth 1 --recursive >&2 || die "$name: submodule fetch failed"
    fi
    touch "$dest/.sdrstar-ok"
    echo "$dest"
}

# fetch_tarball <name> <url> <sha256>
#   Downloads (once), verifies the checksum and unpacks into $DEPS_SRC/<name>-<sha12>; prints that directory.
fetch_tarball() {
    local name=$1 url=$2 sha=$3
    local dl="$DEPS_SRC/dl"
    local file dest
    file="$dl/$(basename "$url")"
    dest="$DEPS_SRC/$name-${sha:0:12}"
    mkdir -p "$dl"
    if [ -f "$dest/.sdrstar-ok" ]; then
        echo "$dest"
        return 0
    fi
    if [ ! -f "$file" ]; then
        log "downloading $url" >&2
        retry 3 curl -fsSL --connect-timeout 30 -o "$file.part" "$url" >&2 || die "download failed: $url"
        mv "$file.part" "$file"
    fi
    local got
    got="$(sha256_of "$file")"
    if [ "$got" != "$sha" ]; then
        rm -f "$file"
        die "checksum mismatch for $(basename "$url"): expected $sha but got $got.  If you intentionally changed the version, update the checksum in scripts/deps.env (or versions.env)."
    fi
    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xf "$file" -C "$dest" --strip-components=1
    touch "$dest/.sdrstar-ok"
    echo "$dest"
}
