#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Small Mach-O inspection / editing toolkit used by bundle_app.py and verify_bundle.py.

Everything is built on `otool -l`, `lipo -archs` and `install_name_tool`.  The tool names can be overridden with the
environment variables OTOOL / LIPO / INSTALL_NAME_TOOL, which is how scripts/test_bundler_logic.sh exercises the
bundler on Linux against synthetic Mach-O files using the llvm-* equivalents.
"""

import os
import re
import subprocess
from dataclasses import dataclass, field
from typing import List, Optional

OTOOL = os.environ.get("OTOOL", "otool")
LIPO = os.environ.get("LIPO", "lipo")
INSTALL_NAME_TOOL = os.environ.get("INSTALL_NAME_TOOL", "install_name_tool")

# Load commands that name another dylib this image depends on.
DEP_COMMANDS = {
    "LC_LOAD_DYLIB",
    "LC_LOAD_WEAK_DYLIB",
    "LC_REEXPORT_DYLIB",
    "LC_LAZY_LOAD_DYLIB",
    "LC_LOAD_UPWARD_DYLIB",
}

_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xfe\xed\xfa\xcf",
    b"\xce\xfa\xed\xfe",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
}


class ToolError(RuntimeError):
    """A command line tool failed."""


def run(cmd: List[str], check: bool = True) -> str:
    """Run a command and return its stdout (stderr is folded in only on failure)."""
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if check and proc.returncode != 0:
        raise ToolError("command failed (%d): %s\n%s" % (proc.returncode, " ".join(cmd), proc.stderr.strip()))
    return proc.stdout


def is_macho(path: str) -> bool:
    """True for a regular (non-symlink) file whose header is a Mach-O or universal binary magic."""
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as fh:
            head = fh.read(8)
    except OSError:
        return False
    if len(head) < 8 or head[:4] not in _MAGICS:
        return False
    if head[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        # Java class files share the 0xCAFEBABE magic; a fat header has a small architecture count.
        count = int.from_bytes(head[4:8], "big" if head[:4] == b"\xca\xfe\xba\xbe" else "little")
        return 0 < count < 16
    return True


@dataclass
class MachOInfo:
    path: str
    install_id: Optional[str] = None
    deps: List[str] = field(default_factory=list)
    rpaths: List[str] = field(default_factory=list)
    minos: Optional[str] = None
    sections: int = 0  # number of Mach headers seen (2+ means a universal binary)


_RE_CMD = re.compile(r"^\s*cmd (\S+)")
_RE_NAME = re.compile(r"^\s*name (.+?) \(offset \d+\)")
_RE_PATH = re.compile(r"^\s*path (.+?) \(offset \d+\)")
_RE_MINOS = re.compile(r"^\s*minos (\S+)")
_RE_VERSION = re.compile(r"^\s*version (\S+)")


def inspect(path: str) -> MachOInfo:
    """Parse `otool -l` for one file."""
    info = MachOInfo(path=path)
    out = run([OTOOL, "-l", path])
    cmd = None
    for line in out.splitlines():
        if line.startswith("Load command"):
            cmd = None
            continue
        m = _RE_CMD.match(line)
        if m:
            cmd = m.group(1)
            if cmd == "LC_SEGMENT" or cmd == "LC_SEGMENT_64":
                pass
            continue
        if cmd is None:
            continue
        if cmd in DEP_COMMANDS:
            m = _RE_NAME.match(line)
            if m:
                info.deps.append(m.group(1))
                cmd = None
        elif cmd == "LC_ID_DYLIB":
            m = _RE_NAME.match(line)
            if m:
                info.install_id = m.group(1)
                cmd = None
        elif cmd == "LC_RPATH":
            m = _RE_PATH.match(line)
            if m:
                info.rpaths.append(m.group(1))
                cmd = None
        elif cmd == "LC_BUILD_VERSION":
            m = _RE_MINOS.match(line)
            if m:
                info.minos = m.group(1)
                cmd = None
        elif cmd == "LC_VERSION_MIN_MACOSX":
            m = _RE_VERSION.match(line)
            if m:
                info.minos = m.group(1)
                cmd = None
    info.sections = len(re.findall(r"^Mach header", out, flags=re.MULTILINE))
    return info


def archs(path: str) -> List[str]:
    """Architectures contained in the file (`lipo -archs`)."""
    return run([LIPO, "-archs", path]).split()


def is_system_path(name: str) -> bool:
    """Libraries Apple guarantees on every Mac."""
    return name.startswith("/usr/lib/") or name.startswith("/System/Library/")


def version_tuple(text: str):
    parts = []
    for piece in text.split("."):
        digits = re.match(r"\d+", piece)
        parts.append(int(digits.group(0)) if digits else 0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts)


def expand_token(name: str, loader_dir: str, exe_dir: str) -> str:
    """Expand @loader_path / @executable_path in a path (but not @rpath)."""
    if name.startswith("@loader_path"):
        return os.path.normpath(loader_dir + name[len("@loader_path"):])
    if name.startswith("@executable_path"):
        return os.path.normpath(exe_dir + name[len("@executable_path"):])
    return name


# --- editing ---------------------------------------------------------------------------------------------------
def change_dep(path: str, old: str, new: str) -> None:
    run([INSTALL_NAME_TOOL, "-change", old, new, path])


def set_id(path: str, new_id: str) -> None:
    run([INSTALL_NAME_TOOL, "-id", new_id, path])


def add_rpath(path: str, rpath: str) -> None:
    run([INSTALL_NAME_TOOL, "-add_rpath", rpath, path])


def delete_rpath(path: str, rpath: str) -> None:
    run([INSTALL_NAME_TOOL, "-delete_rpath", rpath, path])
