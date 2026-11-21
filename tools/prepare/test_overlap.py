"""The lift and the compile overlapped (compile.py EarlyCompile): the files a lifter has finished are compiled while it
goes on, compile_all adopts those objects, and nothing changes in what a Prepare produces. A fake lifter writes C files
one by one, a fake compiler turns each into an "object" made of its source's bytes (so an object names the bytes it
was compiled from), and records when it runs."""

import json
import os
import signal
import stat
import subprocess
import sys
import textwrap
import threading
import time

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import compile as cc         # noqa: E402
from tools.prepare import gba                   # noqa: E402
from tools.prepare.progress import parse_line   # noqa: E402

pytestmark = pytest.mark.skipif(os.name == "nt", reason="the fakes are POSIX scripts")


def _script(path, body):
    path.write_text("#!%s\n%s" % (sys.executable, textwrap.dedent(body)))
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return str(path)


def _fake_cc(tmp_path):
    """`fake-cc [flags] -c SRC -o OUT`: OUT = b"obj:" + SRC's bytes, after FAKE_CC_SLEEP seconds; each run appends
    `start <pid> <t>` and `end <pid> <t>` to $FAKE_CC_LOG."""
    return _script(tmp_path / "fake-cc", """
        import os, sys, time
        a = sys.argv[1:]
        if "--version" in a:
            print("clang version 19.0.0 (fake)")
            sys.exit(0)
        src, out = a[a.index("-c") + 1], a[a.index("-o") + 1]
        log = os.environ.get("FAKE_CC_LOG")
        def note(what):
            if log:
                with open(log, "a") as f:
                    f.write("%s %d %f\\n" % (what, os.getpid(), time.time()))
        note("start")
        with open(src, "rb") as f:
            data = f.read()
        time.sleep(float(os.environ.get("FAKE_CC_SLEEP", "0")))
        if b"#error" in data:
            sys.stderr.write("%s:1: error: asked to fail\\n" % src)
            note("end")
            sys.exit(1)
        with open(out, "wb") as f:
            f.write(b"obj:" + data)
        note("end")
    """)


def _fake_lifter(tmp_path):
    """`fake-lift OUT N DELAY [--fail-after K] [--rewrite-first]`: N files, DELAY seconds apart."""
    return _script(tmp_path / "fake-lift", """
        import os, sys, time
        out, n, delay = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
        fail = int(sys.argv[sys.argv.index("--fail-after") + 1]) if "--fail-after" in sys.argv else None
        for i in range(n):
            if fail is not None and i == fail:
                sys.exit(3)
            with open(os.path.join(out, "chunk_%04d.c" % i), "w") as f:
                f.write("/* chunk %d */\\nvoid f%d(void) {}\\n" % (i, i))
            time.sleep(delay)
        if "--rewrite-first" in sys.argv:       # the end of a lift writing a chunk again (tools.recomp's check)
            with open(os.path.join(out, "chunk_0000.c"), "w") as f:
                f.write("/* chunk 0, as the end wrote it */\\nvoid f0(void) {}\\n")
            time.sleep(delay)
    """)


def _max_running(log):
    """The most fake compiles running at once, from $FAKE_CC_LOG."""
    ev = []
    if os.path.exists(log):
        with open(log) as f:
            for line in f:
                what, _, t = line.split()
                ev.append((float(t), 1 if what == "start" else -1))
    n = best = 0
    for _, d in sorted(ev):
        n += d
        best = max(best, n)
    return best


class _Tc(cc.Toolchain):
    def compiler_id(self):
        return "fake"


def _lift(tmp_path, early, *args):
    gen = tmp_path / "gen"
    p = early.tc.procs.start([_fake_lifter(tmp_path), str(gen)] + [str(a) for a in args],
                             stdin=subprocess.DEVNULL)
    try:
        rc = p.wait()
    finally:
        early.tc.procs.done(p)
    return rc


def _setup(tmp_path, monkeypatch, jobs=3, limit=None, sleep="0.3"):
    monkeypatch.setenv("FAKE_CC_LOG", str(tmp_path / "cc.log"))
    monkeypatch.setenv("FAKE_CC_SLEEP", sleep)
    gen = tmp_path / "gen"
    gen.mkdir()
    tc = _Tc(_fake_cc(tmp_path), str(tmp_path))
    flags = ["-O2", "-I" + str(gen)]
    early = cc.EarlyCompile(str(gen), flags, tc, str(tmp_path / "obj"), jobs, limit=limit, mem=None)
    early.POLL_S = 0.05
    return gen, flags, tc, early


def _objects(objs):
    out = {}
    for o in objs:
        with open(o, "rb") as f:
            out[os.path.basename(o)] = f.read()
    return out


def test_overlap_adopts_the_objects_compile_all_would_make(tmp_path, monkeypatch):
    gen, flags, tc, early = _setup(tmp_path, monkeypatch)
    early.start()
    assert _lift(tmp_path, early, 8, 0.3) == 0
    early.finish()
    assert early.compiled > 0
    sources = sorted(str(gen / fn) for fn in os.listdir(gen))
    stats = {}
    objs, n = cc.compile_all(sources, flags, tc, str(tmp_path / "obj"), 3, stats=stats, early=early)
    assert n == 8 and stats["compiled"] == 8 and 0 < stats["early"] <= 8
    assert len(stats["peak_kb"]) >= 8
    # The same objects, under the same keys, as a compile without the overlap.
    plain, _ = cc.compile_all(sources, flags, tc, str(tmp_path / "plain"), 3)
    assert _objects(objs) == _objects(plain)
    assert not [fn for fn in os.listdir(tmp_path / "obj") if fn.startswith(cc.EARLY_PREFIX)]
    # A second compile has nothing to do.
    assert cc.compile_all(sources, flags, tc, str(tmp_path / "obj"), 3)[1] == 0


def test_overlap_never_keeps_an_object_of_other_bytes(tmp_path, monkeypatch):
    """A chunk written again after its early compile (tools.recomp's end checking its forecast): that object is not
    adopted; the final bytes are compiled."""
    gen, flags, tc, early = _setup(tmp_path, monkeypatch, sleep="0.05")
    early.start()
    assert _lift(tmp_path, early, 4, 0.4, "--rewrite-first") == 0
    early.finish()
    sources = sorted(str(gen / fn) for fn in os.listdir(gen))
    objs, _ = cc.compile_all(sources, flags, tc, str(tmp_path / "obj"), 3, early=early)
    with open(objs[0], "rb") as f:
        assert f.read() == b"obj:" + (gen / "chunk_0000.c").read_bytes()
    assert b"as the end wrote it" in (gen / "chunk_0000.c").read_bytes()


def test_overlap_counts_the_lifter_as_a_job(tmp_path, monkeypatch):
    gen, flags, tc, early = _setup(tmp_path, monkeypatch, jobs=3, sleep="0.5")
    early.start()
    _lift(tmp_path, early, 8, 0.1)
    early.finish()
    assert early.compiled > 0 and _max_running(str(tmp_path / "cc.log")) <= 2


@pytest.mark.parametrize("jobs,limit", [(1, None), (4, lambda: 1), (4, lambda: 0)])
def test_overlap_none_with_one_job_or_the_device_holding(tmp_path, monkeypatch, jobs, limit):
    """--jobs 1, a severe device (1) or a hold (0): no compile beside the lifter, as before the overlap."""
    gen, flags, tc, early = _setup(tmp_path, monkeypatch, jobs=jobs, limit=limit)
    early.start()
    _lift(tmp_path, early, 4, 0.2)
    early.finish()
    assert early.compiled == 0 and not os.path.exists(tmp_path / "cc.log")


def test_overlap_waits_for_memory(tmp_path, monkeypatch):
    gen, flags, tc, early = _setup(tmp_path, monkeypatch)
    early.mem = lambda: cc.RESERVED          # no room for one more 512 MB job
    early.start()
    _lift(tmp_path, early, 4, 0.2)
    early.finish()
    assert early.compiled == 0


def test_overlap_failed_early_compile_is_reported_by_compile_all(tmp_path, monkeypatch):
    gen, flags, tc, early = _setup(tmp_path, monkeypatch, sleep="0")
    early.start()
    (gen / "bad.c").write_text("#error no\n")
    _lift(tmp_path, early, 3, 0.3)
    early.finish()
    assert early.failed >= 1
    sources = sorted(str(gen / fn) for fn in os.listdir(gen))
    with pytest.raises(cc.StepError) as e:
        cc.compile_all(sources, flags, tc, str(tmp_path / "obj"), 3, early=early)
    assert e.value.code == "COMPILE_FAILED" and "bad.c" in e.value.msg


# ── Through a Run: a failed or cancelled lift stops the compiles ──

class _Rep:
    def __init__(self):
        self.lines = []

    def progress(self, step, pct, msg, eta=None):
        self.lines.append((step, pct, msg))


class _A:
    def __init__(self, cc_path):
        self.cc, self.jobs, self.opt, self.include_dir, self.extra_src = cc_path, 3, "1", [], []
        self.device_log, self.python, self.source = None, sys.executable, "game.gba"


class _Host:
    ROOT = ROOT


def _run(tmp_path, monkeypatch, sleep):
    monkeypatch.setenv("FAKE_CC_LOG", str(tmp_path / "cc.log"))
    monkeypatch.setenv("FAKE_CC_SLEEP", sleep)
    monkeypatch.setattr(cc.EarlyCompile, "POLL_S", 0.05)
    monkeypatch.setattr(cc, "mem_available", lambda meminfo=None: None)
    r = gba.Run(_A(_fake_cc(tmp_path)), _Host(), False)
    r.rep = _Rep()
    r.work = str(tmp_path / "work")
    os.makedirs(r.work)
    r.log = os.path.join(r.work, "prepare.log")
    r.stamp_path = os.path.join(r.work, "stamp.json")
    r.save_stamp(lift="an earlier key")
    return r


def _pids_alive(log):
    pids = set()
    with open(log) as f:
        for line in f:
            pids.add(int(line.split()[1]))
    alive = []
    for pid in pids:
        try:
            os.kill(pid, 0)
            alive.append(pid)
        except OSError:
            pass
    return alive


def test_lift_failure_stops_the_early_compiles(tmp_path, monkeypatch):
    r = _run(tmp_path, monkeypatch, sleep="30")
    gen = os.path.join(r.work, "gen")
    args = [_fake_lifter(tmp_path), gen, "6", "0.3", "--fail-after", "4"]
    t0 = time.time()
    with pytest.raises(cc.StepError) as e:
        r.run_lifter("fake", args, dict(os.environ), gen, "key", "lifting", True)
    assert e.value.code == "LIFT_FAILED"
    assert time.time() - t0 < 20                     # the 30 s compiles were killed, not waited for
    assert os.path.exists(tmp_path / "cc.log") and not _pids_alive(str(tmp_path / "cc.log"))
    assert not r.procs.live
    obj = os.path.join(r.work, "obj")
    assert not [fn for fn in os.listdir(obj) if fn.startswith(cc.EARLY_PREFIX)]
    with open(r.stamp_path) as f:
        assert "lift" not in json.load(f)            # XBA-138: a half-written gen/ is never a finished lift


def test_cancel_during_the_overlap_kills_the_lifter_and_the_compiles(tmp_path, monkeypatch):
    r = _run(tmp_path, monkeypatch, sleep="30")
    gen = os.path.join(r.work, "gen")
    args = [_fake_lifter(tmp_path), gen, "50", "0.3"]
    log = str(tmp_path / "cc.log")

    def cancel_once_compiling():
        for _ in range(200):
            if os.path.exists(log):
                break
            time.sleep(0.05)
        r.procs.cancel()                             # what the SIGTERM handler does (then it raises Cancelled)
    threading.Thread(target=cancel_once_compiling, daemon=True).start()
    t0 = time.time()
    with pytest.raises(cc.Cancelled):
        r.run_lifter("fake", args, dict(os.environ), gen, "key", "lifting", True)
    assert time.time() - t0 < 20
    assert not _pids_alive(log) and not r.procs.live
    assert not [fn for fn in os.listdir(os.path.join(r.work, "obj")) if fn.startswith(cc.EARLY_PREFIX)]


def test_sigterm_during_the_overlap(tmp_path):
    """The whole Prepare (a GBA pack: its lifter replaced by one that writes files slowly) gets SIGTERM while it lifts
    and compiles: exit 130, CANCELLED, no lifter or compiler left, no pack."""
    tools = tmp_path / "tools"
    tools.mkdir()
    (tools / "slow_lift.py").write_text(textwrap.dedent("""
        import os, sys, time
        out = sys.argv[2]
        for i in range(200):
            with open(os.path.join(out, "chunk_%04d.c" % i), "w") as f:
                f.write("void f%d(void) {}\\n" % i)
            time.sleep(0.2)
    """))
    rom = tmp_path / "game.gba"
    rom.write_bytes(bytes(4096))
    log = tmp_path / "cc.log"
    env = dict(os.environ, XBA_GBA_RECOMP="slow_lift", XBA_GBA_TOOLS_DIR=str(tools), FAKE_CC_LOG=str(log),
               FAKE_CC_SLEEP="30")
    p = subprocess.Popen([sys.executable, "-m", "tools.prepare", str(rom), "--work", str(tmp_path / "w"),
                          "--pack-dir", str(tmp_path / "packs"), "--cc", _fake_cc(tmp_path), "--jobs", "3"],
                         cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    try:
        for line in p.stdout:
            lines.append(line)
            if "compiled meanwhile" in line or (log.exists() and "start" in log.read_text()):
                break
        p.send_signal(signal.SIGTERM)
        out, _ = p.communicate(timeout=30)
    finally:
        if p.poll() is None:
            p.kill()
    lines += out.splitlines()
    parsed = [parse_line(l) for l in lines]
    parsed = [x for x in parsed if x]
    assert p.returncode == 130 and parsed[-1]["kind"] == "result" and parsed[-1]["code"] == "CANCELLED", lines
    assert log.exists() and not _pids_alive(str(log))
    assert not (tmp_path / "packs").exists() or not os.listdir(tmp_path / "packs")
    assert all(x.get("id") == "lift" for x in parsed if x["kind"] == "progress" and x.get("step") == 3)
