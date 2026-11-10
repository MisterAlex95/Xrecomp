"""
Prepare an Xbox title from your own copy of the game.

    python -m tools.prepare SOURCE [--work DIR] [--no-build] [--no-data]

SOURCE is a disc image (.iso / .xiso), a folder holding default.xbe (the
extracted disc), or a default.xbe. Steps, each skipped when its output is
already there for this XBE and this toolkit:

  1. find default.xbe (out of the image if needed) and read its title ID
  2. the analysis pipeline: xbe_parser, disasm, func_id, abi_analysis
  3. the title project: templates/new-game + the title's profile from
     titles/<title id>/ when there is one (its switches and fixes)
  4. the C lift (tools.recomp) into the project
  5. build it for this host (CMake), with the game data next to it

The work folder (default: <title id>-work next to SOURCE) holds everything it
writes; nothing is written into the toolkit. Nothing from the game is ever
uploaded or shared: the output is for your own machines.

With --android it makes a title pack for the Android app instead (libtitle.so
+ pack.json, built with clang and ld.lld, no CMake), with machine-readable
progress lines: docs/android/prepare.md.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEMPLATE = os.path.join(ROOT, "templates", "new-game")
TITLES = os.path.join(ROOT, "titles")
STEPS = 5


def step(n, msg):
    print("[%d/%d] %s" % (n, STEPS, msg), flush=True)


def run(args, log, cwd=ROOT):
    """Run a command, its output appended to `log`; raise with the tail on failure."""
    t0 = time.time()
    with open(log, "a") as f:
        f.write("\n$ %s\n" % " ".join(args))
        f.flush()
        rc = subprocess.call(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if rc != 0:
        with open(log) as f:
            tail = f.read()[-3000:]
        raise SystemExit("command failed (%d): %s\n--- %s (end) ---\n%s"
                         % (rc, " ".join(args), log, tail))
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
                last = pct


def read_stamp(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def assemble_project(proj, profile, info):
    """templates/new-game + the profile's .c files in src/title/."""
    os.makedirs(os.path.join(proj, "src", "recomp", "gen"), exist_ok=True)
    name = (profile or {}).get("project") or "title_%s" % info["title_id"][2:].lower()
    with open(os.path.join(TEMPLATE, "CMakeLists.txt")) as f:
        cm = f.read().replace("project(your_game_recomp C)", "project(%s C)" % name)
    with open(os.path.join(proj, "CMakeLists.txt"), "w") as f:
        f.write(cm)
    with open(os.path.join(TEMPLATE, "src", "main.c")) as f:
        main = f.read()
    for old, new in (('"game\\\\Your Game Title\\\\default.xbe"', '"game/default.xbe"'),
                     ('"game\\\\Your Game Title"', '"game"')):
        main = main.replace(old, new)
    with open(os.path.join(proj, "src", "main.c"), "w") as f:
        f.write(main)
    shutil.copy(os.path.join(TEMPLATE, "src", "recomp_manual.c"),
                os.path.join(proj, "src", "recomp_manual.c"))
    tdir = os.path.join(proj, "src", "title")
    shutil.rmtree(tdir, ignore_errors=True)
    if profile:
        os.makedirs(tdir)
        for fn in sorted(os.listdir(profile["dir"])):
            if fn.endswith(".c"):
                shutil.copy(os.path.join(profile["dir"], fn), os.path.join(tdir, fn))
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
    a = ap.parse_args(argv)
    if a.android:
        from tools.prepare import android
        return android.run(a, sys.modules[__name__])
    py = sys.executable
    src = os.path.abspath(a.source)
    tmp_work = a.work or os.path.join(os.path.dirname(src), "xboxrecomp-work")
    os.makedirs(tmp_work, exist_ok=True)

    step(1, "finding default.xbe")
    xbe, data_dir, image = find_xbe(src, tmp_work)
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
    pdir = os.path.join(TITLES, tid.lower())
    profile = None
    if os.path.isfile(os.path.join(pdir, "title.json")):
        with open(os.path.join(pdir, "title.json")) as f:
            profile = json.load(f)
        profile["dir"] = pdir
        print("      title profile: titles/%s (%s)" % (tid.lower(), profile.get("status", "")), flush=True)
    else:
        print("      no title profile in titles/%s: the generic template only" % tid.lower(), flush=True)

    stamp_path = os.path.join(work, "stamp.json")
    stamp = {} if a.force else read_stamp(stamp_path)
    want = {"xbe": sha256(xbe), "toolkit": toolkit_version()}
    same = stamp.get("xbe") == want["xbe"] and stamp.get("toolkit") == want["toolkit"]
    an = os.path.join(work, "analysis")
    xjson = os.path.splitext(xbe)[0] + "_analysis.json"

    step(2, "analysing the executable (a few minutes the first time)")
    if same and stamp.get("analysis"):
        print("      up to date", flush=True)
    else:
        run([py, "-m", "tools.xbe_parser", xbe, "--json", xjson], log)
        t = run([py, "-m", "tools.disasm", xbe, "-o", os.path.join(an, "disasm"), "--force"], log)
        print("      disassembled in %.0f s" % t, flush=True)
        run([py, "-m", "tools.func_id", xbe,
             "--functions", os.path.join(an, "disasm", "functions.json"),
             "--strings", os.path.join(an, "disasm", "strings.json"),
             "--xrefs", os.path.join(an, "disasm", "xrefs.json"),
             "-o", os.path.join(an, "func_id")], log)
        run([py, "-m", "tools.abi_analysis", xbe, "--disasm-dir", os.path.join(an, "disasm"),
             "--func-id-dir", os.path.join(an, "func_id"),
             "--output-dir", os.path.join(an, "abi")], log)
        stamp = dict(want, analysis=True)
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)

    step(3, "assembling the title project")
    proj = os.path.join(work, "project")
    name = assemble_project(proj, profile, info)

    step(4, "lifting the code to C (a few minutes the first time)")
    gen = os.path.join(proj, "src", "recomp", "gen")
    if same and stamp.get("lift") and os.listdir(gen):
        print("      up to date", flush=True)
    else:
        for fn in os.listdir(gen):
            os.remove(os.path.join(gen, fn))
        cmd = [py, "-m", "tools.recomp", xbe, "--all", "--split", "250",
               "--game-name", info["title"],
               "--disasm-dir", os.path.join(an, "disasm"),
               "--func-id-dir", os.path.join(an, "func_id"),
               "--abi-dir", os.path.join(an, "abi"), "--gen-dir", gen]
        for fn in (profile or {}).get("exclude_manual", [])[:1]:
            cmd += ["--exclude-manual", os.path.join(proj, "src", "title", fn)]
        t = run(cmd, log)
        print("      %d C files in %.0f s" % (len(os.listdir(gen)), t), flush=True)
        stamp["lift"] = True
        with open(stamp_path, "w") as f:
            json.dump(stamp, f)

    if a.no_build:
        print("done: %s (not built)" % proj)
        return 0

    step(5, "building for this computer")
    build = os.path.join(proj, "build")
    gen_args = ["-G", "Ninja"] if shutil.which("ninja") else []
    run(["cmake", "-S", proj, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DXBOXRECOMP_DIR=" + ROOT] + gen_args, log)
    t = run(["cmake", "--build", build, "--parallel"], log)
    print("      built in %.0f s" % t, flush=True)
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
    print("\nReady: %s\n  game data: %s" % (exe, data_dir or "(none: put the disc's files in %s)" % link))
    return 0


if __name__ == "__main__":
    sys.exit(main())
