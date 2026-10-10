#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# ci_summary.sh -- print a Markdown summary of a macOS CI run (appended to $GITHUB_STEP_SUMMARY by the workflow).
# Reads what the earlier steps left behind; every section is optional, so it works after a partial failure too.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

echo "## SDRstar macOS build: $ARCH ($DMG_ARCH)"
echo
echo "| | |"
echo "| --- | --- |"
echo "| runner | $(sw_vers -productName 2>/dev/null || echo ?) $(sw_vers -productVersion 2>/dev/null || echo ?) / $(uname -m) |"
echo "| minimum macOS of the app | $MACOS_DEPLOYMENT_TARGET |"
echo "| library cache | ${DEPS_CACHE_HIT:-n/a} (cache-hit) |"
echo "| helper cache | ${HELPERS_CACHE_HIT:-n/a} (cache-hit) |"
echo "| ccache restored from | ${CCACHE_RESTORED_KEY:-nothing} |"
if [ -d "$APP_DIR/Contents/Frameworks" ]; then
    echo "| libraries bundled | $(find "$APP_DIR/Contents/Frameworks" -name '*.dylib' | wc -l | tr -d ' ') |"
    echo "| plugins bundled | $(find "$APP_DIR/Contents/Plugins" -name '*.dylib' | wc -l | tr -d ' ') |"
    echo "| app size | $(du -sh "$APP_DIR" | cut -f1) |"
fi
dmg="$DIST_DIR/SDRstar-$DMG_ARCH.dmg"
if [ -f "$dmg" ]; then
    echo "| dmg | \`$(basename "$dmg")\`, $(du -h "$dmg" | cut -f1), sha256 \`$(cut -d' ' -f1 "$dmg.sha256")\` |"
fi
echo

if [ -d "$APP_DIR/Contents/Frameworks" ]; then
    echo "### Bundled libraries"
    echo '```'
    find "$APP_DIR/Contents/Frameworks" -name '*.dylib' -exec du -h {} + | sed "s#$APP_DIR/Contents/Frameworks/##" | sort -k2
    echo '```'
fi
f="$DIST_DIR/logs/verify-$ARCH.log"
if [ -f "$f" ]; then
    echo "### Bundle verification"
    echo '```'
    grep -E '^\[[0-9]\]|highest minos|libraries in Frameworks|VERIFY|FAIL' "$f" || true
    echo '```'
fi
if [ -f "$DIST_DIR/launch-$ARCH.log" ]; then
    echo "### Launch test"
    echo '```'
    sed -n '/== SUMMARY/,$p' "$DIST_DIR/launch-$ARCH.log"
    grep -E 'window server session|^== macOS' "$DIST_DIR/launch-$ARCH.log" | head -n 2 || true
    echo '```'
fi
if [ -f "$BUILD_DIR/core/ctest.log" ]; then
    echo "### Core tests (ctest)"
    echo '```'
    grep -E 'Test +#|tests passed|tests failed' "$BUILD_DIR/core/ctest.log" || true
    echo '```'
fi
