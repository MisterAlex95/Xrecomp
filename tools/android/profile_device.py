#!/usr/bin/env python3
"""Profile the running XRecomp game on an Android device over adb (XBA-36).

    tools/android/profile.sh SECONDS [--serial S] [--package P] [--out DIR] [--freq HZ] [--dwarf]
                             [--ndk DIR] [--lib DIR]... [--no-report]

Records SECONDS of CPU samples of the game's process with the NDK's simpleperf, pulls the profile and the libraries
it needs, and writes into DIR (default ~/xrecomp-profiles/<package>-<date>):

    perf.data           the samples (simpleperf record, call graphs)
    binary_cache/       the libraries, matched by build id (simpleperf's binary_cache_builder.py)
    profile.html        simpleperf's report_html.py: function table, call graphs and a flame graph per thread
    flamegraph.html     the flame graph of the game's process (inferno)
    hot.txt             the hottest functions (self time), each lifted function with its guest address
    guest_symbols.tsv   the title pack's functions: host offset, size, name, console, guest address
    frames.log          the performance lines logged meanwhile ("<console>: perf ...", every 10 s)

Access: the app must be debuggable (run-as): the rolling xrecomp-latest build and debug builds are; the release
APKs (xrecomp-v*) are not, and then only a rooted device (su) works. The game runs in the app's main process (the
launcher is <package>:launcher), so `pidof <package>` is the game.

Lifted functions keep their names in libtitle.so: Prepare links it with ld.lld and no -s, so its .symtab names every
generated function after its guest address (sub_0001ABCD on the Xbox, gba_rc_08000123, n64_rc_80001234,
ps1_rc_80010000). Functions with other names (hand-written overrides, functions func_id named) get their address from
the title's dispatch table in the Prepare work folder when it is still there. The pack is pulled with run-as (it lives
in the app's private storage, which binary_cache_builder.py cannot read) and handed to binary_cache_builder.py.

The output folder holds the title's compiled code (libtitle.so): keep it to yourself, never commit or share it.
"""
import argparse
import datetime
import glob
import os
import re
import shlex
import struct
import subprocess
import sys

DEFAULT_PACKAGE = "fr.adserver.xrecomp.shell"
DEVICE_TMP = "/data/local/tmp"
DEVICE_SIMPLEPERF = DEVICE_TMP + "/simpleperf"
DEVICE_PERF_DATA = DEVICE_TMP + "/xrecomp-perf.data"
MAX_SECONDS = 3600

# A lifted function's name -> (console, guest address). The Xbox's sub_XXXXXXXX (its _gen body too), the other
# consoles' <console>_rc_XXXXXXXX (and the _rc_f_ variant).
GUEST_NAME = re.compile(r"^(?:sub_|(gba|n64|ps1)_rc_(?:f_)?)([0-9A-Fa-f]{8})(?:_gen)?$")
# A dispatch table's entry: { 0x0001ABCDu, (recomp_func_t)name },
DISPATCH_ENTRY = re.compile(r"\{\s*0x([0-9A-Fa-f]{1,8})u?\s*,\s*\(recomp_func_t\)\s*([A-Za-z_]\w*)\s*\}")
# A performance line in logcat ("xbox: perf ...", "n64: perf over the run: ...").
PERF_LINE = re.compile(r"\b(xbox|gba|ps1|n64|host): perf ")
ABI_DIRS = {"arm64-v8a": "arm64", "armeabi-v7a": "arm", "x86_64": "x86_64", "x86": "x86", "riscv64": "riscv64"}


class ProfileError(Exception):
    """A step failed in a way the user can act on; the message says how."""


# ── Arguments ──────────────────────────────────────────────────────────────────────────────────────────────────────

def _seconds(text):
    try:
        v = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a whole number of seconds" % text)
    if not 1 <= v <= MAX_SECONDS:
        raise argparse.ArgumentTypeError("seconds must be between 1 and %d" % MAX_SECONDS)
    return v


def _freq(text):
    try:
        v = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("%r is not a frequency in Hz" % text)
    if not 100 <= v <= 20000:
        raise argparse.ArgumentTypeError("the sampling frequency must be between 100 and 20000 Hz")
    return v


def parse_args(argv):
    ap = argparse.ArgumentParser(prog="profile.sh", description="Profile the running XRecomp game over adb "
                                 "(simpleperf) and write a flame graph.")
    ap.add_argument("seconds", type=_seconds, help="how long to record")
    ap.add_argument("--serial", "-s", help="the device's adb serial (default: $ANDROID_SERIAL, or the only device)")
    ap.add_argument("--package", "-p", default=DEFAULT_PACKAGE,
                    help="the app's id (default %(default)s, the XRecomp app; a per-title APK has its own)")
    ap.add_argument("--out", "-o", help="output folder (default ~/xrecomp-profiles/<package>-<date>)")
    ap.add_argument("--freq", "-f", type=_freq, default=1000, help="samples per second per thread (default 1000)")
    ap.add_argument("--dwarf", action="store_true",
                    help="unwind with DWARF (heavier, copes with code built without frame pointers) instead of the "
                         "frame pointers Android's arm64 code keeps")
    ap.add_argument("--ndk", help="the NDK (default: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, $ANDROID_NDK, the newest "
                                  "under $ANDROID_HOME/ndk or $ANDROID_SDK_ROOT/ndk)")
    ap.add_argument("--lib", action="append", default=[],
                    help="a folder of unstripped libraries (libmain.so of the same build): matched by build id")
    ap.add_argument("--no-report", action="store_true", help="record and pull only (no HTML, no hot list)")
    return ap.parse_args(argv)


def default_out(package, now=None, home=None):
    now = now or datetime.datetime.now()
    home = home or os.path.expanduser("~")
    return os.path.join(home, "xrecomp-profiles", "%s-%s" % (package, now.strftime("%Y%m%d-%H%M%S")))


# ── The NDK ────────────────────────────────────────────────────────────────────────────────────────────────────────

def _version_key(path):
    return [int(x) if x.isdigit() else 0 for x in re.split(r"[.\-]", os.path.basename(path))]


def find_simpleperf(ndk=None, env=None):
    """The NDK's simpleperf folder (app_profiler.py, report_html.py, bin/...)."""
    env = os.environ if env is None else env
    candidates = []
    if ndk:
        candidates.append(ndk)
    else:
        for var in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK"):
            if env.get(var):
                candidates.append(env[var])
        for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
            if env.get(var):
                candidates += sorted(glob.glob(os.path.join(env[var], "ndk", "*")), key=_version_key, reverse=True)
    for c in candidates:
        sp = os.path.join(c, "simpleperf")
        if os.path.isfile(os.path.join(sp, "report_html.py")):
            return sp
    raise ProfileError("no NDK with simpleperf found (looked in: %s); pass --ndk or set ANDROID_NDK_HOME"
                       % (", ".join(candidates) or "nothing set"))


def host_simpleperf(sp_dir, platform=None):
    platform = platform or sys.platform
    sub = "darwin" if platform == "darwin" else "windows" if platform.startswith("win") else "linux"
    exe = "simpleperf.exe" if sub == "windows" else "simpleperf"
    path = os.path.join(sp_dir, "bin", sub, "x86_64", exe)
    if not os.path.isfile(path):
        raise ProfileError("%s: no host simpleperf in this NDK" % path)
    return path


def device_simpleperf(sp_dir, abi):
    arch = ABI_DIRS.get(abi.strip())
    if not arch:
        raise ProfileError("unknown device ABI %r" % abi)
    path = os.path.join(sp_dir, "bin", "android", arch, "simpleperf")
    if not os.path.isfile(path):
        raise ProfileError("%s: no simpleperf for %s in this NDK" % (path, abi))
    return path


# ── adb ────────────────────────────────────────────────────────────────────────────────────────────────────────────

class Adb:
    """adb with one device; `run` is subprocess.run-shaped so tests can fake it."""

    def __init__(self, serial=None, run=subprocess.run, adb="adb"):
        self.base = [adb] + (["-s", serial] if serial else [])
        self._run = run

    def call(self, args, timeout=60, binary=False):
        try:
            r = self._run(self.base + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except FileNotFoundError:
            raise ProfileError("adb not found on the PATH (Android SDK platform-tools)")
        except subprocess.TimeoutExpired:
            raise ProfileError("adb %s: no answer in %d s" % (" ".join(args[:3]), timeout))
        out = r.stdout if binary else (r.stdout or b"").decode("utf-8", "replace")
        return r.returncode, out

    def shell(self, cmd, timeout=60):
        """cmd: a list of words, quoted for the device's shell."""
        return self.call(["shell", " ".join(shlex.quote(c) for c in cmd)], timeout=timeout)

    def exec_out(self, cmd, timeout=300):
        return self.call(["exec-out", " ".join(shlex.quote(c) for c in cmd)], timeout=timeout, binary=True)

    def push(self, local, remote):
        return self.call(["push", local, remote], timeout=300)[0] == 0

    def pull(self, remote, local):
        return self.call(["pull", remote, local], timeout=600)[0] == 0


def as_app(mode, package, cmd):
    """cmd run with the app's rights: run-as on a debuggable app, su on a rooted device."""
    if mode == "run-as":
        return ["run-as", package] + cmd
    return ["su", "0"] + cmd


def access_mode(adb, package):
    """'run-as' (the app is debuggable) or 'su' (a rooted device); ProfileError otherwise."""
    rc, out = adb.shell(["pm", "path", package])
    if rc != 0 or "package:" not in out:
        raise ProfileError("%s is not installed on the device" % package)
    rc, out = adb.shell(["run-as", package, "id"])
    if rc == 0 and "uid=" in out:
        return "run-as"
    rc, out = adb.shell(["su", "0", "id"])
    if rc == 0 and "uid=0" in out:
        return "su"
    raise ProfileError(
        "%s is not debuggable (run-as refused) and the device is not rooted. simpleperf can only profile a "
        "debuggable app on a production device: install the rolling xrecomp-latest APK (debuggable) or a debug "
        "build; the release APKs (xrecomp-v*) are not debuggable." % package)


def find_pid(adb, package):
    """The game's process: the app's main process (the launcher runs as <package>:launcher)."""
    rc, out = adb.shell(["pidof", package])
    pids = [int(p) for p in out.split() if p.isdigit()] if rc == 0 else []
    if not pids:
        raise ProfileError("%s is not running: start a game, then run profile.sh while it plays" % package)
    return pids[0]


# ── Process maps and the pack ──────────────────────────────────────────────────────────────────────────────────────

def app_libraries(maps_text, package):
    """The shared libraries mapped from the app's private storage (the title pack), in first-seen order. Those
    from the APK (libmain.so) and the system are pulled by binary_cache_builder.py itself."""
    roots = ("/data/data/%s/" % package, "/data/user/")
    out = []
    for line in maps_text.splitlines():
        parts = line.split(None, 5)
        if len(parts) < 6:
            continue
        path = parts[5].strip()
        if path.endswith(" (deleted)") or not path.endswith(".so"):
            continue
        if path.startswith(roots[0]) or (path.startswith(roots[1]) and "/%s/" % package in path):
            if path not in out:
                out.append(path)
    return out


def pack_dir_name(lib_path):
    """'files/packs/<dir>/libtitle.so' -> '<dir>' (a title id, gba-<key>...), else None."""
    m = re.search(r"/files/packs/([^/]+)/[^/]+\.so$", lib_path)
    return m.group(1) if m else None


def local_lib_path(libs_dir, device_path):
    return os.path.join(libs_dir, device_path.lstrip("/"))


# ── ELF symbols ────────────────────────────────────────────────────────────────────────────────────────────────────

class ElfInfo:
    def __init__(self):
        self.build_id = None
        self.has_symtab = False
        self.functions = []   # (value, size, name) of the STT_FUNC symbols with a value, from .symtab


def read_elf(path):
    """Build id and function symbols of an ELF64 little-endian file (enough for an arm64 libtitle.so)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise ProfileError("%s: not an ELF file" % path)
    if data[4] != 2 or data[5] != 1:
        raise ProfileError("%s: not a 64-bit little-endian ELF" % path)
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, typ, _flags, _addr, offset, size, link, _info, _align, entsize = \
            struct.unpack_from("<IIQQQQIIQQ", data, off)
        secs.append((name, typ, offset, size, link, entsize))
    info = ElfInfo()

    def cstr(blob_off, idx):
        end = data.index(b"\0", blob_off + idx)
        return data[blob_off + idx:end].decode("utf-8", "replace")

    shstr_off = secs[e_shstrndx][2] if e_shstrndx < len(secs) else None
    for name, typ, offset, size, link, entsize in secs:
        sname = cstr(shstr_off, name) if shstr_off is not None else ""
        if typ == 7 and sname == ".note.gnu.build-id":   # SHT_NOTE
            namesz, descsz, ntype = struct.unpack_from("<III", data, offset)
            desc = offset + 12 + ((namesz + 3) & ~3)
            if ntype == 3:                                 # NT_GNU_BUILD_ID
                info.build_id = data[desc:desc + descsz].hex()
        if typ == 2:                                       # SHT_SYMTAB
            info.has_symtab = True
            str_off = secs[link][2]
            n = size // (entsize or 24)
            for k in range(n):
                st_name, st_info, _other, shndx, value, ssize = struct.unpack_from("<IBBHQQ", data, offset + k * 24)
                if (st_info & 0xF) == 2 and shndx != 0 and value:   # STT_FUNC, defined
                    info.functions.append((value, ssize, cstr(str_off, st_name)))
    info.functions.sort()
    return info


def guest_address(name, dispatch=None):
    """(console, guest address) of a lifted function's name, else None."""
    m = GUEST_NAME.match(name)
    if m:
        return (m.group(1) or "xbox"), int(m.group(2), 16)
    if dispatch and name in dispatch:
        return "xbox", dispatch[name]
    return None


def parse_dispatch(text):
    """A title's dispatch table (<prefix>_dispatch.c) -> {function name: guest address}."""
    return {m.group(2): int(m.group(1), 16) for m in DISPATCH_ENTRY.finditer(text)}


def guest_symbols(info, dispatch=None):
    """Rows (host offset, size, name, console, guest address) of the lifted functions of a library."""
    rows = []
    for value, size, name in info.functions:
        g = guest_address(name, dispatch)
        if g:
            rows.append((value, size, name, g[0], g[1]))
    return rows


def write_guest_symbols(path, rows):
    with open(path, "w") as f:
        f.write("# host_offset\tsize\tname\tconsole\tguest_address\n")
        for value, size, name, console, va in rows:
            f.write("0x%x\t%d\t%s\t%s\t0x%08X\n" % (value, size, name, console, va))


# ── Reports ────────────────────────────────────────────────────────────────────────────────────────────────────────

def annotate_report(text, dispatch=None):
    """simpleperf report's text with the guest address after each lifted function ("  [xbox 0x0001ABCD]")."""
    out = []
    for line in text.splitlines():
        tail = line.rstrip()
        words = tail.split()
        g = guest_address(words[-1], dispatch) if words else None
        if g and re.match(r"^\s*\d+(\.\d+)?%", line):
            tail += "  [%s 0x%08X]" % g
        out.append(tail)
    return "\n".join(out) + ("\n" if out else "")


def perf_lines(logcat_text):
    """The performance lines of a logcat dump."""
    return [l for l in logcat_text.splitlines() if PERF_LINE.search(l)]


def record_command(package, pid, seconds, freq, dwarf, mode):
    """simpleperf record on the device. With run-as, --app makes simpleperf run in the app's context (all its
    processes; the reports keep the game's pid); with su, the game's pid alone."""
    cmd = [DEVICE_SIMPLEPERF, "record", "-e", "task-clock", "-f", str(freq),
           "--call-graph", "dwarf" if dwarf else "fp", "--duration", str(seconds), "-o", DEVICE_PERF_DATA]
    if mode == "run-as":
        cmd += ["--app", package]
    else:
        cmd = ["su", "0"] + cmd + ["-p", str(pid)]
    return cmd


# ── The run ────────────────────────────────────────────────────────────────────────────────────────────────────────

def log(msg):
    print("profile: " + msg, flush=True)


def run_tool(argv, cwd, env, what):
    log("%s: %s" % (what, " ".join(shlex.quote(a) for a in argv)))
    r = subprocess.run(argv, cwd=cwd, env=env)
    if r.returncode != 0:
        log("%s failed (exit %d)" % (what, r.returncode))
    return r.returncode == 0


def profile(a):
    sp = find_simpleperf(a.ndk)
    ndk = os.path.dirname(sp)
    out = os.path.abspath(a.out or default_out(a.package))
    os.makedirs(out, exist_ok=True)
    env = dict(os.environ)
    if a.serial:
        env["ANDROID_SERIAL"] = a.serial
    adb = Adb(a.serial or env.get("ANDROID_SERIAL"))

    rc, state = adb.call(["get-state"])
    if rc != 0 or "device" not in state:
        raise ProfileError("no device on adb (or more than one: pass --serial)")
    mode = access_mode(adb, a.package)
    pid = find_pid(adb, a.package)
    _, abi = adb.shell(["getprop", "ro.product.cpu.abi"])
    log("%s pid %d (%s, %s), %d s at %d Hz into %s" % (a.package, pid, abi.strip(), mode, a.seconds, a.freq, out))

    # The pack and the other libraries from the app's storage, while the game has them mapped.
    rc, maps = adb.exec_out(as_app(mode, a.package, ["cat", "/proc/%d/maps" % pid]))
    libs = app_libraries(maps.decode("utf-8", "replace"), a.package) if rc == 0 else []
    if not libs:
        log("warning: no library from the app's storage is mapped (no title pack loaded?)")
    libs_dir = os.path.join(out, "libs")
    pulled = []
    for path in libs:
        rc, blob = adb.exec_out(as_app(mode, a.package, ["cat", path]))
        if rc != 0 or blob[:4] != b"\x7fELF":
            log("warning: could not pull %s" % path)
            continue
        dst = local_lib_path(libs_dir, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(blob)
        pulled.append((path, dst))

    # Record.
    if not adb.push(device_simpleperf(sp, abi), DEVICE_SIMPLEPERF):
        raise ProfileError("could not push simpleperf to %s" % DEVICE_TMP)
    adb.shell(["chmod", "755", DEVICE_SIMPLEPERF])
    _, t0 = adb.shell(["date", "+%s"])
    cmd = record_command(a.package, pid, a.seconds, a.freq, a.dwarf, mode)
    log("recording: adb shell " + " ".join(cmd))
    rc, text = adb.shell(cmd, timeout=a.seconds + 120)
    if rc != 0:
        raise ProfileError("simpleperf record failed:\n" + text.strip())
    if not adb.pull(DEVICE_PERF_DATA, os.path.join(out, "perf.data")):
        raise ProfileError("could not pull %s" % DEVICE_PERF_DATA)
    adb.shell(["rm", "-f", DEVICE_PERF_DATA])

    # The performance lines logged meanwhile.
    t0 = t0.strip()
    if t0.isdigit():
        rc, lc = adb.call(["logcat", "-d", "-T", t0 + ".000"], timeout=60)
        lines = perf_lines(lc) if rc == 0 else []
        with open(os.path.join(out, "frames.log"), "w") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))

    # The pack's symbols, with the guest addresses of the functions not named after theirs.
    rows = []
    for path, dst in pulled:
        info = read_elf(dst)
        dispatch = None
        tid = pack_dir_name(path)
        if tid and not tid.startswith(("gba-", "n64-", "ps1-")):
            rc, found = adb.shell(as_app(mode, a.package, ["find", "/data/data/%s/files/prepare/%s" % (a.package, tid),
                                                             "-name", "*_dispatch.c"]))
            first = found.split()[0] if rc == 0 and found.split() else None
            if first:
                rc, blob = adb.exec_out(as_app(mode, a.package, ["cat", first]))
                if rc == 0:
                    dispatch = parse_dispatch(blob.decode("utf-8", "replace"))
        if not info.has_symtab:
            log("warning: %s has no .symtab (stripped): lifted functions will show as addresses in it" % path)
            continue
        found_rows = guest_symbols(info, dispatch)
        log("%s: %d functions, %d lifted, build id %s" % (os.path.basename(path), len(info.functions),
                                                          len(found_rows), info.build_id or "none"))
        rows += found_rows
    if rows:
        write_guest_symbols(os.path.join(out, "guest_symbols.tsv"), rows)

    # The libraries for simpleperf (binary_cache/), then the reports, in the output folder.
    py = sys.executable or "python3"
    cache = [py, os.path.join(sp, "binary_cache_builder.py"), "-i", "perf.data", "--ndk_path", ndk,
             "-lib", libs_dir] + [os.path.abspath(d) for d in a.lib]
    os.makedirs(libs_dir, exist_ok=True)
    run_tool(cache, out, env, "binary cache")
    if a.no_report:
        log("done (no report): %s" % out)
        return out
    run_tool([py, os.path.join(sp, "report_html.py"), "-i", "perf.data", "-o", "profile.html", "--no_browser",
              "--ndk_path", ndk, "--pid", str(pid)], out, env, "report_html")
    run_tool([py, os.path.join(sp, "inferno", "inferno.py"), "-sc", "--record_file", "perf.data", "-o",
              "flamegraph.html", "--no_browser", "--symfs", "binary_cache", "--include-pid", str(pid),
              "--title", "%s %d s" % (a.package, a.seconds)], out, env, "inferno")
    r = subprocess.run([host_simpleperf(sp), "report", "-i", "perf.data", "--symfs", "binary_cache", "--pids",
                        str(pid), "--sort", "dso,symbol", "-n", "--percent-limit", "0.2"],
                       cwd=out, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode == 0:
        dispatch_all = {name: va for _o, _s, name, c, va in rows if c == "xbox"}
        with open(os.path.join(out, "hot.txt"), "w") as f:
            f.write(annotate_report(r.stdout.decode("utf-8", "replace"), dispatch_all))
    else:
        log("simpleperf report failed: %s" % r.stderr.decode("utf-8", "replace").strip()[:300])
    log("done: %s (flamegraph.html, profile.html, hot.txt, frames.log)" % out)
    return out


def main(argv=None):
    a = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        profile(a)
    except ProfileError as e:
        print("profile: error: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
