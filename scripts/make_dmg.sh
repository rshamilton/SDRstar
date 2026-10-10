#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# make_dmg.sh [app] -- package dist/<arch>/SDRstar.app into dist/SDRstar-<AppleSilicon|Intel>.dmg (+ .sha256).
# The disk image holds the app, a shortcut to /Applications and a plain-language "READ ME FIRST.txt".  Afterwards the
# image is verified, mounted read-only and the app inside is checked for an intact code signature.
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

[ "$IS_DARWIN" = 1 ] || die "making a .dmg needs macOS (hdiutil)"
app="${1:-$APP_DIR}"
[ -d "$app" ] || die "no app at $app; run scripts/bundle_mac.sh first"
dmg="$DIST_DIR/SDRstar-$DMG_ARCH.dmg"
mkdir -p "$DIST_DIR"

stage="$(mktemp -d "$DIST_DIR/.dmg-stage.XXXXXX")"
mnt=""
cleanup() {
    if [ -n "$mnt" ] && [ -d "$mnt" ]; then hdiutil detach "$mnt" -force >/dev/null 2>&1 || true; fi
    rm -rf "$stage"
}
trap cleanup EXIT

log "staging the disk image contents"
ditto "$app" "$stage/SDRstar.app" # ditto keeps the code signature, permissions and extended attributes intact
ln -s /Applications "$stage/Applications"
cp "$PACKAGING/README-dmg.txt" "$stage/READ ME FIRST.txt"

log "creating $dmg"
rm -f "$dmg" "$dmg.sha256"
# hdiutil occasionally fails with "Resource busy" on CI runners; retry.
retry 3 hdiutil create -volname SDRstar -srcfolder "$stage" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$dmg"

log "verifying the image"
hdiutil verify "$dmg"
mnt="$(mktemp -d "${TMPDIR:-/tmp}/sdrstar-mnt.XXXXXX")"
hdiutil attach "$dmg" -readonly -nobrowse -noverify -mountpoint "$mnt" >/dev/null
[ -d "$mnt/SDRstar.app" ] || die "the mounted image does not contain SDRstar.app"
codesign --verify --deep --strict --verbose=2 "$mnt/SDRstar.app"
[ -L "$mnt/Applications" ] || die "the Applications shortcut is missing from the image"
[ -f "$mnt/READ ME FIRST.txt" ] || die "READ ME FIRST.txt is missing from the image"
hdiutil detach "$mnt" >/dev/null || hdiutil detach "$mnt" -force >/dev/null
mnt=""

(cd "$DIST_DIR" && shasum -a 256 "$(basename "$dmg")" >"$(basename "$dmg").sha256")
log "done: $dmg ($(du -h "$dmg" | cut -f1))"
cat "$dmg.sha256"
