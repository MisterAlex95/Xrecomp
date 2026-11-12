"""
Prepare an Xbox title from your own copy of the game.

    python -m tools.prepare SOURCE [--work DIR] [--no-build] [--no-data]

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
PROFILES = os.path.join(ROOT, "profiles")
DISCOVERY_FILE = "discovered.jsonl"     # in the work folder: src/kernel/recomp_discovery.h
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
    ident = identify_xbe(xbe, work)
    if ident:
        print("      built with: %s" % ident["summary"], flush=True)
    from tools.prepare import pack
    try:
        profile = load_profile(tid, ident)
        env = pack.env_force_of(profile)
    except ValueError as e:
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
        print("      up to date", flush=True)
    else:
        run([py, "-m", "tools.xbe_parser", xbe, "--json", xjson], log)
        cmd = [py, "-m", "tools.disasm", xbe, "-o", os.path.join(an, "disasm"), "--force"]
        # Function starts the analysis cannot find and a run did ("seed_functions":
        # files in the profile layers, tools.seed_from_log's format).
        for fn in (profile or {}).get("seed_functions", []):
            cmd += ["--seed-functions", fn]
        if disc_seeds:
            cmd += ["--seed-functions", disc_seeds]
        t = run(cmd, log)
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
        for fn in (profile or {}).get("exclude_manual", []):
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
         "-DXBOXRECOMP_DIR=" + ROOT,
         # What a run meets that the analysis missed: the next prepare reads it.
         "-DXBA_DISCOVERY_FILE=" + os.path.join(work, DISCOVERY_FILE).replace("\\", "/")] + gen_args, log)
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
