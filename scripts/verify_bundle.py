#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a finished SDRstar.app.  Exits non-zero (and says exactly why) on any problem.

Checks
  1. Dependency closure   every Mach-O references only /usr/lib, /System/Library, or @rpath/@loader_path/
                          @executable_path names that resolve to a file INSIDE the bundle.  Any reference to
                          /opt/homebrew, /usr/local, /Users/..., the build tree, ... is a failure.  Every LC_RPATH
                          must be relative and stay inside the bundle.
  2. Architecture/minos   `lipo -archs` is exactly the expected architecture and the minimum macOS each binary
                          declares (LC_BUILD_VERSION minos / LC_VERSION_MIN_MACOSX) is <= MACOS_DEPLOYMENT_TARGET.
  3. Bundle shape         Info.plist keys, executable, icon, helpers, resources, expected plugins.
  4. Code signature       (macOS only, unless --no-codesign) `codesign --verify --deep --strict`.
"""

import argparse
import os
import plistlib
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib"))
import macho  # noqa: E402

DEFAULT_PLUGINS = [
    "rtl_sdr_source", "rtl_tcp_source", "file_source", "network_source", "spyserver_source", "sdrpp_server_source",
    "audio_sink", "new_portaudio_sink", "network_sink",
    "radio", "pager_decoder", "meteor_demodulator", "atv_decoder",
    "recorder", "frequency_manager", "scanner", "rigctl_server", "rigctl_client", "iq_exporter",
]
HELPERS = ["dsd-neo", "multimon-ng", "rtl_433"]


class Report:
    def __init__(self):
        self.failures = []

    def fail(self, msg):
        self.failures.append(msg)
        print("  FAIL: " + msg, flush=True)


def within(path, root):
    path, root = os.path.realpath(path), os.path.realpath(root)
    return path == root or path.startswith(root + os.sep)


def all_macho(app):
    found = []
    for root, _dirs, files in os.walk(app):
        for fn in sorted(files):
            p = os.path.join(root, fn)
            if macho.is_macho(p):
                found.append(p)
    return found


def check_closure(app, files, rep):
    print("[1] dependency closure of %d Mach-O files" % len(files), flush=True)
    for f in files:
        rel = os.path.relpath(f, app)
        info = macho.inspect(f)
        loader_dir = os.path.dirname(f)
        exe_dir = os.path.join(app, "Contents", "MacOS")
        # rpaths: relative, inside the bundle
        for rp in info.rpaths:
            if not (rp.startswith("@loader_path") or rp.startswith("@executable_path")):
                rep.fail("%s: absolute LC_RPATH %s" % (rel, rp))
                continue
            target = macho.expand_token(rp, loader_dir, exe_dir)
            if not within(target, app):
                rep.fail("%s: LC_RPATH %s leaves the bundle" % (rel, rp))
        # install id
        if info.install_id is not None and not info.install_id.startswith("@rpath/"):
            rep.fail("%s: install name %s is not @rpath/..." % (rel, info.install_id))
        # dependencies
        for dep in info.deps:
            if macho.is_system_path(dep):
                continue
            if dep.startswith("@rpath/"):
                name = dep[len("@rpath/"):]
                ok = False
                for rp in info.rpaths:
                    cand = os.path.join(macho.expand_token(rp, loader_dir, exe_dir), name)
                    if os.path.isfile(cand) and within(cand, app):
                        ok = True
                        break
                if not ok:
                    rep.fail("%s: %s does not resolve inside the bundle via its rpaths %s" % (rel, dep, info.rpaths))
            elif dep.startswith("@loader_path") or dep.startswith("@executable_path"):
                cand = macho.expand_token(dep, loader_dir, exe_dir)
                if not (os.path.isfile(cand) and within(cand, app)):
                    rep.fail("%s: %s does not resolve inside the bundle" % (rel, dep))
            else:
                rep.fail("%s: links %s (not a system library and not bundle-relative)" % (rel, dep))


def check_arch_minos(app, files, arch, min_os, rep):
    print("[2] architecture == %s and minimum macOS <= %s" % (arch, min_os), flush=True)
    limit = macho.version_tuple(min_os)
    worst = (0, 0, 0)
    for f in files:
        rel = os.path.relpath(f, app)
        got = macho.archs(f)
        if got != [arch]:
            rep.fail("%s: architectures %s, expected only [%s]" % (rel, got, arch))
        info = macho.inspect(f)
        if info.minos is None:
            rep.fail("%s: no LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX (cannot tell the minimum macOS)" % rel)
            continue
        v = macho.version_tuple(info.minos)
        worst = max(worst, v)
        if v > limit:
            rep.fail("%s: requires macOS %s, newer than the deployment target %s" % (rel, info.minos, min_os))
    print("    highest minos found: %d.%d" % (worst[0], worst[1]), flush=True)


def check_shape(app, arch, version, min_os, plugins, rep):
    print("[3] bundle contents and Info.plist", flush=True)
    contents = os.path.join(app, "Contents")
    plist_path = os.path.join(contents, "Info.plist")
    if not os.path.isfile(plist_path):
        rep.fail("Info.plist missing")
        return
    with open(plist_path, "rb") as fh:
        pl = plistlib.load(fh)
    want = {
        "CFBundleIdentifier": "org.sdrstar.sdrstar",
        "CFBundleName": "SDRstar",
        "CFBundleDisplayName": "SDRstar",
        "CFBundleExecutable": "sdrstar",
        "CFBundleIconFile": "SDRstar",
        "CFBundlePackageType": "APPL",
        "LSMinimumSystemVersion": min_os,
        "LSApplicationCategoryType": "public.app-category.utilities",
        "NSHighResolutionCapable": True,
        "NSAppSleepDisabled": True,
    }
    if version:
        want["CFBundleShortVersionString"] = version
    for k, v in want.items():
        if pl.get(k) != v:
            rep.fail("Info.plist %s = %r, expected %r" % (k, pl.get(k), v))
    for k in pl:
        if re.match(r"NS(Microphone|Camera|Location|Contacts|Calendars|Photo|Bluetooth)", k):
            rep.fail("Info.plist must not declare %s (SDRstar never asks for that permission)" % k)

    exe = os.path.join(contents, "MacOS", "sdrstar")
    if not (os.path.isfile(exe) and os.access(exe, os.X_OK)):
        rep.fail("MacOS/sdrstar missing or not executable")
    if not os.path.isfile(os.path.join(contents, "Resources", "SDRstar.icns")):
        rep.fail("Resources/SDRstar.icns missing")
    for h in HELPERS:
        p = os.path.join(contents, "Resources", "helpers", h)
        if not (os.path.isfile(p) and os.access(p, os.X_OK)):
            rep.fail("helper Resources/helpers/%s missing or not executable" % h)
    for d in ("icons", "themes", "fonts", "bandplans"):
        if not os.path.isdir(os.path.join(contents, "Resources", d)):
            rep.fail("SDR++ resource directory Resources/%s missing" % d)
    for fn in ("LICENSE", "THIRD_PARTY.md"):
        if not os.path.isfile(os.path.join(contents, "Resources", "licenses", fn)):
            rep.fail("Resources/licenses/%s missing" % fn)
    for name in plugins:
        if not os.path.isfile(os.path.join(contents, "Plugins", name + ".dylib")):
            rep.fail("expected plugin Plugins/%s.dylib is missing" % name)
    if not os.path.isfile(os.path.join(contents, "Frameworks", "libsdrpp_core.dylib")):
        rep.fail("Frameworks/libsdrpp_core.dylib missing")
    n_fw = len([x for x in os.listdir(os.path.join(contents, "Frameworks")) if x.endswith(".dylib")])
    n_pl = len([x for x in os.listdir(os.path.join(contents, "Plugins")) if x.endswith(".dylib")])
    print("    %d libraries in Frameworks, %d plugins" % (n_fw, n_pl), flush=True)


def check_codesign(app, files, rep):
    print("[4] code signature", flush=True)
    cs = os.environ.get("CODESIGN", "codesign")
    for f in files:
        proc = subprocess.run([cs, "--verify", "--strict", f], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if proc.returncode != 0:
            rep.fail("codesign --verify %s: %s" % (os.path.relpath(f, app), proc.stdout.strip()))
    proc = subprocess.run([cs, "--verify", "--deep", "--strict", "--verbose=2", app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    print("    " + proc.stdout.strip().replace("\n", "\n    "), flush=True)
    if proc.returncode != 0:
        rep.fail("codesign --verify --deep --strict failed for the app")
    info = subprocess.run([cs, "-dv", app], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True).stdout
    if "Signature=adhoc" not in info:
        rep.fail("app is not ad-hoc signed: " + info.strip().replace("\n", " | "))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True)
    ap.add_argument("--arch", required=True, choices=["arm64", "x86_64"])
    ap.add_argument("--min-os", required=True)
    ap.add_argument("--version", default="")
    ap.add_argument("--expect-plugin", action="append", default=[], help="additional plugin that must be present")
    ap.add_argument("--no-codesign", action="store_true")
    args = ap.parse_args()

    app = os.path.abspath(args.app)
    if not os.path.isdir(app):
        print("ERROR: %s is not a directory" % app, file=sys.stderr)
        return 2
    rep = Report()
    files = all_macho(app)
    if not files:
        print("ERROR: no Mach-O files found in %s" % app, file=sys.stderr)
        return 2
    check_closure(app, files, rep)
    check_arch_minos(app, files, args.arch, args.min_os, rep)
    check_shape(app, args.arch, args.version, args.min_os, DEFAULT_PLUGINS + args.expect_plugin, rep)
    if not args.no_codesign:
        check_codesign(app, files, rep)
    if rep.failures:
        print("\nVERIFY FAILED: %d problem(s)" % len(rep.failures), file=sys.stderr)
        return 1
    print("\nVERIFY OK: %d Mach-O files, all references stay inside the bundle, arch %s, minos <= %s" % (len(files), args.arch, args.min_os))
    return 0


if __name__ == "__main__":
    sys.exit(main())
