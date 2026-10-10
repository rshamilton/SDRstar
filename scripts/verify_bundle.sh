#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# verify_bundle.sh [app] -- dependency-closure, architecture, minimum-macOS, Info.plist and code-signature checks
# (see scripts/verify_bundle.py).  Fails (non-zero) on any problem.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

app="${1:-$APP_DIR}"
extra=()
if [ -f "$ROOT/modules/sdrstar_smart/CMakeLists.txt" ]; then
    extra+=(--expect-plugin sdrstar_smart)
fi
if [ "$IS_DARWIN" = 0 ]; then extra+=(--no-codesign); fi

python3 "$SCRIPTS/verify_bundle.py" --app "$app" --arch "$ARCH" --min-os "$MACOS_DEPLOYMENT_TARGET" \
    --version "$VERSION" ${extra[@]+"${extra[@]}"}
