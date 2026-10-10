#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck source-path=SCRIPTDIR
#
# test_bundler_logic.sh -- exercise scripts/bundle_app.py and scripts/verify_bundle.py on LINUX, without a Mac.
#
# It fabricates tiny but genuine Mach-O dylibs/executables with clang + ld64.lld (freestanding code, platform macOS
# 12.0) that mimic what the real build produces: a library with an absolute install name (autotools style), a
# library addressed as @rpath/... through an absolute rpath (CMake style), plug-ins, helpers, a "system" library.
# Then it runs the real bundler with llvm-install-name-tool / llvm-otool in place of Apple's tools and checks:
#   * all non-system libraries end up in Frameworks exactly once, every reference is @rpath/<name>,
#   * every binary carries exactly one rpath that resolves inside the bundle,
#   * verify_bundle.py accepts the result,
#   * the bundler REFUSES a library from outside the private prefix (a stand-in for a Homebrew leak),
#   * verify_bundle.py REJECTS a stray absolute reference, a too-new minimum macOS, and an absolute rpath.
# Needs: clang, ld64.lld, llvm-otool, llvm-install-name-tool, llvm-lipo (apt: clang lld llvm).
set -euo pipefail
# shellcheck source=lib/common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/common.sh"

for d in /usr/lib/llvm-*/bin; do
    [ -x "$d/ld64.lld" ] && PATH="$d:$PATH"
done
for t in clang ld64.lld llvm-otool llvm-install-name-tool llvm-lipo; do
    have "$t" || die "$t not found (apt-get install clang lld llvm)"
done
export OTOOL=llvm-otool LIPO=llvm-lipo INSTALL_NAME_TOOL=llvm-install-name-tool

W="$(mktemp -d "${TMPDIR:-/tmp}/bundler-test.XXXXXX")"
if [ -n "${KEEP:-}" ]; then echo "keeping $W"; else trap 'rm -rf "$W"' EXIT; fi
cd "$W"
fails=0
check() { # description, command...
    local what=$1
    shift
    if "$@" >/dev/null 2>&1; then note "ok    $what"; else
        note "FAIL  $what"
        fails=$((fails + 1))
    fi
}

# ---- fabricate Mach-O files --------------------------------------------------------------------------------------
cat >leaf.c <<'C'
int leaf(void) { return 1; }
C
cat >mid.c <<'C'
extern int leaf(void);
int mid(void) { return leaf() + 1; }
C
cat >core.c <<'C'
extern int mid(void);
int core(void) { return mid() + 1; }
C
cat >plug.c <<'C'
extern int core(void);
extern int leaf(void);
int plug(void) { return core() + leaf(); }
C
cat >prog.c <<'C'
extern int leaf(void);
int main(void) { return leaf() - 1; }
C
cat >prog2.c <<'C'
extern int core(void);
extern int leaf(void);
int main(void) { return core() + leaf() - 4; }
C
cat >sys.c <<'C'
int sysfn(void) { return 0; }
void stub_binder(void) __asm__("dyld_stub_binder");
void stub_binder(void) {}
C
CC=(clang --target=x86_64-apple-macos12 -ffreestanding -nostdinc -c)
LD=(ld64.lld -arch x86_64 -headerpad_max_install_names)
PLAT=(-platform_version macos 12.0 12.0)
for f in leaf mid core plug prog prog2 sys; do "${CC[@]}" "$f.c" -o "$f.o"; done

mkdir -p prefix/lib build/core homebrew/lib fake
# a stand-in for libSystem so executables can name /usr/lib/libSystem.B.dylib
"${LD[@]}" "${PLAT[@]}" -dylib -install_name /usr/lib/libSystem.B.dylib -o fake/libSystem.B.dylib sys.o
# libleaf: absolute install name (autotools style)
"${LD[@]}" "${PLAT[@]}" -dylib -install_name "$W/prefix/lib/libleaf.dylib" -o prefix/lib/libleaf.dylib leaf.o fake/libSystem.B.dylib
# libmid: @rpath id, depends on libleaf by absolute path
"${LD[@]}" "${PLAT[@]}" -dylib -install_name @rpath/libmid.dylib -o prefix/lib/libmid.dylib mid.o prefix/lib/libleaf.dylib fake/libSystem.B.dylib
# libcore (stand-in for libsdrpp_core): @rpath id, depends on @rpath/libmid.dylib found through rpath $W/prefix/lib
"${LD[@]}" "${PLAT[@]}" -dylib -install_name @rpath/libsdrpp_core.dylib -rpath "$W/prefix/lib" -o build/core/libsdrpp_core.dylib core.o prefix/lib/libmid.dylib fake/libSystem.B.dylib
# main program
"${LD[@]}" "${PLAT[@]}" -bind_at_load -e _main -rpath "$W/build/core" -o build/sdrpp prog2.o build/core/libsdrpp_core.dylib prefix/lib/libleaf.dylib fake/libSystem.B.dylib

plugins_source=(rtl_sdr_source rtl_tcp_source file_source network_source spyserver_source sdrpp_server_source)
plugins_sink=(audio_sink new_portaudio_sink network_sink)
plugins_decoder=(radio pager_decoder meteor_demodulator atv_decoder)
plugins_misc=(recorder frequency_manager scanner rigctl_server rigctl_client iq_exporter)
mkplugin() { # group name
    mkdir -p "build/$1/$2"
    "${LD[@]}" "${PLAT[@]}" -dylib -install_name "@rpath/$2.dylib" -rpath "$W/build/core" -o "build/$1/$2/$2.dylib" plug.o build/core/libsdrpp_core.dylib prefix/lib/libleaf.dylib fake/libSystem.B.dylib
}
for n in "${plugins_source[@]}"; do mkplugin source_modules "$n"; done
for n in "${plugins_sink[@]}"; do mkplugin sink_modules "$n"; done
for n in "${plugins_decoder[@]}"; do mkplugin decoder_modules "$n"; done
for n in "${plugins_misc[@]}"; do mkplugin misc_modules "$n"; done

mkdir -p helpers sdrpp-src/root/res/{icons,themes,fonts,bandplans}
for n in dsd-neo multimon-ng rtl_433; do
    "${LD[@]}" "${PLAT[@]}" -bind_at_load -e _main -rpath "$W/prefix/lib" -o "helpers/$n" prog.o prefix/lib/libleaf.dylib fake/libSystem.B.dylib
done
for d in icons themes fonts bandplans; do echo x >"sdrpp-src/root/res/$d/x.txt"; done
echo icns >fake.icns
# a "Homebrew" library outside the allowed roots, and a helper that links it
"${LD[@]}" "${PLAT[@]}" -dylib -install_name "$W/homebrew/lib/libbad.dylib" -o homebrew/lib/libbad.dylib leaf.o fake/libSystem.B.dylib
"${LD[@]}" "${PLAT[@]}" -bind_at_load -e _main -o helpers_bad prog.o homebrew/lib/libbad.dylib fake/libSystem.B.dylib

bundle() { # app helpers-dir
    python3 "$SCRIPTS/bundle_app.py" --app "$1" --build-dir build --sdrpp-src sdrpp-src --helpers-dir "$2" \
        --search-dir prefix/lib --allowed-root prefix --allowed-root build \
        --plist-template "$PACKAGING/Info.plist.in" --version 0.1.0 --build-number 7 --min-os 12.0 \
        --icon fake.icns --license-file "$ROOT/LICENSE" --license-file "$ROOT/THIRD_PARTY.md"
}
verify() { python3 "$SCRIPTS/verify_bundle.py" --app "$1" --arch x86_64 --min-os 12.0 --version 0.1.0 --no-codesign; }

log "bundling the synthetic build"
bundle out/SDRstar.app helpers >bundle.log 2>&1 || {
    cat bundle.log
    die "bundler failed on the synthetic build"
}
tail -n 4 bundle.log | sed 's/^/    /'
APP="$W/out/SDRstar.app"

log "checking the result"
check "verify_bundle accepts the bundle" verify "$APP"
check "Frameworks holds exactly leaf, mid, core (de-duplicated)" test "$(find "$APP/Contents/Frameworks" -name '*.dylib' | wc -l | tr -d ' ')" = 3
check "main program rpath is @loader_path/../Frameworks" bash -c "llvm-otool -l '$APP/Contents/MacOS/sdrstar' | grep -c 'path @loader_path/../Frameworks' | grep -qx 1"
check "main program has exactly one rpath" bash -c "test \$(llvm-otool -l '$APP/Contents/MacOS/sdrstar' | grep -c LC_RPATH) = 1"
check "helper rpath is @loader_path/../../Frameworks" bash -c "llvm-otool -l '$APP/Contents/Resources/helpers/dsd-neo' | grep -q 'path @loader_path/../../Frameworks'"
check "plugin rpath is @loader_path/../Frameworks" bash -c "llvm-otool -l '$APP/Contents/Plugins/radio.dylib' | grep -q 'path @loader_path/../Frameworks'"
check "framework rpath is @loader_path" bash -c "llvm-otool -l '$APP/Contents/Frameworks/libmid.dylib' | grep -q 'path @loader_path (offset'"
check "libleaf was renamed to @rpath id" bash -c "llvm-otool -D '$APP/Contents/Frameworks/libleaf.dylib' | grep -q '^@rpath/libleaf.dylib\$'"
check "no absolute build path survives anywhere" bash -c "! (for f in \$(find '$APP' -type f); do llvm-otool -l \$f 2>/dev/null | tail -n +2; done) | grep -q '$W'"
check "system library reference is left alone" bash -c "llvm-otool -L '$APP/Contents/MacOS/sdrstar' | grep -q '/usr/lib/libSystem.B.dylib'"
check "Info.plist written with the version" grep -q '<string>0.1.0</string>' "$APP/Contents/Info.plist"

log "negative tests"
if bundle out_bad/SDRstar.app helpers_bad_dir >bad.log 2>&1; then :; fi # helpers_bad_dir does not exist yet
mkdir -p helpers_bad_dir
cp helpers_bad helpers_bad_dir/dsd-neo
cp helpers_bad helpers_bad_dir/multimon-ng
cp helpers_bad helpers_bad_dir/rtl_433
check "bundler refuses a library from outside the private prefix" bash -c "! python3 '$SCRIPTS/bundle_app.py' --app out_bad/SDRstar.app --build-dir build --sdrpp-src sdrpp-src --helpers-dir helpers_bad_dir --search-dir prefix/lib --allowed-root prefix --allowed-root build --plist-template '$PACKAGING/Info.plist.in' > bad.log 2>&1 && grep -q 'outside the private dependency prefix' bad.log"

cp -R "$APP" app_stray.app
llvm-install-name-tool -change @rpath/libleaf.dylib /opt/homebrew/lib/libfoo.dylib app_stray.app/Contents/Plugins/radio.dylib 2>/dev/null ||
    llvm-install-name-tool -change @rpath/libsdrpp_core.dylib /opt/homebrew/lib/libfoo.dylib app_stray.app/Contents/Plugins/radio.dylib
check "verify rejects a reference to /opt/homebrew" bash -c "! python3 '$SCRIPTS/verify_bundle.py' --app app_stray.app --arch x86_64 --min-os 12.0 --no-codesign 2>&1 | tee stray.log | grep -q 'VERIFY OK' && grep -q 'opt/homebrew' stray.log"

cp -R "$APP" app_rpath.app
llvm-install-name-tool -add_rpath /usr/local/lib app_rpath.app/Contents/Plugins/scanner.dylib
check "verify rejects an absolute rpath" bash -c "! python3 '$SCRIPTS/verify_bundle.py' --app app_rpath.app --arch x86_64 --min-os 12.0 --no-codesign > rpath.log 2>&1 && grep -q 'absolute LC_RPATH' rpath.log"

cp -R "$APP" app_new.app
ld64.lld -arch x86_64 -platform_version macos 15.0 15.0 -dylib -install_name @rpath/libnew.dylib -o app_new.app/Contents/Frameworks/libnew.dylib leaf.o fake/libSystem.B.dylib
check "verify rejects a too-new minimum macOS" bash -c "! python3 '$SCRIPTS/verify_bundle.py' --app app_new.app --arch x86_64 --min-os 12.0 --no-codesign > new.log 2>&1 && grep -q 'newer than the deployment target' new.log"

check "verify rejects the wrong architecture" bash -c "! python3 '$SCRIPTS/verify_bundle.py' --app '$APP' --arch arm64 --min-os 12.0 --no-codesign > arch.log 2>&1 && grep -q 'expected only' arch.log"

if [ "$fails" -ne 0 ]; then die "$fails bundler logic check(s) failed"; fi
log "bundler / verifier logic: all checks passed"
