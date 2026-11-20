"""
Prepare an Xbox title from your own copy of the game.

    python -m tools.prepare SOURCE [--work DIR] [--no-build] [--no-data] [--progress]
    python -m tools.prepare SOURCE [--work DIR] --check [--json]

SOURCE is a disc image (.iso / .xiso), a folder holding default.xbe (the
extracted disc), or a default.xbe. Steps, each skipped when its output is
already there for this XBE, this toolkit and this title profile:

  1. find default.xbe (out of the image if needed) and read its title ID;
     the player's mods turned on for it (docs/technical/mods.md): their
     patches go to a copy of the XBE in the work folder, which the next steps
     use (left out with --no-mods)
  2. the analysis pipeline: xbe_parser, disasm, func_id, abi_analysis
  3. the title project: templates/new-game on the toolkit's defaults, plus
     the optional layers of profiles/ (docs/technical/profiles.md) and the
     options the layers and mods force
  4. the C lift (tools.recomp) into the project
  5. build it for this host, with the game data next to it: the compile
     driver against the toolkit's prebuilt runtime when it has one for this
     host (runtime/<host>/, no CMake: tools/prepare/desktop_build.py), else
     CMake

The work folder (default: <title id>-work next to SOURCE) holds everything it
writes; nothing is written into the toolkit. Nothing from the game is ever
uploaded or shared: the output is for your own machines.

--progress adds machine-readable progress lines (XBA_PROGRESS / XBA_RESULT,
tools/prepare/progress.py) to the output; --check says whether the title is
prepared and up to date without doing anything. Both are the contract with
the macOS launcher: docs/macos/launcher.md « Prepare interface ».

With --android it makes a title pack for the Android app instead (libtitle.so
+ pack.json, built with clang and ld.lld, no CMake), with machine-readable
progress lines: docs/android/prepare.md.

A Game Boy Advance ROM (.gba) makes a GBA title pack, with --android for the
app or for gba_play on this host (tools/prepare/gba.py); a PlayStation disc
(.cue / .bin / .iso) or PS-X EXE a PS1 title pack, for the app or for
ps1_play (tools/prepare/ps1.py); a Nintendo 64 ROM (.z64 / .v64 / .n64) an
N64 title pack, for the app or for n64_play / n64_run (tools/prepare/n64.py); a
GameCube disc (.iso / .gcm / .ciso / .gcz / .rvz / .wia) or DOL a GameCube title
pack, for the app or for gc_play (tools/prepare/gc.py); an Xbox 360 XEX or disc image an Xbox 360 title pack, with
the player's key in $XEX_KEY_FILE (tools/prepare/x360.py).
"""

import argparse
import errno
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEMPLATE = os.path.join(ROOT, "templates", "new-game")
PROFILES = os.path.join(ROOT, "profiles")
DISCOVERY_FILE = "discovered.jsonl"     # in the work folder: src/kernel/recomp_discovery.h
STEPS = 5

# --progress (docs/macos/launcher.md « Prepare interface »): the Reporter that
# writes the protocol lines, the step running, and the error code a failure
# in it maps to. None / unused without --progress: the output stays as it was.
_REP = None
_STEP = 0
_CODE = "INTERNAL"
STEP_CODES = {1: "NO_XBE", 2: "ANALYSE_FAILED", 3: "INTERNAL", 4: "LIFT_FAILED", 5: "COMPILE_FAILED"}


def step(n, msg):
    global _STEP, _CODE
    _STEP, _CODE = n, STEP_CODES[n]
    print("[%d/%d] %s" % (n, STEPS, msg), flush=True)
    progress(0, msg)


def progress(pct, msg):
    """An XBA_PROGRESS line for the step running (with --progress only)."""
    if _REP is not None:
        _REP.progress(_STEP, pct, msg)


def note(msg, pct=None):
    """A detail line of the step running, also as its progress message."""
    print("      " + msg, flush=True)
    if pct is not None:
        progress(pct, msg)


class CommandFailed(SystemExit):
    """run()'s failure: a SystemExit whose text is what it always printed."""

    def __init__(self, rc, args, log, tail):
        SystemExit.__init__(self, "command failed (%d): %s\n--- %s (end) ---\n%s"
                            % (rc, " ".join(args), log, tail))
        self.rc, self.cmd, self.log = rc, args, log

    def short(self):
        tool = self.cmd[2] if len(self.cmd) > 2 and self.cmd[1] == "-m" else os.path.basename(self.cmd[0])
        if tool == "cmake" and "--build" in self.cmd:
            tool = "cmake --build"
        return "%s failed (exit %d), see %s" % (tool, self.rc, self.log)


class Cancelled(BaseException):
    """SIGTERM / SIGINT under --progress."""


# Ninja's "[12/345] ..." and make's "[ 45%] ...": how far a build is.
_NINJA = re.compile(r"^\[(\d+)/(\d+)\]")
_MAKE = re.compile(r"^\[\s*(\d+)%\]")


def build_fraction(line):
    """(0.0-1.0, "12/345" or "45 %") from one line of a CMake build's output, or None."""
    m = _NINJA.match(line)
    if m:
        return int(m.group(1)) / max(1, int(m.group(2))), "%s/%s" % m.groups()
    m = _MAKE.match(line)
    if m:
        return min(100, int(m.group(1))) / 100.0, "%s %%" % m.group(1)
    return None


class BuildProgress:
    """on_line for the build: step 5 from 5 to 90 %, never going back, one
    line when the percentage moves or every 2 s (a big title has thousands of
    files). CMake's own "[1/2] Re-checking globbed directories" is not the build."""

    def __init__(self, lo=5, hi=90):
        self.lo, self.hi, self.last, self.t = lo, hi, None, 0.0

    def __call__(self, line):
        got = build_fraction(line)
        if got is None or "Re-checking globbed directories" in line:
            return
        pct = max(self.lo + int((self.hi - self.lo) * got[0]), self.last or 0)
        now = time.time()
        if pct != self.last or now - self.t >= 2.0:
            self.last, self.t = pct, now
            progress(pct, "compiling %s" % got[1])


def _elapsed(sec):
    sec = int(sec)
    return "%d s" % sec if sec < 60 else "%d min %02d s" % (sec // 60, sec % 60)


def run(args, log, cwd=ROOT, on_line=None, tick=None):
    """Run a command, its output appended to `log`; raise with the tail on failure.
    With on_line, each output line is also handed to it as it comes. With
    tick=(pct_from, pct_to, msg, typical_s) and --progress, a long silent step
    still moves: every few seconds the step's percent creeps from pct_from
    towards pct_to (never reaching it) and the message carries the elapsed
    time, so a launcher does not look stuck on "disassembling". The command
    heads its own process group: a cancel or a crash kills it and all it
    started (cmake --build's ninja and compilers too, not cmake alone)."""
    from tools.prepare import proctree
    t0 = time.time()
    with open(log, "a") as f:
        f.write("\n$ %s\n" % " ".join(args))
        f.flush()
        if on_line is None:
            p = subprocess.Popen(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, **proctree.popen_kw())
        else:
            p = subprocess.Popen(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 universal_newlines=True, errors="replace", bufsize=1, **proctree.popen_kw())
        try:
            if on_line is not None:
                for line in p.stdout:
                    f.write(line)
                    on_line(line)
                p.stdout.close()
                f.flush()
            if tick is not None and on_line is None and _REP is not None:
                lo, hi, msg, typical = tick
                while True:
                    try:
                        rc = p.wait(timeout=5)
                        break
                    except subprocess.TimeoutExpired:
                        el = time.time() - t0
                        frac = el / (el + max(typical, 1.0))     # 0 -> 1, never 1
                        progress(int(lo + (hi - lo) * frac), "%s (%s)" % (msg, _elapsed(el)))
            else:
                rc = p.wait()
        except BaseException:       # never leave the child, or what it started, running
            proctree.kill_tree(p)
            p.wait()
            raise
    if rc != 0:
        with open(log) as f:
            tail = f.read()[-3000:]
        raise CommandFailed(rc, args, log, tail)
    return time.time() - t0


# The line tools.recomp prints for bodies lifted with a recomp_cut_body trap
# (tools/recomp/__main__.py CUT_BODIES_PREFIX, WP2).
CUT_BODIES_PREFIX = "cut bodies trapped (recomp_cut_body):"


def _lift_line(log, offset, prefix):
    """What follows `prefix` on the first line of `log` past byte `offset`
    that starts with it, or None."""
    try:
        with open(log, "rb") as f:
            f.seek(offset)
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return None


def lift_cut_bodies(log, offset):
    """The cut-body report tools.recomp wrote into `log` past byte `offset`
    ("N: sub_X (last 0x...), ..."), or None when the lift had none. Each is
    a body the analysis cut short; reaching its end stops the title."""
    return _lift_line(log, offset, CUT_BODIES_PREFIX)


# The line tools.recomp prints with the counts of each class of code the
# title cannot reach (tools/recomp/__main__.py LIFT_HEALTH_PREFIX, XBA-139).
LIFT_HEALTH_PREFIX = "lift health:"
LIFT_HEALTH_KEYS = ("seh_lost", "unresolved_calls", "unimplemented", "misdecoded", "cut_bodies")


def lift_health(log, offset):
    """The counts tools.recomp reported past byte `offset` of `log`, as
    {key: int} over LIFT_HEALTH_KEYS, or None (an older lift, or none)."""
    text = _lift_line(log, offset, LIFT_HEALTH_PREFIX)
    if text is None:
        return None
    try:
        raw = json.loads(text)
    except ValueError:
        return None
    if not isinstance(raw, dict):
        return None
    return {k: int(raw.get(k) or 0) for k in LIFT_HEALTH_KEYS}


def health_line(health):
    """The prepare summary's words for lift_health()'s counts, or None
    when every count is zero."""
    if not health or not any(health.values()):
        return None
    words = (("seh_lost", "exception handler targets no function enters"),
             ("unresolved_calls", "unresolved call targets"),
             ("unimplemented", "unimplemented instructions"),
             ("misdecoded", "misdecoded sites (string port I/O)"),
             ("cut_bodies", "cut bodies"))
    return ", ".join("%d %s" % (health[k], w) for k, w in words if health.get(k))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


VERSION_FILE = os.path.join(ROOT, "tools", "VERSION")


def toolkit_version():
    """The toolkit's commit: git, else tools/VERSION (written into the app's
    assets, where there is no git), else "unknown"."""
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    try:
        with open(VERSION_FILE) as f:
            v = f.read().split()
        if v:
            return v[0]
    except OSError:
        pass
    return "unknown"


def toolkit_same(stamp):
    """True when the lift in this stamp is the current toolkit's (XBA-134):
    the toolkit content digest (tools/prepare/toolkit.py, the one the
    Android packs carry), not the commit, so a commit that changes no file
    of the toolkit tree (docs, Java, tests, the runtime's .c files) keeps the
    lift. A stamp from before the digest has only the commit: the same
    commit is the same toolkit."""
    from tools.prepare import toolkit
    if "toolkit_sha256" in stamp:
        return stamp["toolkit_sha256"] == toolkit.digest()
    return stamp.get("toolkit") == toolkit_version()


def runtime_same(stamp):
    """True when the executable was built against the current runtime
    (toolkit.runtime_digest: src/, include/, cmake/): a change there needs a
    rebuild, minutes, not a new lift. A stamp from before it: by commit."""
    from tools.prepare import toolkit
    if "runtime_sha256" in stamp:
        return stamp["runtime_sha256"] == toolkit.runtime_digest()
    return stamp.get("toolkit") == toolkit_version()


def find_xbe(source, work):
    """(path to default.xbe, game data folder or None, image or None)."""
    src = os.path.abspath(source)
    if os.path.isdir(src):
        from tools.prepare.identify import folder_xbe
        xbe = folder_xbe(src)
        if xbe is None:
            raise SystemExit("%s: no default.xbe in this folder" % src)
        return xbe, src, None
    low = src.lower()
    if low.endswith(".xbe"):
        return src, os.path.dirname(src), None
    sys.path.insert(0, ROOT)
    from tools.xiso.source import is_image_name
    if is_image_name(src):                  # .iso/.xiso/.cso/.zso/.cci, or a split part (XBA-126)
        from tools.xiso.xdvdfs import Xiso
        dest = os.path.join(work, "xbe", "default.xbe")
        if not os.path.isfile(dest):
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with Xiso(src) as iso:
                e = iso.find("default.xbe")
                if e is None:
                    raise SystemExit("%s: no default.xbe in the image" % src)
                # Whole or not there: a run killed part way must not leave a short default.xbe that
                # every later run takes for the extracted one.
                part = dest + ".part"
                iso.extract(e, part)
                os.replace(part, dest)
        return dest, None, src
    raise SystemExit("%s: expected a disc image (.iso/.xiso/.cso/.zso/.cci, or a split part), a folder with "
                     "default.xbe, or default.xbe" % src)


def merge_tree(src, dst):
    """Moves everything under src into dst and removes src. A folder that is already in dst is merged, a
    file replaced: shutil.move would put the folder inside the one that is there (xbe/xbe/default.xbe on
    a second run, an error on the third)."""
    os.makedirs(dst, exist_ok=True)
    for fn in os.listdir(src):
        s, d = os.path.join(src, fn), os.path.join(dst, fn)
        if os.path.isdir(s) and not os.path.islink(s):
            merge_tree(s, d)
        else:
            if os.path.isdir(d) and not os.path.islink(d):
                shutil.rmtree(d)
            os.replace(s, d)
    os.rmdir(src)


def unpack_image(image, dest):
    sys.path.insert(0, ROOT)
    from tools.xiso.xdvdfs import Xiso
    with Xiso(image) as iso:
        entries = list(iso.walk())
        total = sum(e.size for _, e in entries) or 1
        done = 0
        last = -1
        for d, e in entries:
            out = os.path.join(dest, *(d.split("/") if d else []), e.name)
            if not (os.path.isfile(out) and os.path.getsize(out) == e.size):
                iso.extract(e, out)
            done += e.size
            pct = done * 100 // total
            if pct != last and pct % 5 == 0:
                print("      extracting the disc: %d %%" % pct, flush=True)
                progress(90 + pct * 8 // 100, "extracting the disc: %d %%" % pct)
                last = pct


def profile_digest(profile):
    """What the analysis and the lift depend on beyond the XBE and the toolkit
    commit: the profile layers' files (seed_functions and exclude_manual name
    some of them). Both the desktop and the Android stamps hold it."""
    from tools.prepare import profiles
    h = hashlib.sha256()
    profiles.digest_into(h, profile)
    return h.hexdigest()


def load_profile(tid, ident):
    """The merged profile layers for this title (tools.prepare.profiles), or
    None. Raises profiles.ProfileError."""
    from tools.prepare import profiles
    return profiles.resolve(tid, ident, PROFILES, title_layer=not no_title_layer())


def title_layer_state(tid):
    """"on" when the title has optional enhancements (the player's mods turned
    on in the store, docs/technical/mods.md; or a profiles/titles/<tid> layer)
    and they go in, "off" when they are left out (--no-mods / --no-title-layer,
    XBA_NO_MODS=1 / XBA_NO_TITLE_LAYER=1), None when it has none: what
    pack.json and --check say, for a launcher's « Enhancements » switch."""
    from tools.prepare import profiles
    from tools.mods import patch
    if not profiles.find_title_layer(tid, PROFILES) and not patch.enabled_count("xbox", tid):
        return None
    return "off" if no_title_layer() else "on"


def mods_store_has(tid):
    from tools.mods import patch
    return patch.enabled_count("xbox", tid) > 0


def mods_plan(tid, xbe):
    """The title's enabled mods (tools.mods.patch.plan): their patches applied
    to a copy of the XBE's bytes, the mod-set hash, the options they force.
    None of it when the launcher's switch leaves them out. Raises
    tools.mods.package.ModError (code BAD_MOD, WRONG_TITLE, WRONG_BINARY...)."""
    from tools.mods import patch
    with open(xbe, "rb") as f:
        data = f.read()
    return patch.plan("xbox", tid, data, off=no_title_layer())


def write_patched(path, data):
    """The patched XBE at path (written only when it differs): the original is
    never touched."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return path
    except OSError:
        pass
    with open(path + ".tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".tmp", path)
    return path


def no_title_layer():
    """--no-title-layer, or XBA_NO_TITLE_LAYER=1 in the environment (what a
    launcher's child inherits, its per-title « Enhancements » switch off),
    or XBA_NO_MODS=1: prepare with the title's optional enhancements (its
    mods) left out. The option sets the variable, so the disc's other images
    follow."""
    return any(os.environ.get(k, "") not in ("", "0") for k in ("XBA_NO_TITLE_LAYER", "XBA_NO_MODS"))


def sync_generated(new_dir, dest_dir):
    """Move the files of a fresh lift (new_dir) into dest_dir, keeping every
    file whose content did not change as it was -- same bytes, same mtime --
    so the build recompiles only what the lift changed (XBA-65). Files the
    lift no longer writes are removed. Returns (kept, written, removed)."""
    import filecmp
    os.makedirs(dest_dir, exist_ok=True)
    new = set(os.listdir(new_dir))
    kept = written = removed = 0
    for fn in sorted(new):
        src, dst = os.path.join(new_dir, fn), os.path.join(dest_dir, fn)
        if os.path.isfile(dst) and filecmp.cmp(src, dst, shallow=False):
            os.remove(src)
            kept += 1
        else:
            os.replace(src, dst)
            written += 1
    for fn in os.listdir(dest_dir):
        if fn not in new:
            os.remove(os.path.join(dest_dir, fn))
            removed += 1
    shutil.rmtree(new_dir, ignore_errors=True)
    return kept, written, removed


def read_stamp(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def c_text(s):
    """`s` safe inside a C string literal that is also a printf format, and
    inside a comment."""
    s = "".join(ch for ch in s if " " <= ch <= "~")
    return (s.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%")
             .replace("*/", "* /").replace("??", "?\\?"))


def env_force_header(tid, env):
    """src/title/env_force.h: title.json's env_force for the desktop main.c,
    which sets each one before anything reads the environment (the Android
    app gets the same from pack.json). Keys and values are already checked
    (pack.env_force_of: RECOMP_* names, printable ASCII)."""
    def cstr(v):
        return '"%s"' % v.replace("\\", "\\\\").replace('"', '\\"').replace("?", "\\?")
    lines = ["/* Generated by tools.prepare from the profile layers' \"env_force\" (%s): the options this" % tid.upper(),
             " * title forces at start-up (docs/runtime/environment-variables.md « Forced by a title »). */",
             "#define XBA_TITLE_ENV_FORCE_TID %s" % cstr(tid.upper()),
             "static const char *const xba_title_env_force[][2] = {"]
    lines += ["    { %s, %s }," % (cstr(k), cstr(v)) for k, v in env.items()]
    lines += ["};", ""]
    return "\n".join(lines)


def identify_xbe(xbe, work):
    """tools.xbe_ident's answer (XDK version, engines, profile layers) in
    work/ident.json, or None when it cannot tell: the prepare goes on
    without it (it only informs the layers, XBA-66)."""
    from tools import xbe_ident
    try:
        ident = xbe_ident.identify(xbe)
    except Exception as e:
        print("      could not identify the XDK and engine: %s" % e, flush=True)
        return None
    ident["summary"] = xbe_ident.summary(ident)
    # Said, not fatal: the rest of the title may still run (XBA-124).
    for b in xbe_ident.blockers(ident):
        print("      BLOCKER: %s" % b, flush=True)
    with open(os.path.join(work, "ident.json"), "w", encoding="utf-8") as f:
        json.dump(ident, f, indent=1)
    return ident


def assemble_project(proj, profile, info, env=None):
    """templates/new-game + the profile layers in src/title/: each layer's .c
    files (profiles.c_name), xba_layers.c (the hooks main.c calls),
    xba_params.h, and env_force.h when options are forced (env: the layers'
    env_force and the mods' options; default the layers' alone). main.c gets
    the title's name and entry point in place of the template's placeholders."""
    from tools.prepare import profiles
    from tools.prepare import pack
    if env is None:
        env = pack.env_force_of(profile)
    os.makedirs(os.path.join(proj, "src", "recomp", "gen"), exist_ok=True)
    name = (profile or {}).get("project") or "title_%s" % info["title_id"][2:].lower()
    with open(os.path.join(TEMPLATE, "CMakeLists.txt")) as f:
        cm = f.read().replace("project(your_game_recomp C)", "project(%s C)" % name)
    with open(os.path.join(proj, "CMakeLists.txt"), "w") as f:
        f.write(cm)
    with open(os.path.join(TEMPLATE, "src", "main.c")) as f:
        main = f.read()
    title = c_text((profile or {}).get("name") or info.get("title") or name)
    entry = int(info.get("entry_point", "0"), 16)
    tid = int(info.get("title_id", "0"), 16)
    for old, new in (('"game\\\\Your Game Title\\\\default.xbe"', '"game/default.xbe"'),
                     ('"game\\\\Your Game Title"', '"game"'),
                     ("#define YOUR_GAME_ENTRY_POINT   0x00000000",
                      "#define YOUR_GAME_ENTRY_POINT   0x%08X" % entry),
                     (" *   Title ID:    0x00000000", " *   Title ID:    0x%08X" % tid),
                     (" *   Entry point: 0x00000000", " *   Entry point: 0x%08X" % entry),
                     ("YOUR_GAME_NAME", title)):
        main = main.replace(old, new)
    with open(os.path.join(proj, "src", "main.c"), "w") as f:
        f.write(main)
    shutil.copy(os.path.join(TEMPLATE, "src", "recomp_manual.c"),
                os.path.join(proj, "src", "recomp_manual.c"))
    tdir = os.path.join(proj, "src", "title")
    shutil.rmtree(tdir, ignore_errors=True)
    if profile:
        os.makedirs(tdir)
        for src, dest in profile["c_files"]:
            shutil.copy(src, os.path.join(tdir, dest))
        with open(os.path.join(tdir, "xba_layers.c"), "w") as f:
            f.write(profiles.layers_source(profile))
        with open(os.path.join(tdir, "xba_params.h"), "w") as f:
            f.write(profiles.params_header(profile))
    if env:
        os.makedirs(tdir, exist_ok=True)
        with open(os.path.join(tdir, "env_force.h"), "w") as f:
            f.write(env_force_header(info["title_id"][2:], env))
    return name


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.prepare", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="a disc image (.iso/.xiso/.cso/.zso/.cci, or a split part), a folder with "
                                   "default.xbe, or default.xbe")
    ap.add_argument("--work", help="work folder (default: <title id>-work next to SOURCE)")
    ap.add_argument("--no-build", action="store_true", help="stop after the C lift")
    ap.add_argument("--no-data", action="store_true",
                    help="no game data for the desktop build (the image is neither linked nor extracted)")
    ap.add_argument("--extract", action="store_true",
                    help="extract the disc image into <work>/game for the desktop build, as before XBA-126 "
                         "(default: the build reads the image in place)")
    ap.add_argument("--force", action="store_true", help="redo every step")
    ap.add_argument("--no-mods", "--no-title-layer", dest="no_title_layer", action="store_true",
                    help="leave the title's optional enhancements out (the mods turned on for it, "
                         "docs/technical/mods.md): prepare the game as it is (also XBA_NO_MODS=1 or "
                         "XBA_NO_TITLE_LAYER=1 in the environment: a launcher's « Enhancements » switch off)")
    ap.add_argument("--mods", metavar="DIR",
                    help="the mod store (default: $XBA_MODS_DIR, else <user data>/mods; docs/technical/mods.md)")
    mi = ap.add_argument_group("for a launcher (docs/macos/launcher.md « Prepare interface »)")
    mi.add_argument("--progress", action="store_true",
                    help="also print XBA_PROGRESS / XBA_RESULT lines (tools/prepare/progress.py)")
    mi.add_argument("--check", action="store_true",
                    help="say whether the title is prepared and up to date (absent, stale, ready); writes nothing")
    mi.add_argument("--json", action="store_true", help="with --check: one JSON object on stdout")
    an = ap.add_argument_group("Android title pack (docs/android/prepare.md)")
    an.add_argument("--android", action="store_true", help="make a title pack for the Android app")
    an.add_argument("--pack-dir", help="packs folder: the pack goes to PACK_DIR/<TITLEID>/")
    ap.add_argument("--builder", choices=("auto", "driver", "cmake"),
                    help="desktop build: the compile driver against the toolkit's prebuilt runtime (runtime/<host>/), "
                         "or CMake; default auto (the driver when that runtime is there and current, also "
                         "$XBA_BUILDER); --cc, --jobs, --opt and --include-dir apply to the driver")
    an.add_argument("--cc", help="clang (or the multicall LLVM binary, with --cc-prefix-arg clang); desktop: the "
                                 "compiler (default: $XBA_CC, the toolkit's toolchain/, clang, cc)")
    an.add_argument("--cc-prefix-arg", action="append", default=[], metavar="ARG",
                    help="argument(s) put right after --cc (multicall: clang)")
    an.add_argument("--ld", help="ld.lld (default: CC ld.lld for a multicall CC, else ld.lld next to CC)")
    an.add_argument("--ld-prefix-arg", action="append", default=[], metavar="ARG")
    an.add_argument("--sysroot", help="the NDK sysroot (usr/include, usr/lib/aarch64-linux-android/30)")
    an.add_argument("--builtins", help="libclang_rt.builtins-aarch64-android.a (default: asked from CC)")
    an.add_argument("--shell-lib", help="the app shell's libmain.so, which the pack links against")
    an.add_argument("--shell-abi", type=int, default=1, help="the shell ABI the app exports (default 1)")
    an.add_argument("--cpu-features", default="auto", metavar="auto|none|lse",
                    help="CPU features an Xbox pack (or a linux-arm64 desktop build) is compiled for "
                         "(tools/prepare/cpu.py, XBA-313): auto (default) detects the CPU Prepare runs on when it is "
                         "an aarch64 Linux / Android (the device), else none (ARMv8.0); a pack records them and is "
                         "refused on a CPU without them")
    an.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: from free memory)")
    an.add_argument("--opt", default="2", choices=("0", "1", "2", "3", "s"),
                    help="clang -O level (default 2; a GBA ROM compiles 2 and 3 at 1)")
    an.add_argument("--python", help="Python for the pipeline steps (default: $XBA_PYTHON, else this one)")
    an.add_argument("--extra-src", action="append", default=[], metavar="FILE.c",
                    help="an extra C file compiled into the pack (src/extra/)")
    an.add_argument("--include-dir", action="append", default=[], metavar="DIR", help="an extra -I")
    an.add_argument("--device-log", metavar="FILE",
                    help="the app's device log (JSON lines: thermal status, compiles allowed now; XBA-128)")
    an.add_argument("--all-images", action="store_true",
                    help="build every other image default.xbe names now (default: only those asked with --image, "
                         "in <work>/images.want or already in the pack; the others are built on their first launch)")
    an.add_argument("--image", action="append", default=[], metavar="PATH",
                    help="also build this other image of the disc (its disc path, e.g. game.xbe)")
    mi = ap.add_argument_group("the disc's other images (XBA-82; set by the top-level prepare)")
    mi.add_argument("--image-map", help=argparse.SUPPRESS)   # the map file every image's build bakes in
    mi.add_argument("--game-dir", help=argparse.SUPPRESS)    # D:\ for an image that is not the disc root's
    mi.add_argument("--image-name", help=argparse.SUPPRESS)  # that image's disc path (XeImageFileName)
    gb = ap.add_argument_group("Game Boy Advance, PS1 and N64 title packs (docs/android/prepare.md « GBA packs », "
                               "« PS1 packs », « N64 packs »)")
    gb.add_argument("--discovered", metavar="FILE",
                    help="a GBA ROM, a PS1 disc or an N64 ROM: more code the runtime had to interpret, merged with "
                         "<work>/discovered.txt")
    a = ap.parse_args(argv)
    if a.no_title_layer:
        os.environ["XBA_NO_TITLE_LAYER"] = "1"
    if a.mods:
        os.environ["XBA_MODS_DIR"] = os.path.abspath(a.mods)    # the disc's other images follow
    from tools.prepare import x360
    if x360.is_x360(a.source):          # an XEX or an Xbox 360 disc (first, as recomp_pack_rom_identify): XBA-282
        return x360.run(a, sys.modules[__name__], a.android)
    from tools.prepare import n64
    if n64.is_n64(a.source):            # an N64 ROM (its first word, any byte order): the PS1's steps (XBA-174)
        return n64.run(a, sys.modules[__name__], a.android)
    from tools.prepare import gba
    if gba.is_gba(a.source):            # a GBA ROM: its own steps, --android or a desktop pack (XBA-157 / 158)
        return gba.run(a, sys.modules[__name__], a.android)
    from tools.prepare import gc
    if gc.is_gc(a.source):              # a GameCube disc (by content) or DOL: the PS1's steps, REL modules (XBA-264)
        return gc.run(a, sys.modules[__name__], a.android)
    from tools.prepare import ps1
    if ps1.is_ps1(a.source):            # a PS1 disc or EXE: the GBA's steps for a disc (XBA-168)
        return ps1.run(a, sys.modules[__name__], a.android)
    if a.android:
        from tools.prepare import android
        return android.run(a, sys.modules[__name__])
    if a.check:
        return check(a)
    lock = _lock_work(a.work)
    if lock is False:
        msg = "another prepare is already working in %s" % a.work
        print("error: " + msg, file=sys.stderr, flush=True)
        if a.progress:
            from tools.prepare.progress import Reporter
            Reporter().error("BUSY", msg)
        return 1
    try:
        if a.progress:
            return run_with_progress(a)
        desktop(a)
        return 0
    finally:
        if lock:
            lock.close()


def _lock_work(work):
    """Hold an exclusive lock on WORK for the whole prepare, so two prepares
    of one title (a launcher and a terminal) cannot rewrite the same project
    under each other. False when another process holds it; None when there is
    nothing to lock (no --work, or no fcntl on this host); else the open file,
    released when closed or when the process ends."""
    if not work:
        return None
    try:
        import fcntl
    except ImportError:
        return None
    os.makedirs(work, exist_ok=True)
    f = open(os.path.join(work, ".prepare.lock"), "w")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.close()
        return False
    return f


def run_with_progress(a):
    """desktop() with the protocol lines: progress on each step, then one
    XBA_RESULT. A failure still prints what it printed without --progress (on
    stderr), then the XBA_RESULT error line on stdout."""
    global _REP, _STEP, _CODE
    from tools.prepare.progress import DESKTOP_STEP_IDS, Reporter
    _REP, _STEP, _CODE = Reporter(ids=DESKTOP_STEP_IDS), 0, "INTERNAL"
    rep = _REP

    def on_signal(signum, frame):
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise Cancelled("signal %d" % signum)
    old = signal.signal(signal.SIGTERM, on_signal), signal.signal(signal.SIGINT, on_signal)
    try:
        kind, path = desktop(a)
        if kind == "exe":
            rep.ok_exe(path)
        else:
            # --no-build: no executable; the project folder instead.
            rep.ok_project(path)
        return 0
    except (Cancelled, KeyboardInterrupt) as e:
        rep.error("CANCELLED", "cancelled (%s)" % (e.args[0] if e.args else "signal"))
        return 130
    except CommandFailed as e:
        print(e.code, file=sys.stderr, flush=True)
        rep.error(_CODE, e.short())
        return 1
    except SystemExit as e:
        if e.code is None or isinstance(e.code, int):
            raise
        print(e.code, file=sys.stderr, flush=True)
        rep.error(_CODE, str(e.code).splitlines()[0])
        return 1
    except OSError as e:
        traceback.print_exc()
        rep.error("NO_SPACE" if e.errno == errno.ENOSPC else "INTERNAL", str(e))
        return 1
    except Exception as e:
        traceback.print_exc()
        from tools.xiso.xdvdfs import XisoError
        if isinstance(e, XisoError):        # an unreadable image: the step's code (NO_XBE in step 1)
            rep.error(_CODE, str(e))
        else:                               # a bug
            rep.error("INTERNAL", "%s: %s" % (type(e).__name__, e))
        return 1
    finally:
        signal.signal(signal.SIGTERM, old[0])
        signal.signal(signal.SIGINT, old[1])
        _REP = None


def desktop(a):
    """The five desktop steps. ("exe", path) when built, ("project", folder)
    with --no-build; SystemExit on a failure."""
    global _CODE
    py = sys.executable
    src = os.path.abspath(a.source)
    tmp_work = a.work or os.path.join(os.path.dirname(src), "xboxrecomp-work")
    os.makedirs(tmp_work, exist_ok=True)

    step(1, "finding default.xbe")
    xbe, data_dir, image = find_xbe(src, tmp_work)
    _CODE = "BAD_XBE"
    if a.game_dir:
        data_dir = os.path.abspath(a.game_dir)
    tmp_json = os.path.join(tmp_work, "xbe_info.json")
    log = os.path.join(tmp_work, "prepare.log")
    run([py, "-m", "tools.xbe_parser", xbe, "--json", tmp_json], log)
    with open(tmp_json) as f:
        info = json.load(f)
    tid = info["title_id"][2:].upper()
    work = a.work or os.path.join(os.path.dirname(src), "%s-work" % tid)
    if work != tmp_work:
        merge_tree(tmp_work, work)
        if image:
            xbe = os.path.join(work, "xbe", "default.xbe")
    log = os.path.join(work, "prepare.log")
    print("      %s (title ID %s), work folder %s" % (info["title"], tid, work), flush=True)
    progress(30, "%s (title ID %s)" % (info["title"], tid))
    ident = identify_xbe(xbe, work)
    if ident:
        print("      built with: %s" % ident["summary"], flush=True)
    from tools.prepare import pack, profiles
    try:
        profile = load_profile(tid, ident)
        env = pack.env_force_of(profile)
    except ValueError as e:
        _CODE = "BAD_PROFILE"
        raise SystemExit("profiles: %s" % e)
    for line in profiles.xdk_report(ident, PROFILES):
        print("      " + line, flush=True)
    en = profiles.title_line(tid, PROFILES, left_out=no_title_layer())
    if en:
        print("      " + en, flush=True)
    if profile:
        print("      profile layers: %s" % " ".join(profile["keys"]), flush=True)
        for k in profile["missing"]:
            print("      %s: asked for by the title profile, not selected by the XBE" % k, flush=True)
        if profile["title"] and profile.get("status"):
            print("      title profile: %s" % profile["status"], flush=True)
        for k, v in env.items():
            print("      the title forces %s=%s" % (k, v), flush=True)
    else:
        print("      no profile layer for this XBE: the generic template only, on the toolkit's defaults "
              "(the normal case)", flush=True)
    # The player's mods (docs/technical/mods.md): their patches go to a copy of
    # the XBE in the work folder, which is what gets analysed and lifted (the
    # original is only read); the options they force go with the layers'.
    from tools.mods.package import ModError
    try:
        mods = mods_plan(tid, xbe)
        xbe_source_sha = mods.original_sha256
        if mods.patched:
            xbe = write_patched(os.path.join(work, "mods", "default.xbe"), mods.data)
        env = dict(env, **mods.env(xbe if mods.patched else None))
    except ModError as e:
        _CODE = "BAD_MOD"
        raise SystemExit("mods: %s" % e)
    if mods.mods or mods_store_has(tid):
        print("      " + mods.summary() + (" (left out: Enhancements off)" if no_title_layer() else ""),
              flush=True)
    progress(100, "%s (title ID %s)%s" % ((profile or {}).get("name") or info["title"], tid,
                                          ", title profile" if profile and profile["title"] else ""))

    stamp_path = os.path.join(work, "stamp.json")
    stamp = {} if a.force else read_stamp(stamp_path)
    an = os.path.join(work, "analysis")
    xjson = os.path.splitext(xbe)[0] + "_analysis.json"
    # What earlier runs found (discovered.jsonl), gated into seeds (XBA-97).
    from tools.prepare import discovered
    disc_seeds = None
    try:
        disc_seeds = discovered.refresh(work, xbe, os.path.join(an, "disasm", "functions.json"),
                                        xjson if os.path.isfile(xjson) else None,
                                        log=lambda m: print(m, flush=True))
    except Exception as e:          # never let a bad discovery file stop a prepare
        print("      discovered.jsonl not used: %s" % e, flush=True)
    # The profile counts too: its seeds go to the disassembler, its
    # exclude_manual to the lift; and the seeds earlier runs found.
    # The static discovery switches too (XBA-137): "" for the default.
    from tools.prepare import toolkit
    want = {"xbe": sha256(xbe), "toolkit_sha256": toolkit.digest(), "profile": profile_digest(profile),
            "discovered": discovered.digest(disc_seeds) + discovered.mode_suffix()}
    # A stamp from before XBA-97 has no "discovered": the same as none. The
    # toolkit by its content digest, an old stamp by its commit (XBA-134).
    same = toolkit_same(stamp) and all(stamp.get(k, "" if k == "discovered" else None) == v
                                       for k, v in want.items() if k != "toolkit_sha256")
    want["toolkit"] = toolkit_version()     # recorded, not compared
    # The game's own XBE and the mod set, for --check (the stamp's "xbe" is the
    # one lifted: the patched copy when a mod patches it).
    want["xbe_source"], want["mods"] = xbe_source_sha, mods.mod_set

    step(2, "analysing the executable (a few minutes the first time)")
    if same and stamp.get("analysis"):
        note("up to date", 100)
    else:
        progress(2, "xbe_parser")
        run([py, "-m", "tools.xbe_parser", xbe, "--json", xjson], log)
        cmd = [py, "-m", "tools.disasm", xbe, "-o", os.path.join(an, "disasm"), "--force"]
        # Function starts the analysis cannot find and a run did ("seed_functions":
        # files in the profile layers, tools.seed_from_log's format).
        for fn in (profile or {}).get("seed_functions", []):
            cmd += ["--seed-functions", fn]
        if disc_seeds:
            cmd += ["--seed-functions", disc_seeds]
        progress(5, "disassembling")
        t = run(cmd, log, tick=(5, 58, "disassembling", 90))
        counts = discovered.counts_line(discovered.analysis_counts(an))
        note("disassembled in %.0f s%s" % (t, ": " + counts if counts else ""), 60)
        progress(60, "identifying library functions")
        run([py, "-m", "tools.func_id", xbe,
             "--functions", os.path.join(an, "disasm", "functions.json"),
             "--strings", os.path.join(an, "disasm", "strings.json"),
             "--xrefs", os.path.join(an, "disasm", "xrefs.json"),
             "-o", os.path.join(an, "func_id")], log,
            tick=(60, 78, "identifying library functions", 30))
        progress(80, "ABI analysis")
        run([py, "-m", "tools.abi_analysis", xbe, "--disasm-dir", os.path.join(an, "disasm"),
             "--func-id-dir", os.path.join(an, "func_id"),
             "--output-dir", os.path.join(an, "abi")], log,
            tick=(80, 98, "ABI analysis", 30))
        stamp = dict(want, analysis=True)
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)
        progress(100, "analysis done")
    if stamp.get("mods", "") != mods.mod_set or stamp.get("xbe_source") != xbe_source_sha:
        stamp["mods"], stamp["xbe_source"] = mods.mod_set, xbe_source_sha     # display-only mods: same lift
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)

    step(3, "assembling the title project")
    proj = os.path.join(work, "project")
    name = assemble_project(proj, profile, info, env)
    progress(100, "project %s" % name)

    step(4, "lifting the code to C (a few minutes the first time)")
    gen = os.path.join(proj, "src", "recomp", "gen")
    # The XDK's XMV player API, found in the XBE and bound to the runtime's
    # decoder when it is sure (XBA-199, docs/runtime/xmv.md): the lift wraps it.
    from tools.xmv import find_api
    xmv_bind = find_api.prepare_bind(xbe, an, log=note)
    xmv_key = find_api.bind_key(xmv_bind)
    if same and stamp.get("lift") and os.listdir(gen) and stamp.get("xmv_bind", "") == xmv_key:
        note("up to date", 100)
    else:
        # Lift into a fresh folder, then keep the files that did not change
        # (XBA-65): a re-prepare after a few new seeds rebuilds a few files,
        # not all of them.
        fresh = gen + ".new"
        shutil.rmtree(fresh, ignore_errors=True)
        os.makedirs(fresh)
        from tools.prepare.compile import LIFT_CHUNK_BYTES
        cmd = [py, "-m", "tools.recomp", xbe, "--all", "--split-bytes", str(LIFT_CHUNK_BYTES),
               "--game-name", info["title"],
               "--disasm-dir", os.path.join(an, "disasm"),
               "--func-id-dir", os.path.join(an, "func_id"),
               "--abi-dir", os.path.join(an, "abi"), "--gen-dir", fresh]
        for fn in (profile or {}).get("exclude_manual", []):
            cmd += ["--exclude-manual", os.path.join(proj, "src", "title", fn)]
        if xmv_bind:
            cmd += ["--xmv-bind", xmv_bind]
        progress(2, "lifting")
        log_at = os.path.getsize(log) if os.path.exists(log) else 0
        t = run(cmd, log, tick=(2, 98, "lifting", 100))
        kept, written, removed = sync_generated(fresh, gen)
        cut = lift_cut_bodies(log, log_at)
        if cut:
            note("cut bodies (the analysis stopped them short; reaching one stops the title): " + cut)
        # The counts a library check ranks titles by (XBA-139): kept in the
        # stamp, reported by --check --json.
        health = lift_health(log, log_at)
        words = health_line(health)
        if words:
            note("lift health: " + words)
        note("%d C files in %.0f s (%d changed, %d unchanged, %d removed)"
             % (len(os.listdir(gen)), t, written, kept, removed), 100)
        # The executable no longer matches the lift until a build succeeds (--check).
        stamp["lift"], stamp["built"] = True, False
        stamp["xmv_bind"] = xmv_key
        if health is not None:
            stamp["lift_health"] = health
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)

    if a.no_build:
        print("done: %s (not built)" % proj)
        return "project", proj

    step(5, "building for this computer")
    build = os.path.join(proj, "build")
    work_files = {
        # What a run meets that the analysis missed: the next prepare reads it.
        "XBA_DISCOVERY_FILE": os.path.join(work, DISCOVERY_FILE).replace("\\", "/"),
        # Where a launch of another image of the disc finds its executable (XBA-82).
        "XBA_IMAGE_MAP": (a.image_map or os.path.join(work, IMAGE_MAP)).replace("\\", "/"),
        # Which image this is, when not default.xbe (RECOMP_IMAGE: XeImageFileName).
        "XBA_IMAGE_NAME": a.image_name or ""}
    from tools.prepare import desktop_build
    from tools.prepare import compile as cc
    try:
        how, man, why = desktop_build.route(ROOT, getattr(a, "builder", None))
    except cc.StepError as e:
        raise SystemExit("%s: %s" % (e.code, e.msg))
    if how == "driver":
        exe = build_with_driver(a, proj, name, man, why, work_files, log)
    else:
        if why != "CMake (asked for)":
            note("building with " + why)
        gen_args = ["-G", "Ninja"] if shutil.which("ninja") else []
        run(["cmake", "-S", proj, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DXBOXRECOMP_DIR=" + ROOT]
            + ["-D%s=%s" % kv for kv in work_files.items()] + gen_args, log)
        progress(5, "compiling")
        t = run(["cmake", "--build", build] + desktop_build.rt.cmake_parallel(), log, on_line=BuildProgress() if _REP else None)
        note("built in %.0f s" % t, 90)
        exe = os.path.join(build, desktop_build.exe_name(name, desktop_build.rt.host_id()))
    if stamp.get("built") is False or stamp.get("runtime_sha256") != toolkit.runtime_digest():
        stamp["built"] = True
        stamp["runtime_sha256"] = toolkit.runtime_digest()     # what --check compares (XBA-134)
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)
    if image and not a.no_data:
        if a.extract:
            data_dir = os.path.join(work, "game")
            unpack_image(image, data_dir)
        else:
            # Read in place (XBA-126): build/game links to the image, the
            # runtime mounts it (src/kernel/kernel_disc.c); nothing is copied.
            data_dir = image
            print("      the game data is read from the disc image in place: %s" % image, flush=True)
    link = os.path.join(build, "game")
    if data_dir:
        link_game_data(data_dir, link)
    if not a.image_map and data_dir:
        progress(98, "the disc's other images")
        prepare_other_images(a, py, xbe, data_dir, work, exe, log)
    print("\nReady: %s\n  game data: %s" % (exe, data_dir or "(none: put the disc's files in %s)" % link))
    progress(100, "ready")
    return "exe", exe


IMAGE_MAP = "images.map"


def _is_junction(path):
    isj = getattr(os.path, "isjunction", None)          # Python 3.12+
    return bool(isj and isj(path))


def link_game_data(data_dir, link):
    """build/game -> the game's data (a folder, or a disc image read in place).
    A symbolic link; on Windows, where one needs Developer Mode or an
    administrator, a directory junction for a folder (XBA-89) or a hard link
    for an image on the same drive."""
    if _is_junction(link):
        os.rmdir(link)
    elif os.path.islink(link) or os.path.isfile(link):
        os.remove(link)
    if os.path.exists(link):
        return
    try:
        os.symlink(data_dir, link, target_is_directory=os.path.isdir(data_dir))
        return
    except OSError as e:
        if os.name != "nt":
            raise
        why = e
    try:
        if os.path.isdir(data_dir):
            import _winapi
            _winapi.CreateJunction(os.path.abspath(data_dir), link)
        else:
            os.link(data_dir, link)
    except OSError as e:
        raise SystemExit("cannot link %s to %s (%s; then %s): turn on Developer Mode in Windows' settings, or "
                         "extract the disc (--extract)" % (link, data_dir, why, e))


def build_with_driver(a, proj, name, man, why, work_files, log):
    """Step 5 without CMake (XBA-86, tools/prepare/desktop_build.py): the
    project's C by the compile driver, linked against the toolkit's prebuilt
    runtime. The executable's path; SystemExit on a failure."""
    from tools.prepare import compile as cc
    from tools.prepare import desktop_build
    procs = cc.Procs()
    t0 = time.time()
    try:
        tc = desktop_build.find_toolchain(ROOT, cc_path=a.cc, procs=procs)
        note("compile driver: %s; %s" % (tc.describe(), why))
        progress(5, "compiling")

        def prog(done, total, now, eta):
            progress(5 + 85 * done // max(1, total), "compiling %d/%d" % (done, total))
        exe, n, linked = desktop_build.build(proj, name, man, tc, ROOT, work_files, opt=a.opt, jobs=a.jobs,
                                             log=log, on_progress=prog, include_dirs=a.include_dir,
                                             cpu_arg=getattr(a, "cpu_features", "auto"))
    except cc.StepError as e:
        raise SystemExit("%s: %s (see %s)" % (e.code, e.msg, log))
    except BaseException:           # a cancel or a crash: no compile left running
        procs.cancel()
        raise
    note("built in %.0f s (%d compiled, %s)" % (time.time() - t0, n, "linked" if linked else "up to date"), 90)
    return exe


def check_state(source, work=None):
    """--check: {"state": "absent" | "stale" | "ready", "reasons": [...],
    "exe", "title", "title_id", "env_force", "work", and "title_layer": "on" |
    "off" when the title has optional enhancements (mods on), and "lift_health":
    {seh_lost, unresolved_calls, unimplemented, misdecoded, cut_bodies} when the
    last lift reported them (XBA-139)} for SOURCE, from what
    a prepare would compare (the stamp: XBE, toolkit commit, profile layers,
    mod set, seeds from earlier runs) and what it built. Reads only: nothing is
    written, not even the work folder. identify.IdentifyError when SOURCE is
    not usable; profiles.ProfileError / ValueError for a bad profile."""
    import tempfile
    from tools.prepare import identify, pack
    from tools.compat.__main__ import project_exe
    src = os.path.abspath(source)
    kind, xbe, data = identify.read_source(src)
    x = identify.parse_xbe(data)
    tid = "%08X" % x.certificate.title_id
    work = os.path.abspath(work or os.path.join(os.path.dirname(src), "%s-work" % tid))
    tmp = None
    if xbe is None:             # an image: the copy a prepare made, else a temporary one
        mine = os.path.join(work, "xbe", "default.xbe")
        if os.path.isfile(mine):
            xbe = mine
            with open(mine, "rb") as f:
                data = f.read()
        else:
            tmp = tempfile.mkdtemp(prefix="xba-check-")
            xbe = os.path.join(tmp, "default.xbe")
            with open(xbe, "wb") as f:
                f.write(data)
    try:
        from tools import xbe_ident
        try:
            ident = xbe_ident.identify(xbe)
        except Exception:
            ident = None
        profile = load_profile(tid, ident)
        env = pack.env_force_of(profile)
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    out = {"state": "absent", "reasons": [], "exe": project_exe(work),
           "title": (profile or {}).get("name") or x.certificate.title_name.strip() or tid,
           "title_id": tid, "env_force": env, "work": work}
    tl = title_layer_state(tid)
    if tl:                      # the toolkit has optional enhancements for it (WP6)
        out["title_layer"] = tl
    stamp = read_stamp(os.path.join(work, "stamp.json"))
    reasons = []
    if stamp and isinstance(stamp.get("lift_health"), dict):
        out["lift_health"] = stamp["lift_health"]   # what the last lift could not reach (XBA-139)
    if stamp:
        # A stamp made with mods holds the game's own XBE in "xbe_source" ("xbe"
        # is the patched copy it lifted) and the mod set in "mods".
        if stamp.get("xbe_source", stamp.get("xbe")) != hashlib.sha256(data).hexdigest():
            reasons.append("xbe")
        if stamp.get("mods", "") != check_mod_set(tid):
            reasons.append("mods")
        if not toolkit_same(stamp):
            reasons.append("toolkit")
        elif stamp.get("built") is not False and not runtime_same(stamp):
            reasons.append("runtime")       # a rebuild only: the lift stands
        if stamp.get("profile") != profile_digest(profile):
            reasons.append("profile")
        if discovered_changed(work, stamp):
            reasons.append("discovered")
        if not (stamp.get("analysis") and stamp.get("lift")) or stamp.get("built") is False:
            reasons.append("not_built")
    elif out["exe"]:
        reasons.append("no_stamp")
    if out["exe"]:
        out["state"] = "stale" if reasons else "ready"
        out["reasons"] = reasons
    elif stamp:
        out["reasons"] = ["not_built"]
    return out


def check_mod_set(tid):
    """The mod-set hash a prepare would use now ("" for none; "?" when the store
    says something a prepare would refuse: it is then stale, and the prepare
    gives the reason)."""
    if no_title_layer():
        return ""
    from tools.mods import store
    from tools.mods.package import ModError
    try:
        return store.mod_set_hash(store.enabled_mods(store.default_root(), "xbox", tid))
    except ModError:
        return "?"


def discovered_changed(work, stamp):
    """True when the next prepare would analyse again for what runs found:
    the seeds file differs from the stamp's, or discovered.jsonl holds
    addresses neither seeded, refused nor held yet (they may become seeds),
    or a held one a healthy run has since reported (profile-free WP1)."""
    from tools.prepare import discovered
    seeds_path = os.path.join(work, discovered.SEEDS)
    if (discovered.digest(seeds_path if os.path.isfile(seeds_path) else None) + discovered.mode_suffix()
            != stamp.get("discovered", "")):
        return True
    return discovered.changed(work)


def check(a):
    """--check [--json]: exit 0 with the state; 1 when the source or its
    profile is not usable ({"state": "error", "code", "msg"} with --json)."""
    from tools.prepare import identify
    try:
        out = check_state(a.source, a.work)
    except identify.IdentifyError as e:
        out = {"state": "error", "code": e.code, "msg": e.msg}
    except ValueError as e:         # profiles.ProfileError, pack.env_force_of
        out = {"state": "error", "code": "BAD_PROFILE", "msg": "profiles: %s" % e}
    if a.json:
        print(json.dumps(out))
    elif out["state"] == "error":
        print("%s: %s" % (out["code"], out["msg"]), file=sys.stderr)
    else:
        print("%s %s (%s): %s%s" % (out["title_id"], out["title"], out["work"], out["state"],
                                    " (%s)" % ", ".join(out["reasons"]) if out["reasons"] else ""))
        if out["exe"]:
            print("  %s" % out["exe"])
    return 1 if out["state"] == "error" else 0


def prepare_other_images(a, py, xbe, data_dir, work, exe, log):
    """XBA-82 (desktop): the disc's other images that default.xbe names
    (tools.triage's test, e.g. "d:\\game.xbe") are prepared too, each into
    <work>/images/<path>/, and <work>/images.map lists every image's
    executable; a launch of one execs it in the same process (the kernel's
    xbox_launch_image, RECOMP_IMAGE_MAP)."""
    import re
    from tools.triage import _named_xbes, disc_xbes
    with open(xbe, "rb") as f:
        named = _named_xbes(f.read())
    # From a folder or straight from a disc image (XBA-126).
    others = [(rel, p) for rel, p in disc_xbes(data_dir, work, named) if rel.lower() != "default.xbe"]
    if not others:
        return
    map_path = os.path.join(work, IMAGE_MAP)
    lines = ["default.xbe\t%s" % exe]
    for rel, p in others:
        sub = os.path.join(work, "images", re.sub(r"[^A-Za-z0-9._-]", "_", rel))
        print("      another image the title launches: %s" % rel, flush=True)
        cmd = [py, "-m", "tools.prepare", p, "--work", sub, "--image-map", map_path, "--game-dir", data_dir,
               "--image-name", rel.replace(os.sep, "/")]
        if a.force:
            cmd.append("--force")
        for opt, v in (("--builder", getattr(a, "builder", None)), ("--cc", a.cc)):
            if v:
                cmd += [opt, v]
        run(cmd, log)
        from tools.compat.__main__ import project_exe
        sub_exe = project_exe(sub)
        if sub_exe:
            lines.append("%s\t%s" % (rel, sub_exe))
    with open(map_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("      %d image(s) in %s" % (len(lines), map_path), flush=True)


if __name__ == "__main__":
    sys.exit(main())
