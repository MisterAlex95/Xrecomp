"""
The prebuilt runtime of a desktop toolkit package (XBA-86): the Xbox runtime
libraries a title executable links, built once when the package is made, so
that `python3 -m tools.prepare` builds a title with a C compiler alone (no
CMake, no Ninja: tools/prepare/desktop_build.py).

    python3 -m tools.prepare.runtime [--toolkit DIR] [--out DIR] [--build DIR]

This is the packaging side and the one place that runs CMake: it configures
templates/new-game on an empty title against the toolkit, asks CMake's file
API (codemodel-v2) what the title executable would be compiled and linked
with, builds the libraries that link line names, and writes them with a
manifest into <toolkit>/runtime/<host>/ (default --out):

    runtime/linux-x86_64/runtime.json   what desktop_build reads
    runtime/linux-x86_64/lib/           the static libraries, in link order
    runtime/linux-x86_64/include/N/     headers CMake generated in its build dir
    runtime/linux-x86_64/bin/           the console players (gba_play, ps1_play,
                                        n64_play) that `xrecomp play` starts,
                                        from the same build (--no-players: none)

Paths in runtime.json are "{toolkit}/..." (the package's own tree),
"{runtime}/..." (this folder) or absolute system paths (SDL2, GL, ...), which
the user's machine has from tools/linux/install_deps.sh / tools/macos/setup.sh.
`runtime_sha256` is toolkit.runtime_digest() of the tree it was built from: a
checkout whose src/ changed since builds with CMake again (desktop_build.find).

Another host's runtime (XBA-87: Linux arm64 from an x86-64 machine): $XBA_HOST
names the host (linux-arm64) and the CMake toolchain file comes after --:

    XBA_HOST=linux-arm64 python3 -m tools.prepare.runtime -- \
        -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/linux-aarch64-cross.cmake

$XBA_HOST is read by tools.prepare too (with XBA_CC=aarch64-linux-gnu-gcc it
prepares a title for that host).
"""

import argparse
import glob
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FORMAT = 1
MANIFEST = "runtime.json"
PROBE = "xba_runtime_probe"
# The console players `xrecomp play` starts (tools/xrecomp find_player): built with the runtime when the toolkit
# has them (consoles/<c>/runtime, on by default on Linux and macOS), copied into bin/.
PLAYERS = ("gba_play", "ps1_play", "n64_play")


def host_id(system=None, machine=None):
    """'linux-x86_64', 'linux-arm64', 'macos-arm64', 'windows-x86_64', ...: the
    runtime folder for this host (or the one given; $XBA_HOST when none is
    given: another host's runtime, a cross build)."""
    if system is None and machine is None and os.environ.get("XBA_HOST"):
        return os.environ["XBA_HOST"]
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    system = {"darwin": "macos"}.get(system, system)
    if system.startswith(("cygwin", "msys", "mingw")):
        system = "windows"
    machine = {"amd64": "x86_64", "x64": "x86_64", "aarch64": "arm64", "arm64e": "arm64"}.get(machine, machine)
    return "%s-%s" % (system, machine)


def runtime_dir(toolkit=ROOT, host=None):
    return os.path.join(toolkit, "runtime", host or host_id())


# ── From CMake's file API to the manifest ──────────────────────

def _inside(path, top):
    try:
        return os.path.commonpath([os.path.realpath(path), os.path.realpath(top)]) == os.path.realpath(top)
    except ValueError:          # another drive (Windows)
        return False


class Relocator:
    """Rewrites the paths CMake gives (its build tree, the toolkit, the probe
    project) into the manifest's: build-tree libraries are copied into lib/,
    generated header folders into include/N/."""

    def __init__(self, toolkit, build, probe, out):
        self.toolkit, self.build, self.probe, self.out = toolkit, build, probe, out
        self.libs = {}              # source path -> "{runtime}/lib/name"
        self.incs = {}

    def path(self, p, kind):
        """kind 'lib' or 'include'. None: drop it (the probe's own folders)."""
        if not os.path.isabs(p):
            return p
        if _inside(p, self.probe) and not _inside(p, self.build):
            return None
        if _inside(p, self.build):
            return self._copy_lib(p) if kind == "lib" else self._copy_inc(p)
        if _inside(p, self.toolkit):
            return "{toolkit}/" + os.path.relpath(os.path.realpath(p),
                                                  os.path.realpath(self.toolkit)).replace(os.sep, "/")
        return p

    def _copy_lib(self, p):
        if p not in self.libs:
            name = os.path.basename(p)
            taken = {v.rsplit("/", 1)[1] for v in self.libs.values()}
            if name in taken:
                name = "%d_%s" % (len(self.libs), name)
            os.makedirs(os.path.join(self.out, "lib"), exist_ok=True)
            shutil.copy2(p, os.path.join(self.out, "lib", name))
            self.libs[p] = "{runtime}/lib/" + name
        return self.libs[p]

    def _copy_inc(self, p):
        if p not in self.incs:
            dest = os.path.join(self.out, "include", str(len(self.incs)))
            if os.path.isdir(p):
                shutil.copytree(p, dest, ignore=lambda d, names: [
                    n for n in names if not os.path.isdir(os.path.join(d, n))
                    and not n.endswith((".h", ".hpp", ".inc"))])
            else:
                os.makedirs(dest)
            self.incs[p] = "{runtime}/include/%d" % (len(self.incs))
        return self.incs[p]


def _split_fragment(frag):
    """One link fragment as arguments: CMake joins a target's flags with spaces."""
    import shlex
    try:
        return shlex.split(frag, posix=(os.name != "nt"))
    except ValueError:
        return frag.split()


# Compile flags of the probe that are not the runtime's: the driver sets its own -O, -g and standard.
_DROP_COMPILE = ("-O", "-g", "-std=", "/O", "/Z", "/std:", "/W", "-W", "-fdiagnostics-color")


# Link flags of the probe that are its compile flags again (CMake passes the configuration's flags to the link).
_DROP_LINK = ("-O", "-g", "-D", "/D", "/O1", "/O2", "/Ob", "/Ox", "/Zi")


def manifest_from_target(target, reloc, toolkit_sha, compiler, host):
    """runtime.json from the file API's reply for the probe executable."""
    group = next((g for g in target.get("compileGroups", []) if g.get("language") == "C"), {})
    defines = [d["define"] for d in group.get("defines", [])]
    includes = []
    for inc in group.get("includes", []):
        p = reloc.path(inc["path"], "include")
        if p and p not in includes:
            includes.append(p)
    cflags = []
    for frag in group.get("compileCommandFragments", []):
        for arg in _split_fragment(frag["fragment"]):
            if not arg.startswith(_DROP_COMPILE):
                cflags.append(arg)
    link = target.get("link", {})
    largs = []
    for frag in link.get("commandFragments", []):
        role = frag.get("role")
        for arg in _split_fragment(frag["fragment"]):
            if role == "flags" and arg.startswith(_DROP_LINK):
                continue
            if role == "libraries" and not arg.startswith("-"):
                full = arg if os.path.isabs(arg) else os.path.join(reloc.build, arg)
                if os.path.exists(full):
                    arg = reloc.path(full, "lib")
            elif arg.startswith("-Wl,-rpath,"):
                kept = [p for p in arg[len("-Wl,-rpath,"):].split(":") if not _inside(p, reloc.build)]
                if not kept:
                    continue
                arg = "-Wl,-rpath," + ":".join(kept)
            if arg:
                largs.append({"arg": arg, "role": role})
    return {
        "format": FORMAT,
        "host": host,
        "runtime_sha256": toolkit_sha,
        "compiler": compiler,
        "defines": defines,
        "includes": includes,
        "compile_flags": cflags,
        "link_language": link.get("language", "C"),
        "link": largs,
    }


# ── The build ───────────────────────────────────────────────────

def _run(args, log, cwd=None):
    print("$ " + " ".join(args), flush=True)
    with open(log, "a") as f:
        f.write("\n$ %s\n" % " ".join(args))
        f.flush()
        rc = subprocess.call(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if rc != 0:
        with open(log) as f:
            sys.stderr.write(f.read()[-4000:])
        raise SystemExit("%s failed (exit %d), see %s" % (os.path.basename(args[0]), rc, log))


def _reply(build):
    """(codemodel, reply dir) from the file API reply in build."""
    reply = os.path.join(build, ".cmake", "api", "v1", "reply")
    index = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not index:
        raise SystemExit("CMake wrote no file API reply in %s (CMake 3.14 or later is needed)" % reply)
    with open(index[-1]) as f:
        idx = json.load(f)
    cm = next(o for o in idx["objects"] if o["kind"] == "codemodel")
    with open(os.path.join(reply, cm["jsonFile"])) as f:
        return json.load(f), reply, idx


def _artifact(target_json, build):
    """The first artifact (the executable) of a file API target, absolute."""
    arts = target_json.get("artifacts") or []
    if not arts:
        return None
    p = arts[0]["path"]
    return p if os.path.isabs(p) else os.path.join(build, p)


def build(toolkit=ROOT, out=None, build_dir=None, jobs=None, extra=(), players=True):
    """Configure, build and write the runtime folder. Returns its path.
    players: also build the console players the toolkit has, into bin/."""
    toolkit = os.path.abspath(toolkit)
    from tools.prepare import toolkit as tk
    host = host_id()
    out = os.path.abspath(out or runtime_dir(toolkit, host))
    tmp = tempfile.mkdtemp(prefix="xba-runtime-")
    build_dir = os.path.abspath(build_dir or os.path.join(tmp, "build"))
    probe = os.path.join(tmp, "probe")
    log = os.path.join(tmp, "runtime.log")
    try:
        os.makedirs(os.path.join(probe, "src", "recomp", "gen"))
        with open(os.path.join(toolkit, "templates", "new-game", "CMakeLists.txt")) as f:
            cm = f.read().replace("project(your_game_recomp C)", "project(%s C)" % PROBE)
        with open(os.path.join(probe, "CMakeLists.txt"), "w") as f:
            f.write(cm)
        for fn in ("main.c", "recomp_manual.c"):
            shutil.copy(os.path.join(toolkit, "templates", "new-game", "src", fn), os.path.join(probe, "src", fn))
        q = os.path.join(build_dir, ".cmake", "api", "v1", "query")
        os.makedirs(q, exist_ok=True)
        open(os.path.join(q, "codemodel-v2"), "w").close()
        open(os.path.join(q, "toolchains-v1"), "w").close()
        gen = ["-G", "Ninja"] if shutil.which("ninja") else []
        _run(["cmake", "-S", probe, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release",
              "-DXBOXRECOMP_DIR=" + toolkit] + gen + list(extra), log)
        model, reply, idx = _reply(build_dir)
        config = model["configurations"][0]
        ref = next((t for t in config["targets"] if t["name"] == PROBE), None)
        if ref is None:
            raise SystemExit("no %s target in the file API reply" % PROBE)
        with open(os.path.join(reply, ref["jsonFile"])) as f:
            target = json.load(f)
        names = {t["id"]: t["name"] for t in config["targets"]}
        deps = [names[d["id"]] for d in target.get("dependencies", []) if d["id"] in names]
        play = {}
        if players:
            for t in config["targets"]:
                if t["name"] in PLAYERS and t["name"] not in play:
                    with open(os.path.join(reply, t["jsonFile"])) as f:
                        play[t["name"]] = _artifact(json.load(f), build_dir)
        cmd = ["cmake", "--build", build_dir, "--parallel"] + (["%d" % jobs] if jobs else [])
        if deps or play:
            cmd += ["--target"] + deps + sorted(play)
        _run(cmd, log)
        compiler = ""
        tc = next((o for o in idx["objects"] if o["kind"] == "toolchains"), None)
        if tc:
            with open(os.path.join(reply, tc["jsonFile"])) as f:
                for t in json.load(f).get("toolchains", []):
                    if t.get("language") == "C":
                        c = t.get("compiler", {})
                        compiler = "%s %s" % (c.get("id", ""), c.get("version", ""))
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        reloc = Relocator(toolkit, build_dir, probe, out)
        man = manifest_from_target(target, reloc, tk.runtime_digest(toolkit), compiler.strip(), host)
        if play:
            os.makedirs(os.path.join(out, "bin"))
            man["players"] = {}
            for name, exe in sorted(play.items()):
                if not exe or not os.path.isfile(exe):
                    raise SystemExit("%s not built (%s)" % (name, exe))
                shutil.copy2(exe, os.path.join(out, "bin", os.path.basename(exe)))
                man["players"][name.split("_")[0]] = "{runtime}/bin/" + os.path.basename(exe)
        missing = [a["arg"] for a in man["link"] if a["arg"].startswith("{runtime}/")
                   and not os.path.isfile(os.path.join(out, a["arg"][len("{runtime}/"):]))]
        if missing:
            raise SystemExit("libraries not built: %s" % ", ".join(missing))
        with open(os.path.join(out, MANIFEST), "w") as f:
            json.dump(man, f, indent=1)
            f.write("\n")
        size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
        print("%s: %d libraries, %d players, %.1f MB, link language %s" % (out, len(reloc.libs), len(play),
                                                                            size / 1e6, man["link_language"]))
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.prepare.runtime", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--toolkit", default=ROOT, help="the toolkit tree (default: this one)")
    ap.add_argument("--out", help="default <toolkit>/runtime/<host>")
    ap.add_argument("--build", help="CMake build folder (default: a temporary one, removed)")
    ap.add_argument("--jobs", type=int, help="parallel compiles")
    ap.add_argument("--no-players", action="store_true", help="leave the console players (bin/) out")
    ap.add_argument("cmake_args", nargs="*", help="more -D options for the configure (after --)")
    a = ap.parse_args(argv)
    build(a.toolkit, a.out, a.build, a.jobs, a.cmake_args, players=not a.no_players)
    return 0


if __name__ == "__main__":
    sys.exit(main())
