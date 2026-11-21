"""
`python -m tools.prepare GAME.cue`: a PlayStation title pack (XBA-168; docs/android/prepare.md « PS1 packs »,
consoles/ps1/runtime/ps1_recomp.h).

The same five steps and progress lines as a GBA pack (tools/prepare/gba.py), for a disc image or a PS-X EXE:

  1 identify  the disc's title key (the serial of SYSTEM.CNF's BOOT line, as recomp_pack_rom_identify; else
              PSX-<CRC32 of the volume descriptor>; an EXE: EXE-<CRC32>), the boot EXE read from ISO 9660 (the
              ps1_tools reader), its SHA-256 (the source identity) and a copy in <work>/boot.exe, which the later
              steps read: the image is opened once, whatever its size
  2 analyse   the code earlier runs had to interpret (<work>/discovered.txt, written by the runtime at exit:
              ps1_recomp_write_missed, `ADDR HASH HEXBYTES` per line), merged without repeats (address + hash) into
              <work>/discovered-seeds.txt
  3 lift      `python -m ps1_tools.recomp <work>/boot.exe <work>/gen [--discovered seeds]` (consoles/ps1/tools)
  4 compile   every generated C file with -I consoles/ps1/runtime, by the Xbox pack's compile driver (compile.py: the
              on-device clang multicall and sysroot with --android, the host's cc otherwise), object cache included
  5 pack      libtitle.so exporting ps1_recomp_title_table, pack.json ("console": "ps1", "title_key",
              "source_sha256", "entry": "ps1_recomp_title_table", "discovered"), the same atomic publish under
              packs/ps1-<KEY>/

The source identity is the SHA-256 of the boot EXE, not of the image: reading a 700 MB image on a phone takes tens of
seconds at every start, the boot EXE is 2 MB at most, and it is what the table itself records (ps1_recomp_table
.source_sha256), so the runtime checks one value against both pack.json and the library. The same disc dumped as
.cue/.bin, .iso or .chd has the same identity; a disc whose EXE differs (another release, a patched copy) does not.
Overlays and other files are not part of it: the table's entries are content-checked in RAM (ps1_recomp.h), so code
the disc changed never runs from the pack.

A .chd is read by ps1_tools' own CHD v5 reader (consoles/ps1/tools/ps1_tools/chd.py, pure Python: cdzl / cdlz
through zlib / lzma, cdzs through Python 3.14's compression.zstd; cdfl, FLAC, is chdman's codec for audio hunks and
never needed). Only the hunks that hold ISO 9660, SYSTEM.CNF and the boot EXE are decompressed. A hunk it cannot
decode (a data hunk in FLAC, zstd on an older Python, a diff CHD's parent) gives BAD_DISC with why. The Android app
hands a .chd over as the sparse disc.iso its native reader extracted (PrepareService.ps1Source).
"""

import errno
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zlib

from tools.prepare import compile as cc
from tools.prepare import gba
from tools.prepare import metrics
from tools.prepare import pack
from tools.prepare import toolkit

CONSOLE = "ps1"
ENTRY = "ps1_recomp_title_table"           # ps1_recomp.h PS1_RECOMP_ENTRY_SYMBOL
EXE_MAX = (2 << 20) + 0x800                # ps1_runtime.c PS1_EXE_MAX: the header + at most the whole RAM
EXE_COPY = "boot.exe"
DISCOVERED = "discovered.txt"              # the runtime's (ps1_runtime_opts.discovered_path)
SEEDS = "discovered-seeds.txt"             # what this Prepare handed to the lifter
DISCOVERED_MAX = 65536                     # records kept (ps1_pack.h PS1_PACK_DISCOVERED_MAX)
DISC_EXT = (".cue", ".bin", ".iso", ".img", ".chd")
# The lifter; overridable for the tests (a fake generator), and its package's directory added to PYTHONPATH.
RECOMP_MODULE_ENV, TOOLS_DIR_ENV = "XBA_PS1_RECOMP", "XBA_PS1_TOOLS_DIR"
DEFAULT_MODULE = "ps1_tools.recomp"
FIELDS = ("format", "console", "title_key", "title", "source_sha256", "abi", "shell_abi", "toolkit", "lib",
          "lib_sha256", "lib_size", "entry", "cc", "built_at")


def runtime_dir(root):
    return os.path.join(root, "consoles", "ps1", "runtime")


def tools_dir(root):
    return os.path.join(root, "consoles", "ps1", "tools")


def _ps1_tools_exe():
    """consoles/ps1/tools/ps1_tools/exe.py: the lifter's own disc reader, so the EXE hashed here is the one lifted."""
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    d = tools_dir(root)
    if d not in sys.path:
        sys.path.insert(0, d)
    from ps1_tools import exe
    return exe


# ── The disc ────────────────────────────────────────────────────

def is_exe(head):
    return head[:8] == b"PS-X EXE"


def is_ps1(path):
    """A PS-X EXE, a .cue, a .chd (its magic), or a .bin / .iso / .img whose ISO 9660 volume says PLAYSTATION."""
    if not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as f:
            if is_exe(f.read(8)):
                return True
    except OSError:
        return False
    low = path.lower()
    if low.endswith((".cue", ".chd")):
        return True
    if not low.endswith((".bin", ".iso", ".img")):
        return False
    try:
        d = _ps1_tools_exe().Disc(path)
        try:
            return d.pvd()[8:19] == b"PLAYSTATION"
        finally:
            d.close()
    except (OSError, ValueError):
        return False


SERIAL = re.compile(r"([A-Z]{4})[_-](\d{3})\.(\d{2})\Z")


def serial(cnf):
    """« BOOT = cdrom:\\SLUS_005.94;1 » -> « SLUS-00594 », as recomp_pack.c ps1_serial and Console.ps1Serial; None
    when the boot file is not serial-shaped."""
    m = re.search(r"BOOT[ \t]*=[ \t]*([^\r\n;]*)", cnf)
    if not m:
        return None
    v = m.group(1)
    name = re.split(r"[\\:/]", v)[-1].strip(" \t").upper()
    s = SERIAL.match(name)
    return "%s-%s%s" % s.groups() if s else None


def disc_key(pvd, cnf):
    """The title key of a disc: the serial, else « PSX-<CRC32 of the 2048 bytes of the volume descriptor> »."""
    k = serial(cnf) if cnf else None
    return k or "PSX-%08X" % (zlib.crc32(pvd) & 0xFFFFFFFF)


def exe_key(data):
    """An EXE's key, as ps1_runtime_fallback_key: « EXE-<CRC32 of the file> »."""
    return "EXE-%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def read_source(path):
    """(key, boot name, EXE bytes) of a disc image or a PS-X EXE. ValueError with why when it is not one."""
    with open(path, "rb") as f:
        head = f.read(8)
        if is_exe(head):
            f.seek(0)
            data = f.read(EXE_MAX + 1)
            if len(data) > EXE_MAX:
                raise ValueError("a PS-X EXE of over 2 MB")
            _ps1_tools_exe().Exe(data)      # the header and its text in main RAM
            return exe_key(data), os.path.basename(path), data
    exe = _ps1_tools_exe()
    d = exe.Disc(path)
    try:
        pvd = d.pvd()
        cnf = d.read_file("SYSTEM.CNF").decode("latin-1") if d.find("SYSTEM.CNF") else None
        boot = d.boot_path()
        data = d.read_file(boot)
    finally:
        d.close()
    if len(data) > EXE_MAX:
        raise ValueError("the boot EXE %s is over 2 MB" % boot)
    exe.Exe(data)
    return disc_key(pvd, cnf), boot.split(";")[0].strip("\\/"), data


# ── Discovered code ─────────────────────────────────────────────

def read_records(path):
    """The records of a discovered file (ps1_recomp.h: `ADDR [HASH [HEXBYTES]]` per line, `#` comments) as a
    dict {(addr, hash or None): line}, the first of each (address, hash) kept; {} when there is no file."""
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.split("#", 1)[0].split()
                if not s:
                    continue
                try:
                    addr = int(s[0], 16)
                    h = int(s[1], 16) if len(s) >= 2 else None
                    if len(s) >= 3:
                        bytes.fromhex(s[2])
                except ValueError:
                    continue
                if not 0 <= addr <= 0xFFFFFFFF or addr & 3 or (h is not None and not 0 <= h < 1 << 64):
                    continue
                key = (addr, h)
                if key not in out and len(out) < DISCOVERED_MAX:
                    out[key] = "%08X" % addr + ("" if h is None else " %016X" % h) + \
                               (" " + s[2].upper() if len(s) >= 3 else "")
    except OSError:
        pass
    return out


def sorted_lines(records):
    return [records[k] for k in sorted(records, key=lambda k: (k[0], -1 if k[1] is None else k[1]))]


def write_records(path, lines):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        for line in lines:
            f.write(line + "\n")
    os.replace(tmp, path)


# ── pack.json ───────────────────────────────────────────────────

def make_meta(lib_path, key, title, source_sha256, boot, abi, shell_abi, toolkit_commit, toolkit_sha256, cc_text,
              discovered, now=None):
    import datetime
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return {
        "format": pack.FORMAT,
        "console": CONSOLE,
        "title_key": key,
        "title": title,
        "source_sha256": source_sha256,
        "boot": boot,
        "abi": abi,
        "shell_abi": int(shell_abi),
        "toolkit": toolkit_commit,
        "toolkit_sha256": toolkit_sha256,
        "lib": pack.LIB,
        "lib_sha256": pack.sha256_file(lib_path),
        "lib_size": os.path.getsize(lib_path),
        "entry": ENTRY,
        "cc": cc_text,
        "built_at": now.replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "discovered": int(discovered),
    }


def read(pack_dir):
    """A PS1 pack.json as a dict, or None (no pack, not a whole one, not a PS1 pack)."""
    try:
        with open(os.path.join(pack_dir, "pack.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS) or meta.get("console") != CONSOLE:
        return None
    return meta


def check(pack_dir, key, source_sha256, abi, shell_abi=pack.SHELL_ABI, verify_hash=True):
    """None when the PS1 pack is loadable for this disc (the runtime's checks, ps1_pack.c), else why not."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    if meta["format"] != pack.FORMAT:
        return "format %r" % meta["format"]
    if meta["title_key"] != key:
        return "title_key %r" % meta["title_key"]
    if meta["source_sha256"] != source_sha256:
        return "built from another boot EXE"
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


# Lifted PS1 code (XBA-161, docs/android/prepare.md « PS1 packs »): -O2 runs it no faster than -O1 and costs more
# compile time, so -O2 / -O3 compile at -O1 (as a GBA ROM: gba.gba_opt). One clang job on a generated file peaks at
# ~PS1_JOB_RSS: with a 1 GB reserve a phone with little free memory still gets its cores.
PS1_JOB_RSS = 256 << 20
PS1_RESERVED = 1 << 30


def ps1_opt(opt):
    """The -O level lifted PS1 code compiles at for a requested --opt."""
    return gba.gba_opt(opt)


lifted_opt, JOB_RSS, RESERVED = ps1_opt, PS1_JOB_RSS, PS1_RESERVED   # what Run reads (Run.cm)


def lifted_cflags(opt):
    """The optimisation flags of lifted PS1 code (also tools/consoles/ps1_recomp_build.py's and the compile-time
    benchmark's): no SLP vectorizer, as for GBA code (nothing to vectorize; a costly pass on big functions)."""
    return gba.lifted_cflags(opt)


def compile_flags(opt, root, gen, sysroot=None, include_dirs=()):
    """The generated C's flags: -I the PS1 runtime (ps1_recomp.h, ps1_recomp_rt.h, ps1_core.h) and the generated
    directory; with a sysroot, the Android target (compile.py's)."""
    flags = []
    if sysroot:
        flags += ["--target=" + cc.TARGET, "--sysroot=" + sysroot]
        rd = cc.sysroot_resource_dir(sysroot)
        if rd:
            flags.append("-resource-dir=" + rd)
    flags += lifted_cflags(opt) + ["-g0", "-fPIC", "-std=gnu11", "-w", "-DNDEBUG"]
    for d in [gen, runtime_dir(root)] + list(include_dirs):
        flags.append("-I" + d)
    return flags


# ── The run: the GBA's (gba.Run), with a disc in place of a ROM ──

class Run(gba.Run):
    # The console module the steps after identify take their lifter, flags, checks and pack.json from: this one, or
    # tools/prepare/n64.py's (an N64 Run is this one with a cartridge in place of a disc).
    cm = sys.modules[__name__]

    # ── 1 identify ──

    def identify(self):
        self.rep.progress(1, 0, "reading the disc")
        try:
            self.key, self.boot, exe = read_source(self.source)
        except FileNotFoundError as e:
            raise cc.StepError("NO_DISC", "%s: %s" % (self.source, e.strerror or e))
        except PermissionError as e:
            raise cc.StepError("NO_DISC", "%s: %s" % (self.source, e.strerror or e))
        except (OSError, ValueError) as e:
            raise cc.StepError("BAD_DISC", "%s: %s" % (self.source, getattr(e, "strerror", None) or e))
        self.name = "%s-%s" % (CONSOLE, self.key)
        self.rom_sha = hashlib.sha256(exe).hexdigest()       # the source identity (the boot EXE's)
        base = os.path.basename(self.source)
        self.title = os.path.splitext(base)[0] or base
        a = self.a
        self.work = os.path.abspath(a.work or os.path.join(gba.data_home(), "work", self.name))
        self.packs = os.path.abspath(a.pack_dir or os.path.join(gba.data_home(), "packs"))
        try:
            os.makedirs(self.work, exist_ok=True)
        except OSError as e:
            raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (self.work, e))
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.rom = os.path.join(self.work, EXE_COPY)
        have = pack.sha256_file(self.rom) if os.path.isfile(self.rom) else None
        if have != self.rom_sha:
            with open(self.rom + ".tmp", "wb") as f:
                f.write(exe)
            os.replace(self.rom + ".tmp", self.rom)
        self.toolkit = self.m.toolkit_version()
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("source") != self.rom_sha or self.stamp.get("toolkit_sha256") != self.toolkit_sha:
            self.stamp = {}
            self.save_stamp(console=CONSOLE, source=self.rom_sha, toolkit=self.toolkit,
                            toolkit_sha256=self.toolkit_sha)
        self.steps.skipped = [k for k in ("lift", "link") if self.stamp.get(k)]
        self.rep.progress(1, 100, "%s (PS1, key %s, boot %s, %d KB)" % (self.title, self.key, self.boot,
                                                                         len(exe) // 1024))

    # ── 2 analyse: the code earlier runs had to interpret ──

    def analyse(self):
        found = read_records(os.path.join(self.work, DISCOVERED))
        extra = getattr(self.a, "discovered", None)
        if extra:
            for k, v in read_records(extra).items():
                if k not in found and len(found) < DISCOVERED_MAX:
                    found[k] = v
        lines = sorted_lines(found)
        seeds = os.path.join(self.work, SEEDS)
        if lines:
            write_records(seeds, lines)
        elif os.path.exists(seeds):
            os.remove(seeds)
        self.seeds = seeds if lines else None
        self.discovered = len(lines)
        self.disc_key = hashlib.sha256("".join(l + "\n" for l in lines).encode()).hexdigest()
        self.rep.progress(2, 100, "%d piece(s) of code found by earlier runs" % len(lines) if lines
                          else "no code from earlier runs")

    # ── 3 lift ──

    def lift(self):
        gen = os.path.join(self.work, "gen")
        cm = self.cm                        # this module, or a Run built on this one (gc.py, x360.py, n64.py)
        module = os.environ.get(cm.RECOMP_MODULE_ENV, cm.DEFAULT_MODULE)
        key = "%s|%s" % (self.disc_key, module)
        if self.stamp.get("lift") == key and os.path.isdir(gen) and any(f.endswith(".c") for f in os.listdir(gen)):
            self.rep.progress(3, 100, "lift up to date (%d C files)" % len([f for f in os.listdir(gen)
                                                                            if f.endswith(".c")]))
            self.gen = gen
            return
        args = self.lift_args(module, gen)
        env = dict(os.environ)
        paths = [p for p in (os.environ.get(cm.TOOLS_DIR_ENV), cm.tools_dir(self.m.ROOT), self.m.ROOT) if p]
        if env.get("PYTHONPATH"):
            paths.append(env["PYTHONPATH"])
        env["PYTHONPATH"] = os.pathsep.join(paths)
        files = self.run_lifter(module, args, env, gen, key, self.lift_note(), self.lift_stdout_to_log)
        self.rep.progress(3, 100, "%d C files%s" % (len(files), self._lift_summary(gen)))

    lift_stdout_to_log = True

    def lift_args(self, module, out):
        args = [self.py, "-m", module, self.rom, out]
        if self.seeds:
            args += ["--discovered", self.seeds]
        return args

    def lift_note(self):
        return "lifting to C"

    def _lift_summary(self, gen):
        return ""

    # ── 4 compile: gba.Run's, with this console's flags ──

    @property
    def job_rss(self):
        return self.cm.JOB_RSS

    @property
    def reserved(self):
        return self.cm.RESERVED

    def lifted_opt(self):
        return self.cm.lifted_opt(self.a.opt)

    def flags_for(self, opt, sysroot):
        return self.cm.compile_flags(opt, self.m.ROOT, self.gen, sysroot, self.a.include_dir)

    # ── 5 pack ──

    def make_pack(self):
        a, cm = self.a, self.cm
        abi = pack.ABI if self.android else gba.host_abi()
        os.makedirs(self.packs, exist_ok=True)
        self.rep.progress(5, 0, "linking libtitle.so")
        h = hashlib.sha256()
        for o in self.objects:
            h.update(os.path.basename(o).encode() + b"\n")
        if self.android:
            h.update(pack.sha256_file(a.shell_lib).encode())
        h.update(("|%s|%s|%s|%s|%d|%s" % (a.shell_abi, self.toolkit_sha, abi, self.rom_sha, self.discovered,
                                         self.key)).encode())
        key = h.hexdigest()
        pack.recover(self.packs, self.name)
        final = pack.final_dir(self.packs, self.name)
        if self.stamp.get("link") == key and cm.check(final, self.key, self.rom_sha, abi, a.shell_abi) is None:
            self.rep.progress(5, 100, "pack up to date")
        else:
            try:
                part = pack.begin(self.packs, self.name)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            lib = os.path.join(part, pack.LIB)
            self.link(lib)
            self.rep.progress(5, 70, "writing pack.json")
            meta = cm.make_meta(lib, self.key, self.title, self.rom_sha, self.boot, abi, a.shell_abi, self.toolkit,
                             self.toolkit_sha, "%s -O%s" % (self.tc.short_version(), self.opt), self.discovered)
            meta["prepare"] = metrics.summary(self.steps, self.compile_stats, self.sources, self.objects, self.jobs,
                                              self.opt, part, getattr(a, "device_log", None), self.tools_peak_kb)
            try:
                pack.publish(self.packs, self.name, meta)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            self.save_stamp(link=key)
            self.rep.progress(5, 100, "pack %s, %d bytes" % (self.name, meta["lib_size"]))
        cc.prune(os.path.join(self.work, "obj"), self.objects)
        return final


def run(a, host, android):
    """gba.run with this module's Run."""
    return gba.run(a, host, android, run_class=Run)
