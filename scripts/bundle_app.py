#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble SDRstar.app from the SDR++ build tree and the helper programs.

Layout produced (see docs/ARCHITECTURE.md):

    SDRstar.app/Contents/
        Info.plist
        MacOS/sdrstar                          SDR++ main program (renamed)
        Frameworks/*.dylib                     libsdrpp_core + every non-system library, de-duplicated
        Plugins/*.dylib                        SDR++ modules (+ sdrstar_smart when present)
        Resources/                             SDR++ resource files, SDRstar.icns, licenses/
        Resources/helpers/{dsd-neo,multimon-ng,rtl_433}

Every Mach-O is rewritten so that it references libraries only as @rpath/<name>, and carries exactly one LC_RPATH
that points (relative to itself) at Contents/Frameworks.  Libraries are only ever copied from the private
dependency prefix or the build tree; a dependency that resolves anywhere else (Homebrew, /usr/local ...) aborts the
build, because such a library would not honour the deployment target.

After all edits (install_name_tool invalidates signatures) every Mach-O is ad-hoc signed inner-first, then the app.
"""

import argparse
import filecmp
import os
import plistlib
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib"))
import macho  # noqa: E402


class BundleError(RuntimeError):
    pass


HELPERS = ("dsd-neo", "multimon-ng", "rtl_433")


class Bundler:
    def __init__(self, args):
        self.args = args
        self.app = os.path.abspath(args.app)
        self.contents = os.path.join(self.app, "Contents")
        self.macos = os.path.join(self.contents, "MacOS")
        self.frameworks = os.path.join(self.contents, "Frameworks")
        self.plugins = os.path.join(self.contents, "Plugins")
        self.resources = os.path.join(self.contents, "Resources")
        self.helpers = os.path.join(self.resources, "helpers")
        self.search_dirs = [os.path.realpath(d) for d in args.search_dir]
        self.allowed_roots = [os.path.realpath(d) for d in args.allowed_root]
        # name -> real source path of every library placed in Frameworks
        self.placed = {}
        # bundle path -> original (pre-copy) path, so @loader_path can be expanded as the linker saw it
        self.origin = {}
        self.exe_origin_dir = None

    # ------------------------------------------------------------------------------------------------------
    def log(self, msg):
        print("    " + msg, flush=True)

    def copy_file(self, src, dest):
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(src, dest)  # follows symlinks: we want the real bytes under the requested name
        os.chmod(dest, 0o755)

    # ------------------------------------------------------------------------------------------------------
    def layout(self):
        if os.path.exists(self.app):
            shutil.rmtree(self.app)
        for d in (self.macos, self.frameworks, self.plugins, self.resources, self.helpers):
            os.makedirs(d)

    def install_payload(self):
        a = self.args
        exe = os.path.join(a.build_dir, "sdrpp")
        core = os.path.join(a.build_dir, "core", "libsdrpp_core.dylib")
        for p in (exe, core):
            if not os.path.isfile(p):
                raise BundleError("expected build product is missing: %s" % p)
        dest_exe = os.path.join(self.macos, "sdrstar")
        self.copy_file(exe, dest_exe)
        self.origin[dest_exe] = os.path.realpath(exe)
        self.exe_origin_dir = os.path.dirname(os.path.realpath(exe))
        self.place_framework("libsdrpp_core.dylib", core)

        # SDR++ modules: <build>/{source,sink,decoder,misc}_modules/<module>/<module>.dylib
        n = 0
        for group in ("source_modules", "sink_modules", "decoder_modules", "misc_modules"):
            group_dir = os.path.join(a.build_dir, group)
            if not os.path.isdir(group_dir):
                continue
            for mod in sorted(os.listdir(group_dir)):
                mod_dir = os.path.join(group_dir, mod)
                if not os.path.isdir(mod_dir):
                    continue
                for fn in sorted(os.listdir(mod_dir)):
                    if fn.endswith(".dylib"):
                        src = os.path.join(mod_dir, fn)
                        dest = os.path.join(self.plugins, fn)
                        if os.path.exists(dest):
                            raise BundleError("two plugins are named %s" % fn)
                        self.copy_file(src, dest)
                        self.origin[dest] = os.path.realpath(src)
                        n += 1
        if n == 0:
            raise BundleError("no SDR++ plugins found under %s" % a.build_dir)
        self.log("%d plugins" % n)

        # helper decoders
        for name in HELPERS:
            src = os.path.join(a.helpers_dir, name)
            if not os.path.isfile(src):
                raise BundleError("helper program missing: %s (run scripts/build_helpers.sh)" % src)
            dest = os.path.join(self.helpers, name)
            self.copy_file(src, dest)
            self.origin[dest] = os.path.realpath(src)

        # SDR++ resource files (themes, fonts, band plans, icons)
        res = os.path.join(a.sdrpp_src, "root", "res")
        if not os.path.isdir(res):
            raise BundleError("SDR++ resources not found: %s" % res)
        for entry in os.listdir(res):
            s = os.path.join(res, entry)
            d = os.path.join(self.resources, entry)
            if os.path.isdir(s):
                shutil.copytree(s, d)
            else:
                shutil.copy2(s, d)

        if a.icon:
            shutil.copy2(a.icon, os.path.join(self.resources, "SDRstar.icns"))
        lic = os.path.join(self.resources, "licenses")
        os.makedirs(lic)
        for fn in a.license_file:
            if os.path.isfile(fn):
                shutil.copy2(fn, lic)
        if a.build_info and os.path.isfile(a.build_info):
            shutil.copy2(a.build_info, os.path.join(self.resources, "BUILD-INFO.txt"))

    def write_plist(self):
        a = self.args
        with open(a.plist_template, "r", encoding="utf-8") as fh:
            text = fh.read()
        text = (
            text.replace("@VERSION@", a.version)
            .replace("@BUILD@", a.build_number)
            .replace("@MIN_OS@", a.min_os)
        )
        plistlib.loads(text.encode("utf-8"))  # raises if the template is malformed
        with open(os.path.join(self.contents, "Info.plist"), "w", encoding="utf-8") as fh:
            fh.write(text)

    # ------------------------------------------------------------------------------------------------------
    # Library resolution and rewriting
    # ------------------------------------------------------------------------------------------------------
    def place_framework(self, name, real_src):
        dest = os.path.join(self.frameworks, name)
        if name in self.placed:
            # The same library may be reachable through two paths (e.g. SDR++ copies libsdrpp_core.dylib next to the
            # executable as well); only a genuine content difference is an error.
            if self.placed[name] != os.path.realpath(real_src) and not filecmp.cmp(self.placed[name], real_src, shallow=False):
                raise BundleError(
                    "two different libraries are both called %s:\n  %s\n  %s" % (name, self.placed[name], real_src)
                )
            return
        self.copy_file(real_src, dest)
        self.placed[name] = os.path.realpath(real_src)
        self.origin[dest] = os.path.realpath(real_src)

    def resolve(self, dep, rpaths, loader_dir, exe_dir, who):
        """Find the on-disk file a load command refers to, as the original (unbundled) binary would have."""
        cands = []
        if dep.startswith("@rpath/"):
            rest = dep[len("@rpath/"):]
            for rp in rpaths:
                cands.append(os.path.join(macho.expand_token(rp, loader_dir, exe_dir), rest))
            for d in self.search_dirs:
                cands.append(os.path.join(d, rest))
        elif dep.startswith("@loader_path") or dep.startswith("@executable_path"):
            cands.append(macho.expand_token(dep, loader_dir, exe_dir))
        else:
            cands.append(dep)
        for c in cands:
            if os.path.exists(c):
                real = os.path.realpath(c)
                if not any(real == r or real.startswith(r + os.sep) for r in self.allowed_roots):
                    raise BundleError(
                        "%s depends on %s, which resolves to %s -- outside the private dependency prefix.\n"
                        "      A library from Homebrew or /usr/local must never be bundled (it ignores the macOS "
                        "deployment target).\n      Build it from source in scripts/build_deps.sh instead."
                        % (who, dep, real)
                    )
                return real
        raise BundleError("%s depends on %s, which cannot be found. Looked in:\n      %s" % (who, dep, "\n      ".join(cands)))

    def relocate(self):
        """Copy every non-system dependency into Frameworks and rewrite all references."""
        pending = []
        for root, _dirs, files in os.walk(self.contents):
            for fn in sorted(files):
                p = os.path.join(root, fn)
                if macho.is_macho(p):
                    pending.append(p)
        done = set()
        while pending:
            f = pending.pop(0)
            if f in done:
                continue
            done.add(f)
            info = macho.inspect(f)
            origin = self.origin.get(f, f)
            loader_dir = os.path.dirname(origin)
            exe_dir = self.exe_origin_dir or loader_dir
            who = os.path.relpath(f, self.app)
            for dep in info.deps:
                if macho.is_system_path(dep):
                    continue
                name = os.path.basename(dep)
                real = self.resolve(dep, info.rpaths, loader_dir, exe_dir, who)
                is_new = name not in self.placed
                self.place_framework(name, real)
                if is_new:
                    pending.append(os.path.join(self.frameworks, name))
                if dep != "@rpath/" + name:
                    macho.change_dep(f, dep, "@rpath/" + name)
            # identity: libraries are addressed as @rpath/<name>
            if f.endswith(".dylib") and info.install_id is not None and info.install_id != "@rpath/" + os.path.basename(f):
                macho.set_id(f, "@rpath/" + os.path.basename(f))
            # exactly one rpath, relative to this file, reaching Contents/Frameworks
            for rp in info.rpaths:
                macho.delete_rpath(f, rp)
            rel = os.path.relpath(self.frameworks, os.path.dirname(f))
            macho.add_rpath(f, "@loader_path" if rel == "." else "@loader_path/" + rel)
        self.log("%d Mach-O files processed, %d libraries in Frameworks" % (len(done), len(self.placed)))

    # ------------------------------------------------------------------------------------------------------
    # Signing
    # ------------------------------------------------------------------------------------------------------
    def sign(self):
        codesign = os.environ.get("CODESIGN", "codesign")
        main_exe = os.path.join(self.macos, "sdrstar")
        files = []
        for root, _dirs, names in os.walk(self.contents):
            for fn in sorted(names):
                p = os.path.join(root, fn)
                if p != main_exe and macho.is_macho(p):
                    files.append(p)
        # inner-first: libraries before the programs that load them
        files.sort(key=lambda p: (0 if p.startswith(self.frameworks) else 1, p))
        for p in files:
            macho.run([codesign, "--force", "--sign", "-", "--timestamp=none", p])
        macho.run([codesign, "--force", "--sign", "-", "--timestamp=none", self.app])
        self.log("ad-hoc signed %d Mach-O files and the app" % len(files))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True, help="output SDRstar.app path")
    ap.add_argument("--build-dir", help="SDR++ CMake build directory")
    ap.add_argument("--sdrpp-src", help="prepared SDR++ source tree (for root/res)")
    ap.add_argument("--helpers-dir", help="directory holding dsd-neo, multimon-ng, rtl_433")
    ap.add_argument("--search-dir", action="append", default=[], help="extra directory to resolve @rpath/ names in")
    ap.add_argument("--allowed-root", action="append", default=[], help="libraries may only be copied from here")
    ap.add_argument("--plist-template")
    ap.add_argument("--version", default="0.0.0")
    ap.add_argument("--build-number", default="1")
    ap.add_argument("--min-os", default="12.0")
    ap.add_argument("--icon")
    ap.add_argument("--license-file", action="append", default=[])
    ap.add_argument("--build-info")
    ap.add_argument("--sign", action="store_true", help="ad-hoc sign after assembling")
    ap.add_argument("--sign-only", action="store_true", help="only (re-)sign an existing app")
    args = ap.parse_args()

    b = Bundler(args)
    try:
        if args.sign_only:
            b.sign()
            return 0
        for need in ("build_dir", "sdrpp_src", "helpers_dir", "plist_template"):
            if not getattr(args, need):
                ap.error("--%s is required" % need.replace("_", "-"))
        print("==> assembling %s" % b.app, flush=True)
        b.layout()
        b.install_payload()
        b.write_plist()
        b.relocate()
        if args.sign:
            b.sign()
    except (BundleError, macho.ToolError) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
