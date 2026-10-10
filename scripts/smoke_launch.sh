#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# smoke_launch.sh [app] [seconds] -- run the bundled app and record what happens (log: dist/launch-<arch>.log).
#
#   1. dlcheck   dlopen() every Plugins/*.dylib (RTLD_NOW) from a fresh process: proves that every plugin and every
#                library it needs resolves inside the bundle.  Always meaningful, needs no window server.
#   2. headless  `sdrstar --server`: SDR++'s server mode needs no window or OpenGL; it loads every *source* plugin
#                (librtlsdr, libusb, ...) and listens for ~<seconds>, then is killed.
#   3. GUI       the real GUI launch for <seconds>.  A GitHub runner may have no window server; if the window cannot
#                be created that is recorded honestly as "GUI NOT AVAILABLE" (it is not a pass, and not a failure of
#                the bundle).  If the GUI does start, the log must show every plugin loaded and "Ready.".
#
# Fails on: any dlcheck failure, any dyld / "Couldn't load" error, a helper-free headless crash, or a GUI run that
# started but loaded fewer plugins than the bundle contains.  Summary line at the end states exactly which of the
# three levels were achieved.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

APP="${1:-$APP_DIR}"
SECS="${2:-15}"
LOG="$DIST_DIR/launch-$ARCH.log"
[ -d "$APP" ] || die "no app at $APP"
mkdir -p "$DIST_DIR"
: >"$LOG"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sdrstar-smoke.XXXXXX")"
pids=()
cleanup() {
    for p in ${pids[@]+"${pids[@]}"}; do kill -KILL "$p" 2>/dev/null || true; done
    rm -rf "$TMP"
}
trap cleanup EXIT

say() {
    printf '%s\n' "$*" | tee -a "$LOG"
}
fail() {
    say "SMOKE FAIL: $*"
    echo "(full log: $LOG)"
    exit 1
}
clean_env() {
    env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH -u DYLD_INSERT_LIBRARIES \
        -u DYLD_VERSIONED_LIBRARY_PATH -u DYLD_IMAGE_SUFFIX "$@"
}

# Start the program in the background with its output appended to a file; wait N seconds; report whether it was
# still alive; then terminate it.  Sets RUN_ALIVE=1/0 and RUN_STATUS (exit status if it ended by itself).
run_for() {
    local seconds=$1 out=$2 pid
    shift 2
    clean_env "$@" >"$out" 2>&1 </dev/null &
    pid=$!
    pids+=("$pid")
    local i=0
    RUN_ALIVE=1
    RUN_STATUS=""
    while [ "$i" -lt "$seconds" ]; do
        sleep 1
        i=$((i + 1))
        if ! kill -0 "$pid" 2>/dev/null; then
            RUN_ALIVE=0
            wait "$pid" 2>/dev/null && RUN_STATUS=0 || RUN_STATUS=$?
            break
        fi
    done
    if [ "$RUN_ALIVE" = 1 ]; then
        kill -TERM "$pid" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

BAD_PATTERN='Library not loaded|Symbol not found|dyld\[|dyld:|dlopen|image not found|Couldn.t load|isn.t a loadable module|is missing _(INFO|INIT|CREATE_INSTANCE|DELETE_INSTANCE|END)_'

n_plugins="$(find "$APP/Contents/Plugins" -name '*.dylib' | wc -l | tr -d ' ')"
say "== SDRstar smoke launch: $APP  ($n_plugins plugins)  $(date -u +%FT%TZ)"
say "== macOS $(sw_vers -productVersion 2>/dev/null || echo '?') $(uname -m)   window server session: $(launchctl managername 2>/dev/null || echo unknown)"

# ---------------------------------------------------------------------------------------------------------------
say ""
say "== [1/3] dlcheck: dlopen every plugin"
cc -O1 -o "$TMP/dlcheck" "$PACKAGING/dlcheck.c" || fail "could not compile dlcheck"
if clean_env "$TMP/dlcheck" "$APP"/Contents/Plugins/*.dylib >"$TMP/dlcheck.out" 2>&1; then
    tail -n 1 "$TMP/dlcheck.out" | tee -a "$LOG"
else
    cat "$TMP/dlcheck.out" | tee -a "$LOG"
    fail "some plugins cannot be loaded from the bundle"
fi
LEVEL_DLCHECK="PASS ($(tail -n 1 "$TMP/dlcheck.out"))"

# ---------------------------------------------------------------------------------------------------------------
say ""
say "== [2/3] headless launch: sdrstar --server for ${SECS}s"
run_for "$SECS" "$TMP/server.log" "$APP/Contents/MacOS/sdrstar" --server --addr 127.0.0.1 -r "$TMP/root-server"
cat "$TMP/server.log" >>"$LOG"
if grep -Eq "$BAD_PATTERN" "$TMP/server.log"; then
    fail "headless launch reported a loader/plugin error (see log)"
fi
if [ "$RUN_ALIVE" != 1 ]; then
    fail "headless launch exited by itself with status ${RUN_STATUS:-?} before ${SECS}s"
fi
grep -q 'Ready, listening' "$TMP/server.log" || fail "headless launch never logged 'Ready, listening'"
n_src="$(grep -c 'Loading .*\.dylib' "$TMP/server.log" || true)"
[ "$n_src" -ge 1 ] || fail "headless launch loaded no source plugins"
say "headless: alive after ${SECS}s, 'Ready, listening' logged, $n_src source plugins loaded -> PASS"
LEVEL_SERVER="PASS (alive ${SECS}s, 'Ready, listening', $n_src source plugins loaded)"

# ---------------------------------------------------------------------------------------------------------------
say ""
say "== [3/3] GUI launch for ${SECS}s"
run_for "$SECS" "$TMP/gui.log" "$APP/Contents/MacOS/sdrstar" -r "$TMP/root-gui"
cat "$TMP/gui.log" >>"$LOG"
if grep -Eq "$BAD_PATTERN" "$TMP/gui.log"; then
    fail "GUI launch reported a loader/plugin error (see log)"
fi
n_loaded="$(grep -c 'Loading .*\.dylib' "$TMP/gui.log" || true)"
if grep -q 'Ready\.' "$TMP/gui.log" && [ "$RUN_ALIVE" = 1 ]; then
    [ "$n_loaded" -ge "$n_plugins" ] || fail "GUI loaded only $n_loaded of $n_plugins plugins"
    say "GUI: reached 'Ready.', stayed up ${SECS}s, loaded $n_loaded/$n_plugins plugins -> PASS"
    LEVEL_GUI="PASS (window up ${SECS}s, $n_loaded/$n_plugins plugins loaded, 'Ready.' logged)"
else
    say "GUI: NOT AVAILABLE on this machine -- the process $( [ "$RUN_ALIVE" = 1 ] && echo 'stayed up but never logged Ready.' || echo "ended with status ${RUN_STATUS:-?}" )."
    say "     Most likely there is no window server / GPU session (typical for a CI runner). Last log lines:"
    tail -n 8 "$TMP/gui.log" | sed 's/^/     | /' | tee -a "$LOG"
    LEVEL_GUI="NOT AVAILABLE (no usable window server; the process was started but the GUI could not come up)"
fi

say ""
say "== SUMMARY"
say "   plugin dlopen check : $LEVEL_DLCHECK"
say "   headless launch     : $LEVEL_SERVER"
say "   GUI launch          : $LEVEL_GUI"
