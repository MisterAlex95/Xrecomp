"""
`python -m tools.prepare GAME.z64`: a Nintendo 64 title pack (XBA-174; docs/android/prepare.md « N64 packs »,
consoles/n64/runtime/n64_recomp.h).

The same five steps and progress lines as a PS1 pack (tools/prepare/ps1.py, whose Run this is), for a cartridge:

  1 identify  the ROM's title key (game code + header CRC1, « NSME-635A2BFF », as recomp_pack_rom_identify, from any
              byte order: .z64 / .v64 / .n64) and its source identity, the SHA-256 of the ROM in the console's
              big-endian order (z64), hashed in one streaming pass (no copy: the lifter reads the file in any order)
  2 analyse   the code earlier runs had to interpret (<work>/discovered.txt, written by the runtime at exit:
              n64_recomp_write_missed, `ADDR HASH HEXBYTES` per line), merged without repeats (address + hash) into
              <work>/discovered-seeds.txt
  3 lift      `python -m n64_tools.recomp ROM <work>/gen [--discovered seeds]` (consoles/n64/tools)
  4 compile   every generated C file with -I consoles/n64/runtime, by the Xbox pack's compile driver (compile.py: the
              on-device clang multicall and sysroot with --android, the host's cc otherwise), object cache included
  5 pack      libtitle.so exporting n64_recomp_title_table, pack.json ("console": "n64", "title_key",
              "source_sha256", "entry": "n64_recomp_title_table", "discovered", "rom_order"), the same atomic
              publish under packs/n64-<KEY>/

The source identity is the whole ROM's (8 to 64 MB), normalised to big-endian, so a .z64, a .v64 and a .n64 dump of
one cartridge are one game with one pack; it is what n64_tools records in the table (n64_recomp_table.source_sha256),
so the runtime checks one value against both pack.json and the library. The runtime reads the whole ROM into memory
anyway; it hashes it only when a pack is there (n64_pack.h: about 0.2 s for 64 MB on a desktop core, measured).
"""

import errno
import hashlib
import os
import sys

from tools.prepare import compile as cc
from tools.prepare import gba
from tools.prepare import pack
from tools.prepare import ps1
from tools.prepare import toolkit
# The discovered file has the PS1's format (n64_recomp.h): the same reader, merge and writer.
from tools.prepare.ps1 import DISCOVERED, SEEDS, DISCOVERED_MAX, read_records, sorted_lines, write_records  # noqa: F401

CONSOLE = "n64"
ENTRY = "n64_recomp_title_table"           # n64_recomp.h N64_RECOMP_ENTRY_SYMBOL
ROM_MAX = 64 << 20                         # n64_core.h N64_ROM_MAX
ROM_MIN = 0x1000                           # the header and IPL3 (n64_tools.rom.Rom)
# The lifter; overridable for the tests (a fake generator), and its package's directory added to PYTHONPATH.
RECOMP_MODULE_ENV, TOOLS_DIR_ENV = "XBA_N64_RECOMP", "XBA_N64_TOOLS_DIR"
DEFAULT_MODULE = "n64_tools.recomp"
FIELDS = ("format", "console", "title_key", "title", "source_sha256", "abi", "shell_abi", "toolkit", "lib",
          "lib_sha256", "lib_size", "entry", "cc", "built_at")
MAGIC = {b"\x80\x37\x12\x40": "z64", b"\x37\x80\x40\x12": "v64", b"\x40\x12\x37\x80": "n64"}


def runtime_dir(root):
    return os.path.join(root, "consoles", "n64", "runtime")


def tools_dir(root):
    return os.path.join(root, "consoles", "n64", "tools")


# ── The ROM ─────────────────────────────────────────────────────

def rom_order(head):
    """« z64 », « v64 » or « n64 » from the ROM's first word, else None."""
    return MAGIC.get(bytes(head[:4]))


def is_n64(path):
    """A file whose first word is an N64 ROM's in one of the three byte orders (the extension does not matter)."""
    if not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as f:
            return rom_order(f.read(4)) is not None
    except OSError:
        return False


def normalize(data, order):
    """data (a multiple of 4 bytes but at the end) in the console's big-endian order, as n64_rom_normalize and
    n64_tools.rom.normalize: v64 swaps each 16-bit pair, n64 reverses each 32-bit word, a trailing part stays."""
    if order == "z64":
        return data
    out = bytearray(data)
    if order == "v64":
        n = len(data) & ~1
        out[0:n:2], out[1:n:2] = data[1:n:2], data[0:n:2]
    else:
        n = len(data) & ~3
        out[0:n:4], out[1:n:4], out[2:n:4], out[3:n:4] = data[3:n:4], data[2:n:4], data[1:n:4], data[0:n:4]
    return bytes(out)


def _code_char(b):
    return chr(b) if 0x41 <= b <= 0x5A or 0x30 <= b <= 0x39 else "_"


def title_key(header):
    """« NSME-635A2BFF »: the game code at 0x3B ([A-Z0-9], else '_') + CRC1 at 0x10, of a big-endian header, as
    recomp_pack.c n64_key, n64_runtime_fallback_key and n64_tools.rom.Rom.title_key."""
    return "".join(_code_char(b) for b in header[0x3B:0x3F]) + "-" + header[0x10:0x14].hex().upper()


def rom_title(header, fallback):
    """The header's 20-character name (printable ASCII), else fallback."""
    t = "".join(chr(b) for b in header[0x20:0x34] if 0x20 <= b <= 0x7E).strip()
    return t or fallback


def read_source(path):
    """(key, title, byte order, SHA-256 of the big-endian ROM, size) of an N64 ROM, read once in 1 MB pieces.
    ValueError with why when it is not one."""
    size = os.path.getsize(path)
    if size > ROM_MAX:
        raise ValueError("%d bytes: over 64 MB, not an N64 ROM" % size)
    if size < ROM_MIN:
        raise ValueError("%d bytes: shorter than an N64 header and IPL3 (4 KB)" % size)
    h = hashlib.sha256()
    header = order = None
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)          # a multiple of 4: only the last piece may have a trailing part
            if not chunk:
                break
            if order is None:
                order = rom_order(chunk)
                if order is None:
                    raise ValueError("not an N64 ROM (its first word is no byte order of one)")
            be = normalize(chunk, order)
            if header is None:
                header = be[:0x40]
            h.update(be)
    base = os.path.basename(path)
    return title_key(header), rom_title(header, os.path.splitext(base)[0] or base), order, h.hexdigest(), size


# ── pack.json ───────────────────────────────────────────────────

def make_meta(lib_path, key, title, source_sha256, order, abi, shell_abi, toolkit_commit, toolkit_sha256, cc_text,
              discovered, now=None):
    meta = ps1.make_meta(lib_path, key, title, source_sha256, None, abi, shell_abi, toolkit_commit, toolkit_sha256,
                         cc_text, discovered, now)
    del meta["boot"]
    meta["console"] = CONSOLE
    meta["entry"] = ENTRY
    meta["rom_order"] = order            # of the file prepared (the identity is the same for every order)
    return meta


def read(pack_dir):
    """An N64 pack.json as a dict, or None (no pack, not a whole one, not an N64 pack)."""
    import json
    try:
        with open(os.path.join(pack_dir, "pack.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS) or meta.get("console") != CONSOLE:
        return None
    return meta


def check(pack_dir, key, source_sha256, abi, shell_abi=pack.SHELL_ABI, verify_hash=True):
    """None when the N64 pack is loadable for this ROM (the runtime's checks, n64_pack.c), else why not."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    if meta["format"] != pack.FORMAT:
        return "format %r" % meta["format"]
    if meta["title_key"] != key:
        return "title_key %r" % meta["title_key"]
    if meta["source_sha256"] != source_sha256:
        return "built from another ROM"
    if meta["abi"] != abi:
        return "abi %r" % meta["abi"]
    if meta["shell_abi"] != shell_abi:
        return "shell_abi %r, this shell is %d" % (meta["shell_abi"], shell_abi)
    if meta["entry"] != ENTRY:
        return "entry %r" % meta["entry"]
    lib = os.path.join(pack_dir, meta["lib"])
    if "/" in meta["lib"] or not os.path.isfile(lib) or os.path.getsize(lib) != meta["lib_size"]:
        return "%s missing or of the wrong size" % meta["lib"]
    if verify_hash and pack.sha256_file(lib) != meta["lib_sha256"]:
        return "%s sha256 mismatch" % meta["lib"]
    return None


# Lifted N64 code (n64_tools README « Lifting »): -O1 -fno-tree-slp-vectorize like GBA and PS1 code (-O2 / -O3 compile
# at -O1). Generated files hold 1000..4000 instructions, so one clang job peaks about as a PS1 one.
JOB_RSS = 256 << 20
RESERVED = 1 << 30


def lifted_opt(opt):
    """The -O level lifted N64 code compiles at for a requested --opt."""
    return gba.gba_opt(opt)


def compile_flags(opt, root, gen, sysroot=None, include_dirs=()):
    """The generated C's flags: -I the N64 runtime (n64_recomp.h, n64_recomp_rt.h, n64_core.h) and the generated
    directory; with a sysroot, the Android target (compile.py's)."""
    flags = []
    if sysroot:
        flags += ["--target=" + cc.TARGET, "--sysroot=" + sysroot]
        rd = cc.sysroot_resource_dir(sysroot)
        if rd:
            flags.append("-resource-dir=" + rd)
    flags += gba.lifted_cflags(opt) + ["-g0", "-fPIC", "-std=gnu11", "-w", "-DNDEBUG"]
    for d in [gen, runtime_dir(root)] + list(include_dirs):
        flags.append("-I" + d)
    return flags


# ── The run: the PS1's (ps1.Run), with a cartridge in place of a disc ──

class Run(ps1.Run):
    cm = sys.modules[__name__]

    # ── 1 identify ──

    def identify(self):
        self.rep.progress(1, 0, "reading the ROM")
        try:
            self.key, self.title, order, self.rom_sha, size = read_source(self.source)
        except (FileNotFoundError, PermissionError) as e:
            raise cc.StepError("NO_ROM", "%s: %s" % (self.source, e.strerror or e))
        except (OSError, ValueError) as e:
            raise cc.StepError("BAD_ROM", "%s: %s" % (self.source, getattr(e, "strerror", None) or e))
        self.boot = order                    # pack.json rom_order (make_meta's slot of the PS1's boot EXE)
        self.name = "%s-%s" % (CONSOLE, self.key)
        a = self.a
        self.work = os.path.abspath(a.work or os.path.join(gba.data_home(), "work", self.name))
        self.packs = os.path.abspath(a.pack_dir or os.path.join(gba.data_home(), "packs"))
        try:
            os.makedirs(self.work, exist_ok=True)
        except OSError as e:
            raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (self.work, e))
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.rom = self.source               # the lifter reads any byte order; no copy of up to 64 MB
        self.toolkit = self.m.toolkit_version()
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("source") != self.rom_sha or self.stamp.get("toolkit_sha256") != self.toolkit_sha:
            self.stamp = {}
            self.save_stamp(console=CONSOLE, source=self.rom_sha, toolkit=self.toolkit,
                            toolkit_sha256=self.toolkit_sha)
        self.steps.skipped = [k for k in ("lift", "link") if self.stamp.get(k)]
        self.rep.progress(1, 100, "%s (N64, key %s, %s, %d KB)" % (self.title, self.key, order, size // 1024))


def run(a, host, android):
    """gba.run with this module's Run."""
    return gba.run(a, host, android, run_class=Run)
