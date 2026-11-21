"""
`python -m tools.prepare DEFAULT.XEX` (or an Xbox 360 disc image): an Xbox 360 title pack (XBA-282;
docs/android/prepare.md « Xbox 360 packs », consoles/x360/runtime/x360_runtime.c).

The same five steps and progress lines as a GameCube pack (tools/prepare/gc.py, whose Run this is):

  1 identify  the XEX: the file itself, or default.xex at the root of the disc's XDVDFS volume (tools/xiso/xdvdfs.py:
              the 360 partition offsets). It is parsed with the player's own key (the XEX_KEY_FILE files: the app
              copies the key file the player picked into its private storage; no key is in the toolkit) and written
              plain (decrypted, expanded) to <work>/plain.xex. An encrypted title without a key that opens it stops
              here with NO_KEY. The title key is the runtime's (X360-<title id>-<CRC32 of the execution info>,
              x360_runtime_key), the source identity the plain XEX's SHA-256.
  2 analyse   what earlier runs found: <work>/discovered.txt (x360_runtime: the addresses the pack did not cover),
              merged with what earlier Prepares lifted (<work>/discovered-seeds.txt): the seeds only grow
  3 lift      `python -m x360_tools.recomp <work>/plain.xex <work>/gen --no-comments --discovered seeds`
              (consoles/x360/tools)
  4 compile   every generated C file with -I consoles/x360/runtime -I consoles/gc/runtime -ffp-contract=off
              -DXE_RUNTIME=1 (bringup's flags), by the shared compile driver (compile.py); -DXE_VIEW4K_DYNAMIC=0
              where the pages of the machine that will run the pack are 4 KB, 1 elsewhere (WP-33, view4k below)
  5 pack      libtitle.so, pack.json ("console": "x360"), default.xex (the plain title the runtime loads), cover.png
              (the title image of the XEX's XDBF resource, when it has one: the app's cover art) and
              shaders/: every shader microcode the runs met (<work>/shaders/*.bin, x360_runtime's dump) translated to
              GLSL ES by xenos_glsl, the .bin kept next to it (a shader that does not translate is not tried again)

Shaders are only known once a run met them: the first pack has none (draws are skipped, reported), each run dumps the
shaders it meets, and the next Prepare translates them. The launcher counts the shaders the runs met that the pack
lacks (Pack.java) and offers Prepare again, as it does for new code: the self-healing loop of the other consoles.

view4k (WP-33, xenon_rt.h XE_VIEW4K_DYNAMIC): a host whose pages are larger than 4 KB cannot map the 360's 4 KB view
where the 360 has it, so the runtime moves it (view4k_skew, x360_mem.c) and every guest memory access of the lifted code
checks for it (a comparison and an addition). Where the pages are 4 KB nothing moves: the pack is compiled without the
check (about a fifth fewer instructions in the hot code). The page size is the real one of the machine running this
Prepare (os.sysconf, as the runtime's sysconf(_SC_PAGESIZE); X360_HOST_PAGE raises it, as the runtime's): on Android
the Prepare runs on the device that plays the pack, on a desktop on the machine that runs it. pack.json says which
("view4k": "static" / "dynamic", "host_page"); check() refuses a static pack on a machine of larger pages (a backup
restored on another device), so a Prepare builds it again, and so does x360_bind ("Prepare it again").
"""

import errno
import hashlib
import os
import re
import shutil
import struct
import sys
import zlib

from tools.prepare import compile as cc
from tools.prepare import gba
from tools.prepare import gc
from tools.prepare import pack
from tools.prepare import ps1

CONSOLE = "x360"
ENTRY = "xe_rc_resume"                     # what x360_runtime.c dlsyms (or xe_rc_resume_parts, XBA-284), metadata
XEX_COPY = "plain.xex"
PACK_XEX = "default.xex"                   # x360_start (pack_loader.c): <pack dir>/default.xex
SHADERS = "shaders"                        # the runtime's dump (<work>/shaders) and the pack's cache (<pack>/shaders)
DISCOVERED = "discovered.txt"              # x360_runtime.c save_missed_locked
SEEDS = "discovered-seeds.txt"
DISCOVERED_MAX = 1 << 20
XEX_MAX = 256 << 20
RECOMP_MODULE_ENV, TOOLS_DIR_ENV = "XBA_X360_RECOMP", "XBA_X360_TOOLS_DIR"
DEFAULT_MODULE = "x360_tools.recomp"
FIELDS = ps1.FIELDS
JOB_RSS = 512 << 20
RESERVED = 1 << 30
SHADER_NAME = re.compile(r"^(vs|ps)_[0-9A-Fa-f]+\.bin$")


def runtime_dir(root):
    return os.path.join(root, "consoles", "x360", "runtime")


def tools_dir(root):
    return os.path.join(root, "consoles", "x360", "tools")


def _x360_tools():
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    d = tools_dir(root)
    if d not in sys.path:
        sys.path.insert(0, d)
    import x360_tools  # noqa: F401
    return root


# ── The source ──────────────────────────────────────────────────

def _head(path, n=4):
    try:
        with open(path, "rb") as f:
            return f.read(n)
    except OSError:
        return b""


def _disc(path):
    """The Xiso of an Xbox 360 disc image holding default.xex at its root, else None."""
    from tools.xiso.xdvdfs import Xiso, XisoError
    try:
        x = Xiso(path)
    except (XisoError, OSError, ValueError):
        return None
    try:
        if x.find("default.xex") is None:   # an original Xbox XISO holds default.xbe: not ours
            x.close()
            return None
    except Exception:
        x.close()
        return None
    return x


def is_x360(path):
    """An XEX (by its magic) or an Xbox 360 disc image (an XDVDFS volume at a 360 partition offset with default.xex)."""
    if not os.path.isfile(path):
        return False
    if _head(path) == b"XEX2":
        return True
    x = _disc(path)
    if x is None:
        return False
    x.close()
    return True


def title_key(raw):
    """x360_runtime_key / recomp_pack_rom_identify: X360-<title id>-<CRC32 of the execution info's 24 bytes>."""
    if len(raw) < 24 or raw[:4] != b"XEX2":
        return None
    count = struct.unpack(">I", raw[20:24])[0]
    for i in range(min(count, 64)):
        if 32 + 8 * i > len(raw):
            break
        k, v = struct.unpack(">II", raw[24 + 8 * i:32 + 8 * i])
        if k == 0x00040006 and v + 24 <= len(raw):
            return "X360-%08X-%08X" % (struct.unpack(">I", raw[v + 12:v + 16])[0],
                                       zlib.crc32(raw[v:v + 24]) & 0xFFFFFFFF)
    return None


def read_xex_bytes(path):
    """The XEX's bytes: the file, or default.xex out of a disc image. ValueError with why."""
    if _head(path) == b"XEX2":
        if os.path.getsize(path) > XEX_MAX:
            raise ValueError("an XEX of over %d MB" % (XEX_MAX >> 20))
        with open(path, "rb") as f:
            return f.read()
    x = _disc(path)
    if x is None:
        raise ValueError("not an XEX nor an Xbox 360 disc image with default.xex")
    try:
        e = x.find("default.xex")
        if e.size > XEX_MAX:
            raise ValueError("default.xex is over %d MB" % (XEX_MAX >> 20))
        return x.read(e)
    finally:
        x.close()


class NoKey(ValueError):
    pass


def plain(raw):
    """(plain XEX bytes, parsed image) with the player's keys ($XEX_KEY_FILE). NoKey when the image stays closed."""
    _x360_tools()
    from tools.xex.xex import XexError, load_keys, parse_xex, plain_xex
    try:
        img = parse_xex(raw, load_keys())
    except XexError as e:
        raise ValueError("XEX: %s" % e)
    except OSError as e:
        raise NoKey("the key file cannot be read: %s" % (e.strerror or e))
    if img.image is None:
        if img.encryption:
            raise NoKey(img.image_note or "encrypted")
        raise ValueError("XEX: %s" % img.image_note)
    return plain_xex(raw, img), img


# ── The title image (cover art) ─────────────────────────────────

XDBF_IMAGE, XDBF_TITLE_ID = 2, 0x8000      # the XDBF image section and the title image's id (the dashboard's tile)
COVER = "cover.png"                        # in the pack: CoverArt.load's title image of a game


def xdbf_entry(blob, section, ident):
    """The bytes of entry (section, id) of an XDBF blob (big-endian: magic, version, entry table size and count, free
    table size and count; 18-byte entries: section u16, id u64, offset u32, length u32; data after both tables), else
    None."""
    if len(blob) < 24 or blob[:4] != b"XDBF":
        return None
    _ver, emax, ecount, fmax, _fcount = struct.unpack_from(">IIIII", blob, 4)
    if emax > 65536 or ecount > emax or fmax > 65536:
        return None
    data = 24 + emax * 18 + fmax * 8
    for i in range(ecount):
        o = 24 + i * 18
        if o + 18 > len(blob):
            return None
        sec, eid, off, ln = struct.unpack_from(">HQII", blob, o)
        if sec == section and eid == ident and data + off + ln <= len(blob):
            return blob[data + off:data + off + ln]
    return None


def title_image(img):
    """The title's image (a PNG) from its XDBF resource (named by the title ID, else any XDBF one), else None."""
    if img.image is None:
        return None
    tid = "%08X" % (img.execution or {}).get("title_id", 0)
    res = sorted(img.resources, key=lambda r: r[0].upper() != tid)
    for name, addr, size in res:
        o = addr - img.base
        if o < 0 or size <= 0 or o + size > len(img.image):
            continue
        png = xdbf_entry(img.image[o:o + size], XDBF_IMAGE, XDBF_TITLE_ID)
        if png and png[:8] == b"\x89PNG\r\n\x1a\n":
            return png
    return None


# ── Discovered code ─────────────────────────────────────────────

def read_addresses(path):
    out = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.split("#", 1)[0].strip()
                if not s:
                    continue
                try:
                    a = int(s.split()[0], 16)
                except ValueError:
                    continue
                if 0 <= a <= 0xFFFFFFFF and not a & 3 and len(out) < DISCOVERED_MAX:
                    out.add(a)
    except OSError:
        pass
    return out


# ── Shaders ─────────────────────────────────────────────────────

def translate_shaders(dump_dir, out_dir):
    """Every vs_*/ps_*.bin of dump_dir not in out_dir yet: copied, and translated next to it (.vert / .frag) when
    xenos_glsl can. (new, translated) counts; what it cannot translate stays as its .bin (known, not tried again)."""
    _x360_tools()
    from x360_tools.xenos_glsl import Unsupported, translate_cached as translate
    new = done = 0
    if not os.path.isdir(dump_dir):
        return 0, 0
    os.makedirs(out_dir, exist_ok=True)
    for name in sorted(os.listdir(dump_dir)):
        if not SHADER_NAME.match(name) or os.path.isfile(os.path.join(out_dir, name)):
            continue
        with open(os.path.join(dump_dir, name), "rb") as f:
            raw = f.read()
        pixel = name.startswith("ps")
        words = list(struct.unpack(">%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
        try:
            glsl, _ = translate(words, "pixel" if pixel else "vertex", interpolators=16 if pixel else 0)
        except Unsupported:
            glsl = None
        except Exception:     # a shader the translator chokes on must not stop the Prepare
            glsl = None
        if glsl is not None:
            dst = os.path.join(out_dir, name[:-4] + (".frag" if pixel else ".vert"))
            with open(dst + ".tmp", "w") as f:
                f.write(glsl)
            os.replace(dst + ".tmp", dst)
            done += 1
        shutil.copyfile(os.path.join(dump_dir, name), os.path.join(out_dir, name + ".tmp"))
        os.replace(os.path.join(out_dir, name + ".tmp"), os.path.join(out_dir, name))
        new += 1
    return new, done


def shader_counts(d):
    """(known, translated) of a shader folder."""
    if not os.path.isdir(d):
        return 0, 0
    names = os.listdir(d)
    return (sum(1 for n in names if SHADER_NAME.match(n)),
            sum(1 for n in names if n.endswith((".vert", ".frag"))))


# ── pack.json ───────────────────────────────────────────────────

def make_meta(lib_path, key, title, source_sha256, boot, abi, shell_abi, toolkit_commit, toolkit_sha256, cc_text,
              discovered, now=None):
    meta = ps1.make_meta(lib_path, key, title, source_sha256, boot, abi, shell_abi, toolkit_commit, toolkit_sha256,
                         cc_text, discovered, now)
    meta["console"] = CONSOLE
    meta["entry"] = ENTRY
    page = host_page_size()     # the page size compile_flags saw (the same process, the same machine)
    meta["view4k"] = "static" if view4k_static(page) else "dynamic"
    meta["host_page"] = page
    return meta


def view4k_refusal(meta, page=None):
    """None when this machine can run a pack of this pack.json's view4k, else why not (WP-33). A pack without the
    field (before WP-33) is not judged here: x360_bind still refuses one that cannot follow the machine."""
    page = host_page_size() if page is None else page
    if meta.get("view4k") == "static" and not view4k_static(page):
        return ("prepared for a device with 4 KB memory pages (this one's are %d bytes): Prepare it again" % page)
    return None


def read(pack_dir):
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
    """None when the Xbox 360 pack is loadable for this title, else why not (gc.check's, plus default.xex)."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    for k, want in (("format", pack.FORMAT), ("title_key", key), ("source_sha256", source_sha256), ("abi", abi),
                    ("shell_abi", shell_abi), ("entry", ENTRY)):
        if meta[k] != want:
            return "%s %r" % (k, meta[k])
    lib = os.path.join(pack_dir, meta["lib"])
    if "/" in meta["lib"] or not os.path.isfile(lib) or os.path.getsize(lib) != meta["lib_size"]:
        return "%s missing or of the wrong size" % meta["lib"]
    if not os.path.isfile(os.path.join(pack_dir, PACK_XEX)):
        return "no %s" % PACK_XEX
    why = view4k_refusal(meta)
    if why:
        return why
    if verify_hash and pack.sha256_file(lib) != meta["lib_sha256"]:
        return "%s sha256 mismatch" % meta["lib"]
    return None


def lifted_opt(opt):
    """bringup's -O1 for lifted Xenon code (a whole title is millions of instructions: the compile time on a phone
    matters); -O0 stays -O0."""
    return "0" if opt == "0" else "1"


def host_page_size():
    """The page size of this machine, the one that will run the pack (the Prepare runs there: on Android on the device
    itself), as x360_mem.c's host_page_size reads it: sysconf, raised (never lowered) by X360_HOST_PAGE."""
    try:
        page = int(os.sysconf("SC_PAGE_SIZE"))
    except (AttributeError, ValueError, OSError):
        import mmap
        page = mmap.PAGESIZE
    if page <= 0:
        page = 0x1000
    e = os.environ.get("X360_HOST_PAGE", "")
    if e:
        try:
            v = int(e, 0)
        except ValueError:
            v = 0
        if page <= v <= 0x10000 and not v & (v - 1):
            page = v
    return page


def view4k_static(page):
    """WP-33: the pack may leave out view4k_skew (XE_VIEW4K_DYNAMIC=0) where the host's pages are 4 KB (or smaller):
    the runtime's view does not move there (x360_mem.c: skew 0 up to 4 KB pages)."""
    return page <= 0x1000


def view4k_flag(page):
    return "-DXE_VIEW4K_DYNAMIC=%d" % (0 if view4k_static(page) else 1)


def lifted_cflags(opt):
    return gba.lifted_cflags(opt) + ["-ffp-contract=off", "-DXE_RUNTIME=1"]


def compile_flags(opt, root, gen, sysroot=None, include_dirs=(), page=None):
    flags = []
    if sysroot:
        flags += ["--target=" + cc.TARGET, "--sysroot=" + sysroot]
        rd = cc.sysroot_resource_dir(sysroot)
        if rd:
            flags.append("-resource-dir=" + rd)
    flags += lifted_cflags(opt) + ["-g0", "-fPIC", "-std=gnu11", "-w", "-DNDEBUG"]
    flags.append(view4k_flag(host_page_size() if page is None else page))
    for d in [gen, runtime_dir(root), gc.runtime_dir(root)] + list(include_dirs):
        flags.append("-I" + d)
    return flags


# ── The run: the GameCube's (gc.Run), with an XEX ──

class Run(gc.Run):
    cm = sys.modules[__name__]

    def identify(self):
        self.rep.progress(1, 0, "reading the title")
        a = self.a
        try:
            raw = read_xex_bytes(self.source)
            key = title_key(raw)
            if not key:
                raise ValueError("the XEX has no execution info: no title key")
            data, img = plain(raw)
        except NoKey as e:
            raise cc.StepError("NO_KEY", "%s: %s" % (os.path.basename(self.source), e))
        except (FileNotFoundError, PermissionError) as e:
            raise cc.StepError("NO_DISC", "%s: %s" % (self.source, e.strerror or e))
        except (OSError, ValueError) as e:
            raise cc.StepError("BAD_DISC", "%s: %s" % (self.source, getattr(e, "strerror", None) or e))
        self.key = key
        base = os.path.basename(self.source)
        self.title = os.path.splitext(base)[0] or base
        self.name = "%s-%s" % (CONSOLE, self.key)
        self.rom_sha = hashlib.sha256(data).hexdigest()
        self.boot = PACK_XEX
        self.work = os.path.abspath(a.work or os.path.join(gba.data_home(), "work", self.name))
        self.packs = os.path.abspath(a.pack_dir or os.path.join(gba.data_home(), "packs"))
        try:
            os.makedirs(self.work, exist_ok=True)
        except OSError as e:
            raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (self.work, e))
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.rom = os.path.join(self.work, XEX_COPY)
        gc._write_if_changed(self.rom, data)
        cover = title_image(img)          # the cover art the app shows (CoverArt.load: <pack>/cover.png)
        if cover:
            gc._write_if_changed(os.path.join(self.work, COVER), cover)
        self.rels, self.rels_sha = [], ""
        from tools.prepare import toolkit
        self.toolkit = self.m.toolkit_version()
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("source") != self.rom_sha or self.stamp.get("toolkit_sha256") != self.toolkit_sha:
            self.stamp = {}
            # another toolkit may translate differently (GLSL_VERSION): the shaders are translated again
            shutil.rmtree(os.path.join(self.work, "glsl"), ignore_errors=True)
            self.save_stamp(console=CONSOLE, source=self.rom_sha, toolkit=self.toolkit,
                            toolkit_sha256=self.toolkit_sha)
        self.steps.skipped = [k for k in ("lift", "link") if self.stamp.get(k)]
        self.rep.progress(1, 100, "%s (Xbox 360, key %s, image %d KB, %s)"
                          % (self.title, self.key, img.image_size // 1024,
                             "encrypted, opened with your key" if img.encryption else "not encrypted"))

    def analyse(self):
        seeds = os.path.join(self.work, SEEDS)
        addrs = read_addresses(os.path.join(self.work, DISCOVERED)) | read_addresses(seeds)
        extra = getattr(self.a, "discovered", None)
        if extra:
            addrs |= read_addresses(extra)
        lines = ["%08X" % v for v in sorted(addrs)][:DISCOVERED_MAX]
        if lines:
            ps1.write_records(seeds, lines)
        self.seeds = seeds if lines else None
        self.discovered = len(lines)
        self.disc_key = hashlib.sha256("".join(x + "\n" for x in lines).encode()).hexdigest()
        known, _ = shader_counts(os.path.join(self.work, SHADERS))
        self.rep.progress(2, 100, "%d address(es) and %d shader(s) met by earlier runs" % (len(lines), known)
                          if lines or known else "nothing from earlier runs")

    def lift_args(self, module, out):
        args = [self.py, "-m", module, self.rom, out, "--no-comments"]
        if self.seeds:
            args += ["--discovered", self.seeds]
        return args

    def lift_note(self):
        return "lifting to C (the XEX's code%s)" % (", %d address(es) from earlier runs" % self.discovered
                                                     if self.discovered else "")

    def _lift_summary(self, gen):
        import json
        try:
            with open(os.path.join(gen, "stats.json")) as f:
                s = json.load(f)
        except (OSError, ValueError):
            return ""
        return ", %d functions, %d instructions to the interpreter" % (s.get("functions", 0),
                                                                        s.get("interp_fallback_insns", 0))

    def link(self, out):
        super().link(out)
        self._fill(os.path.dirname(out))

    def _fill(self, d):
        """default.xex and the shader cache into a pack folder (the partial one before publish, the published one when
        the link was up to date: shaders met since then still go in)."""
        if not os.path.isdir(d):
            return
        dst = os.path.join(d, PACK_XEX)
        if not os.path.isfile(dst) or pack.sha256_file(dst) != self.rom_sha:
            shutil.copyfile(self.rom, dst + ".tmp")
            os.replace(dst + ".tmp", dst)
        src = os.path.join(self.work, COVER)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(d, COVER + ".tmp"))
            os.replace(os.path.join(d, COVER + ".tmp"), os.path.join(d, COVER))
        cache = os.path.join(self.work, "glsl")      # the translations, kept across packs
        os.makedirs(cache, exist_ok=True)
        self.shaders_new, _ = translate_shaders(os.path.join(self.work, SHADERS), cache)
        out = os.path.join(d, SHADERS)
        os.makedirs(out, exist_ok=True)
        for n in os.listdir(cache):
            p = os.path.join(out, n)
            if not os.path.isfile(p):
                shutil.copyfile(os.path.join(cache, n), p + ".tmp")
                os.replace(p + ".tmp", p)
        self.shaders_known, self.shaders_done = shader_counts(out)

    def make_pack(self):
        final = ps1.Run.make_pack(self)
        self._fill(final)
        self.rep.progress(5, 100, "pack %s: %d shader(s), %d translated to GLSL ES (%d new)"
                          % (self.name, self.shaders_known, self.shaders_done, self.shaders_new))
        return final


def run(a, host, android):
    return gba.run(a, host, android, run_class=Run)
