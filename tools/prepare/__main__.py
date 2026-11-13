"""
Prepare an Xbox title from your own copy of the game.

    python -m tools.prepare SOURCE [--work DIR] [--no-build] [--no-data] [--progress]
    python -m tools.prepare SOURCE [--work DIR] --check [--json]

SOURCE is a disc image (.iso / .xiso), a folder holding default.xbe (the
extracted disc), or a default.xbe. Steps, each skipped when its output is
already there for this XBE, this toolkit and this title profile:

  1. find default.xbe (out of the image if needed) and read its title ID
  2. the analysis pipeline: xbe_parser, disasm, func_id, abi_analysis
  3. the title project: templates/new-game + the profile layers this XBE
     picks from profiles/ (XDK libraries, engine, title: their switches and
     fixes, docs/technical/profiles.md; their seed_functions files already
     went to the disassembler in step 2)
  4. the C lift (tools.recomp) into the project
  5. build it for this host (CMake), with the game data next to it

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
    time, so a launcher does not look stuck on "disassembling"."""
    t0 = time.time()
    with open(log, "a") as f:
        f.write("\n$ %s\n" % " ".join(args))
        f.flush()
        if on_line is None:
            p = subprocess.Popen(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
        else:
            p = subprocess.Popen(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 universal_newlines=True, errors="replace", bufsize=1)
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
        except BaseException:       # as subprocess.call: never leave the child running
            p.kill()
            p.wait()
            raise
    if rc != 0:
        with open(log) as f:
            tail = f.read()[-3000:]
        raise CommandFailed(rc, args, log, tail)
    return time.time() - t0


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


def find_xbe(source, work):
    """(path to default.xbe, game data folder or None, image or None)."""
    src = os.path.abspath(source)
    if os.path.isdir(src):
        xbe = os.path.join(src, "default.xbe")
        if not os.path.isfile(xbe):
            raise SystemExit("%s: no default.xbe in this folder" % src)
        return xbe, src, None
    low = src.lower()
    if low.endswith(".xbe"):
        return src, os.path.dirname(src), None
    if low.endswith((".iso", ".xiso")):
        sys.path.insert(0, ROOT)
        from tools.xiso.xdvdfs import Xiso
        dest = os.path.join(work, "xbe", "default.xbe")
        if not os.path.isfile(dest):
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with Xiso(src) as iso:
                e = iso.find("default.xbe")
                if e is None:
                    raise SystemExit("%s: no default.xbe in the image" % src)
                iso.extract(e, dest)
        return dest, None, src
    raise SystemExit("%s: expected a .iso/.xiso, a folder with default.xbe, or default.xbe" % src)


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
    return profiles.resolve(tid, ident, PROFILES)


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
    with open(os.path.join(work, "ident.json"), "w", encoding="utf-8") as f:
        json.dump(ident, f, indent=1)
    return ident


def assemble_project(proj, profile, info):
    """templates/new-game + the profile layers in src/title/: each layer's .c
    files (profiles.c_name), xba_layers.c (the hooks main.c calls),
    xba_params.h, and env_force.h when the layers force options. main.c gets
    the title's name and entry point in place of the template's placeholders."""
    from tools.prepare import profiles
    from tools.prepare import pack
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
            with open(os.path.join(tdir, "env_force.h"), "w") as f:
                f.write(env_force_header(info["title_id"][2:], env))
    return name


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.prepare", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help=".iso/.xiso, a folder with default.xbe, or default.xbe")
    ap.add_argument("--work", help="work folder (default: <title id>-work next to SOURCE)")
    ap.add_argument("--no-build", action="store_true", help="stop after the C lift")
    ap.add_argument("--no-data", action="store_true",
                    help="do not extract the disc image for the desktop build")
    ap.add_argument("--force", action="store_true", help="redo every step")
    mi = ap.add_argument_group("for a launcher (docs/macos/launcher.md « Prepare interface »)")
    mi.add_argument("--progress", action="store_true",
                    help="also print XBA_PROGRESS / XBA_RESULT lines (tools/prepare/progress.py)")
    mi.add_argument("--check", action="store_true",
                    help="say whether the title is prepared and up to date (absent, stale, ready); writes nothing")
    mi.add_argument("--json", action="store_true", help="with --check: one JSON object on stdout")
    an = ap.add_argument_group("Android title pack (docs/android/prepare.md)")
    an.add_argument("--android", action="store_true", help="make a title pack for the Android app")
    an.add_argument("--pack-dir", help="packs folder: the pack goes to PACK_DIR/<TITLEID>/")
    an.add_argument("--cc", help="clang (or the multicall LLVM binary, with --cc-prefix-arg clang)")
    an.add_argument("--cc-prefix-arg", action="append", default=[], metavar="ARG",
                    help="argument(s) put right after --cc (multicall: clang)")
    an.add_argument("--ld", help="ld.lld (default: CC ld.lld for a multicall CC, else ld.lld next to CC)")
    an.add_argument("--ld-prefix-arg", action="append", default=[], metavar="ARG")
    an.add_argument("--sysroot", help="the NDK sysroot (usr/include, usr/lib/aarch64-linux-android/30)")
    an.add_argument("--builtins", help="libclang_rt.builtins-aarch64-android.a (default: asked from CC)")
    an.add_argument("--shell-lib", help="the app shell's libmain.so, which the pack links against")
    an.add_argument("--shell-abi", type=int, default=1, help="the shell ABI the app exports (default 1)")
    an.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: from free memory)")
    an.add_argument("--opt", default="2", choices=("0", "1", "2", "3", "s"), help="clang -O level (default 2)")
    an.add_argument("--python", help="Python for the pipeline steps (default: $XBA_PYTHON, else this one)")
    an.add_argument("--extra-src", action="append", default=[], metavar="FILE.c",
                    help="an extra C file compiled into the pack (src/extra/)")
    an.add_argument("--include-dir", action="append", default=[], metavar="DIR", help="an extra -I")
    mi = ap.add_argument_group("the disc's other images (XBA-82; set by the top-level prepare)")
    mi.add_argument("--image-map", help=argparse.SUPPRESS)   # the map file every image's build bakes in
    mi.add_argument("--game-dir", help=argparse.SUPPRESS)    # D:\ for an image that is not the disc root's
    mi.add_argument("--image-name", help=argparse.SUPPRESS)  # that image's disc path (XeImageFileName)
    a = ap.parse_args(argv)
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
        os.makedirs(work, exist_ok=True)
        for fn in os.listdir(tmp_work):
            shutil.move(os.path.join(tmp_work, fn), os.path.join(work, fn))
        os.rmdir(tmp_work)
        if image:
            xbe = os.path.join(work, "xbe", "default.xbe")
    log = os.path.join(work, "prepare.log")
    print("      %s (title ID %s), work folder %s" % (info["title"], tid, work), flush=True)
    progress(30, "%s (title ID %s)" % (info["title"], tid))
    ident = identify_xbe(xbe, work)
    if ident:
        print("      built with: %s" % ident["summary"], flush=True)
    from tools.prepare import pack
    try:
        profile = load_profile(tid, ident)
        env = pack.env_force_of(profile)
    except ValueError as e:
        _CODE = "BAD_PROFILE"
        raise SystemExit("profiles: %s" % e)
    if profile:
        print("      profile layers: %s" % " ".join(profile["keys"]), flush=True)
        for k in profile["missing"]:
            print("      %s: asked for by the title profile, not selected by the XBE" % k, flush=True)
        if profile["title"]:
            print("      title profile: %s" % profile.get("status", ""), flush=True)
        for k, v in env.items():
            print("      the title forces %s=%s" % (k, v), flush=True)
    else:
        print("      no profile layer for this XBE: the generic template only", flush=True)
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
    want = {"xbe": sha256(xbe), "toolkit": toolkit_version(), "profile": profile_digest(profile),
            "discovered": discovered.digest(disc_seeds)}
    # A stamp from before XBA-97 has no "discovered": the same as none.
    same = all(stamp.get(k, "" if k == "discovered" else None) == v for k, v in want.items())

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
        note("disassembled in %.0f s" % t, 60)
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

    step(3, "assembling the title project")
    proj = os.path.join(work, "project")
    name = assemble_project(proj, profile, info)
    progress(100, "project %s" % name)

    step(4, "lifting the code to C (a few minutes the first time)")
    gen = os.path.join(proj, "src", "recomp", "gen")
    if same and stamp.get("lift") and os.listdir(gen):
        note("up to date", 100)
    else:
        # Lift into a fresh folder, then keep the files that did not change
        # (XBA-65): a re-prepare after a few new seeds rebuilds a few files,
        # not all of them.
        fresh = gen + ".new"
        shutil.rmtree(fresh, ignore_errors=True)
        os.makedirs(fresh)
        cmd = [py, "-m", "tools.recomp", xbe, "--all", "--split", "250",
               "--game-name", info["title"],
               "--disasm-dir", os.path.join(an, "disasm"),
               "--func-id-dir", os.path.join(an, "func_id"),
               "--abi-dir", os.path.join(an, "abi"), "--gen-dir", fresh]
        for fn in (profile or {}).get("exclude_manual", []):
            cmd += ["--exclude-manual", os.path.join(proj, "src", "title", fn)]
        progress(2, "lifting")
        t = run(cmd, log, tick=(2, 98, "lifting", 100))
        kept, written, removed = sync_generated(fresh, gen)
        note("%d C files in %.0f s (%d changed, %d unchanged, %d removed)"
             % (len(os.listdir(gen)), t, written, kept, removed), 100)
        # The executable no longer matches the lift until a build succeeds (--check).
        stamp["lift"], stamp["built"] = True, False
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)

    if a.no_build:
        print("done: %s (not built)" % proj)
        return "project", proj

    step(5, "building for this computer")
    build = os.path.join(proj, "build")
    gen_args = ["-G", "Ninja"] if shutil.which("ninja") else []
    run(["cmake", "-S", proj, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DXBOXRECOMP_DIR=" + ROOT,
         # What a run meets that the analysis missed: the next prepare reads it.
         "-DXBA_DISCOVERY_FILE=" + os.path.join(work, DISCOVERY_FILE).replace("\\", "/"),
         # Where a launch of another image of the disc finds its executable (XBA-82).
         "-DXBA_IMAGE_MAP=" + (a.image_map or os.path.join(work, IMAGE_MAP)).replace("\\", "/"),
         # Which image this is, when not default.xbe (RECOMP_IMAGE: XeImageFileName).
         "-DXBA_IMAGE_NAME=" + (a.image_name or "")] + gen_args, log)
    progress(5, "compiling")
    t = run(["cmake", "--build", build, "--parallel"], log, on_line=BuildProgress() if _REP else None)
    note("built in %.0f s" % t, 90)
    if stamp.get("built") is False:
        stamp["built"] = True
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)
    exe = os.path.join(build, name)
    if image and not a.no_data:
        data_dir = os.path.join(work, "game")
        unpack_image(image, data_dir)
    link = os.path.join(build, "game")
    if data_dir:
        if os.path.islink(link) or os.path.exists(link):
            os.remove(link) if os.path.islink(link) else None
        if not os.path.exists(link):
            os.symlink(data_dir, link)
    if not a.image_map and data_dir:
        progress(98, "the disc's other images")
        prepare_other_images(a, py, xbe, data_dir, work, exe, log)
    print("\nReady: %s\n  game data: %s" % (exe, data_dir or "(none: put the disc's files in %s)" % link))
    progress(100, "ready")
    return "exe", exe


IMAGE_MAP = "images.map"


def check_state(source, work=None):
    """--check: {"state": "absent" | "stale" | "ready", "reasons": [...],
    "exe", "title", "title_id", "env_force", "work"} for SOURCE, from what
    a prepare would compare (the stamp: XBE, toolkit commit, profile layers,
    seeds from earlier runs) and what it built. Reads only: nothing is
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
    stamp = read_stamp(os.path.join(work, "stamp.json"))
    reasons = []
    if stamp:
        if stamp.get("xbe") != hashlib.sha256(data).hexdigest():
            reasons.append("xbe")
        if stamp.get("toolkit") != toolkit_version():
            reasons.append("toolkit")
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


def discovered_changed(work, stamp):
    """True when the next prepare would analyse again for what runs found:
    the seeds file differs from the stamp's, or discovered.jsonl holds
    addresses neither seeded nor rejected yet (they may become seeds)."""
    from tools.prepare import discovered
    seeds_path = os.path.join(work, discovered.SEEDS)
    if discovered.digest(seeds_path if os.path.isfile(seeds_path) else None) != stamp.get("discovered", ""):
        return True
    disc = os.path.join(work, discovered.DISCOVERY)
    if not os.path.isfile(disc):
        return False
    from tools.seed_from_log.__main__ import scan_discovery
    with open(disc, encoding="utf-8", errors="replace") as f:
        candidates = scan_discovery(f.read())
    seeds = discovered._read_json(seeds_path, [])
    rejected = discovered._read_json(os.path.join(work, discovered.REJECTED), [])
    known = {e["start"].lower() for e in seeds if isinstance(e, dict)} | {r.lower() for r in rejected}
    return any(("0x%08x" % va) not in known for va in candidates)


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
    from tools.triage import _named_xbes, _xbes_in_folder
    with open(xbe, "rb") as f:
        named = _named_xbes(f.read())
    others = [(rel, p) for rel, p in _xbes_in_folder(data_dir)
              if rel.lower() != "default.xbe" and os.path.basename(rel).lower() in named]
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
