"""tools.prepare (desktop) for a launcher: --progress lines, --check --json,
the build's percentage. With the synthetic tools/conformance/test.xbe, alone
and in an image made by tools/xiso/mkxiso.py; the CMake build is faked (the
real one takes minutes). No game data. docs/macos/launcher.md « Prepare
interface »."""

import json
import os
import shutil
import signal
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import __main__ as prep             # noqa: E402
from tools.prepare.progress import (                   # noqa: E402
    DESKTOP_STEP_IDS, STEP_IDS, parse_line, progress_line, result_ok_exe, result_ok_project, Reporter)
from tools.xiso import mkxiso                          # noqa: E402

TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")
NAME = "title_12345678"

needs_pipeline = pytest.mark.skipif(
    not all(__import__("importlib").util.find_spec(m) for m in ("capstone", "pefile")),
    reason="the analysis pipeline needs capstone and pefile")


def _protocol(out):
    """The protocol lines of an output, each checked to round-trip."""
    got = []
    for line in out.splitlines():
        if line.startswith("XBA_"):
            p = parse_line(line)
            assert p is not None, line
            got.append((line, p))
    return got


def _xbe(tmp_path):
    """A copy of test.xbe: a prepare writes <stem>_analysis.json beside its XBE."""
    d = tmp_path / "src"
    d.mkdir(exist_ok=True)
    shutil.copy(TEST_XBE, d / "default.xbe")
    return str(d / "default.xbe")


def _image(tmp_path):
    d = tmp_path / "disc"
    (d / "media").mkdir(parents=True)
    shutil.copy(TEST_XBE, d / "default.xbe")
    (d / "media" / "data.bin").write_bytes(b"\x02" * 3000)
    out = tmp_path / "synth.iso"
    mkxiso.build(str(d), str(out))
    return out


def _fake_build(monkeypatch, lines=("[1/4] Building C object a.o", "[2/4] Building C object b.o",
                                    "warning: something", "[4/4] Linking C executable " + NAME), fail=False):
    """cmake configure and build without CMake: the build feeds `lines` to
    on_line and leaves the executable (or fails like a compile error)."""
    real = prep.run

    def run(args, log, cwd=prep.ROOT, on_line=None, tick=None):
        if args[0] != "cmake":
            return real(args, log, cwd, on_line)
        if "--build" in args:
            for ln in lines:
                if on_line:
                    on_line(ln + "\n")
            if fail:
                raise prep.CommandFailed(1, args, log, "error: x.c:1: boom")
            build = args[args.index("--build") + 1]
            with open(os.path.join(build, NAME), "w") as f:
                f.write("#!/bin/sh\n")
        else:
            os.makedirs(args[args.index("-B") + 1], exist_ok=True)
        return 0.0
    monkeypatch.setattr(prep, "run", run)


# ── the grammar ──

def test_desktop_lines_round_trip():
    assert STEP_IDS == ("identify", "analyse", "lift", "compile", "pack")      # Android unchanged
    assert DESKTOP_STEP_IDS == ("identify", "analyse", "project", "lift", "build")
    line = progress_line(5, "build", 37, "compiling 12/345", steps=5, ids=DESKTOP_STEP_IDS)
    assert line == "XBA_PROGRESS step=5/5 id=build pct=37 msg=compiling 12/345"
    assert parse_line(line) == {"kind": "progress", "step": 5, "steps": 5, "id": "build", "pct": 37,
                                "msg": "compiling 12/345"}
    with pytest.raises(ValueError):
        progress_line(3, "project", 0, "x")                                      # not an Android id
    assert result_ok_exe("/w/project/build/t x") == "XBA_RESULT ok exe=/w/project/build/t x"
    assert parse_line(result_ok_exe("/a/b")) == {"kind": "result", "ok": True, "exe": "/a/b"}
    assert parse_line(result_ok_project("/a/p")) == {"kind": "result", "ok": True, "project": "/a/p"}


def test_build_fraction():
    assert prep.build_fraction("[12/345] Building C object x.o\n") == (12 / 345, "12/345")
    assert prep.build_fraction("[ 45%] Building C object x.o") == (0.45, "45 %")
    assert prep.build_fraction("[100%] Built target t") == (1.0, "100 %")
    assert prep.build_fraction("In file included from [x]") is None
    assert prep.build_fraction("") is None


def test_run_streams_lines_to_log_and_callback(tmp_path):
    log = tmp_path / "prepare.log"
    seen = []
    prep.run([sys.executable, "-c", "print('[1/2] a'); print('[2/2] b')"], str(log), on_line=seen.append)
    assert seen == ["[1/2] a\n", "[2/2] b\n"]
    assert log.read_text().endswith("[1/2] a\n[2/2] b\n")
    with pytest.raises(SystemExit) as e:                    # what it always raised and printed
        prep.run([sys.executable, "-c", "print('oops'); raise SystemExit(3)"], str(log), on_line=seen.append)
    assert isinstance(e.value, prep.CommandFailed)
    assert str(e.value.code).startswith("command failed (3): ")
    assert "oops" in str(e.value.code) and "failed (exit 3), see " in e.value.short()


def test_build_progress_throttles(capsys):
    prep._REP, prep._STEP = Reporter(ids=DESKTOP_STEP_IDS), 5
    try:
        bp = prep.BuildProgress()
        bp("[1/2] Re-checking globbed directories...\n")
        for i in range(1, 1001):
            bp("[%d/1000] Building C object f%d.o\n" % (i, i))
    finally:
        prep._REP = None
    lines = [p for _, p in _protocol(capsys.readouterr().out)]
    assert 80 <= len(lines) <= 90                          # one per percent, not one per file
    assert lines[-1]["pct"] == 90 and lines[-1]["msg"] == "compiling 1000/1000"
    assert all(a["pct"] <= b["pct"] for a, b in zip(lines, lines[1:])) and lines[0]["pct"] == 5


# ── --progress ──

@needs_pipeline
def test_no_build_output_unchanged_by_progress(tmp_path, capsys):
    """The human lines are the same with and without --progress; --progress
    adds protocol lines only."""
    xbe = _xbe(tmp_path)
    assert prep.main([xbe, "--work", str(tmp_path / "a"), "--no-build"]) == 0
    plain = capsys.readouterr().out
    assert "XBA_" not in plain
    assert prep.main([xbe, "--work", str(tmp_path / "b"), "--no-build", "--progress"]) == 0
    out = capsys.readouterr().out
    human = [ln for ln in out.splitlines() if not ln.startswith("XBA_")]
    assert human == plain.replace(str(tmp_path / "a"), str(tmp_path / "b")).splitlines()
    proto = _protocol(out)
    assert proto[-1][0] == "XBA_RESULT ok project=%s" % (tmp_path / "b" / "project")
    steps = [p for _, p in proto if p["kind"] == "progress"]
    assert [p["id"] for p in steps if p["pct"] == 0][:4] == ["identify", "analyse", "project", "lift"]
    assert all(p["steps"] == 5 and p["id"] == DESKTOP_STEP_IDS[p["step"] - 1] for p in steps)
    assert {(p["step"], p["pct"]) for p in steps} >= {(1, 100), (2, 100), (3, 100), (4, 100)}


@needs_pipeline
def test_full_prepare_from_an_image(tmp_path, capsys, monkeypatch):
    """XBA-126: the build reads the image in place (build/game links to it), nothing is extracted."""
    _fake_build(monkeypatch)
    iso = _image(tmp_path)
    work = tmp_path / "w"
    assert prep.main([str(iso), "--work", str(work), "--progress"]) == 0
    proto = _protocol(capsys.readouterr().out)
    exe = str(work / "project" / "build" / NAME)
    assert proto[-1][0] == "XBA_RESULT ok exe=" + exe
    build = [p for _, p in proto if p["kind"] == "progress" and p["id"] == "build"]
    pcts = [p["pct"] for p in build]
    assert pcts == sorted(pcts) and pcts[-1] == 100
    assert any(p["msg"] == "compiling 2/4" for p in build)
    assert not any(p["msg"].startswith("extracting the disc") for p in build)
    assert not (work / "game").exists()
    assert os.path.realpath(work / "project" / "build" / "game") == os.path.realpath(iso)
    # And --check agrees.
    st = prep.check_state(str(iso), str(work))
    assert (st["state"], st["reasons"], st["exe"]) == ("ready", [], exe)


@needs_pipeline
def test_full_prepare_from_an_image_extracted(tmp_path, capsys, monkeypatch):
    """--extract: the image is unpacked into <work>/game as before XBA-126."""
    _fake_build(monkeypatch)
    iso = _image(tmp_path)
    work = tmp_path / "w"
    assert prep.main([str(iso), "--work", str(work), "--progress", "--extract"]) == 0
    proto = _protocol(capsys.readouterr().out)
    exe = str(work / "project" / "build" / NAME)
    assert proto[-1][0] == "XBA_RESULT ok exe=" + exe
    build = [p for _, p in proto if p["kind"] == "progress" and p["id"] == "build"]
    assert any(p["msg"].startswith("extracting the disc") for p in build)
    assert (work / "game" / "media" / "data.bin").read_bytes() == b"\x02" * 3000
    # And --check agrees.
    st = prep.check_state(str(iso), str(work))
    assert (st["state"], st["reasons"], st["exe"]) == ("ready", [], exe)


@needs_pipeline
def test_compile_failure(tmp_path, capsys, monkeypatch):
    _fake_build(monkeypatch, fail=True)
    work = tmp_path / "w"
    xbe = _xbe(tmp_path)
    assert prep.main([xbe, "--work", str(work), "--progress"]) == 1
    cap = capsys.readouterr()
    line, p = _protocol(cap.out)[-1]
    assert p == {"kind": "result", "ok": False, "code": "COMPILE_FAILED",
                 "msg": "cmake --build failed (exit 1), see %s" % (work / "prepare.log")}
    assert "error: x.c:1: boom" in cap.err                  # the tail, as without --progress
    st = prep.check_state(xbe, str(work))
    assert (st["state"], st["reasons"]) == ("absent", ["not_built"])


@pytest.mark.parametrize("make,code", [
    (lambda t: t, "NO_XBE"),
    (lambda t: (t / "default.xbe").write_bytes(b"\0" * 4096) and t, "BAD_XBE"),
])
def test_early_errors(tmp_path, capsys, make, code):
    src = tmp_path / "src"
    src.mkdir()
    assert prep.main([str(make(src)), "--work", str(tmp_path / "w"), "--progress"]) == 1
    cap = capsys.readouterr()
    line, p = _protocol(cap.out)[-1]
    assert p["kind"] == "result" and not p["ok"] and p["code"] == code, line
    assert cap.err.strip()
    # Without --progress: no protocol line, the SystemExit as before.
    with pytest.raises(SystemExit):
        prep.main([str(src), "--work", str(tmp_path / "w2")])
    assert "XBA_" not in capsys.readouterr().out


@needs_pipeline
def test_sigterm_cancels(tmp_path, capsys, monkeypatch):
    real = prep.run

    def run(args, log, cwd=prep.ROOT, on_line=None, tick=None):
        if "tools.func_id" in args:
            os.kill(os.getpid(), signal.SIGTERM)
        return real(args, log, cwd, on_line)
    monkeypatch.setattr(prep, "run", run)
    before = signal.getsignal(signal.SIGTERM)
    assert prep.main([_xbe(tmp_path), "--work", str(tmp_path / "w"), "--progress", "--no-build"]) == 130
    line, p = _protocol(capsys.readouterr().out)[-1]
    assert (p["ok"], p["code"]) == (False, "CANCELLED")
    assert signal.getsignal(signal.SIGTERM) is before      # handlers put back


def test_sigterm_kills_the_child(tmp_path):
    """A real SIGTERM to `python -m tools.prepare --progress` mid-command:
    CANCELLED, exit 130, and the command it was running is gone."""
    pidfile = tmp_path / "child.pid"
    script = tmp_path / "slow.py"
    script.write_text("import os, time\nopen(%r, 'w').write(str(os.getpid()))\ntime.sleep(60)\n" % str(pidfile))
    code = ("import sys, signal; sys.path.insert(0, %r)\n"
            "from tools.prepare import __main__ as prep\n"
            "from tools.prepare.progress import DESKTOP_STEP_IDS, Reporter\n"
            "prep._REP = Reporter(ids=DESKTOP_STEP_IDS)\n"
            "def body(a):\n"
            "    prep.step(2, 'analysing')\n"
            "    prep.run([sys.executable, %r], %r)\n"
            "prep.desktop = body\n"
            "sys.exit(prep.main(['x', '--progress']))\n") % (ROOT, str(script), str(tmp_path / "log"))
    p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    import time
    for _ in range(200):
        if pidfile.exists() and pidfile.read_text():
            break
        time.sleep(0.05)
    child = int(pidfile.read_text())
    p.send_signal(signal.SIGTERM)
    out, _ = p.communicate(timeout=10)
    assert p.returncode == 130
    last = parse_line(out.splitlines()[-1])
    assert (last["ok"], last["code"]) == (False, "CANCELLED")
    with pytest.raises(OSError):
        for _ in range(100):
            os.kill(child, 0)
            time.sleep(0.05)


def _gone(pid):
    """True once pid has exited: no such process, or a zombie (an orphan
    reparented to a PID 1 that does not reap, as in a container)."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] == "Z"
    except OSError:
        pass
    try:
        os.kill(pid, 0)
    except OSError:
        return True
    return False


def test_sigterm_kills_the_whole_tree(tmp_path):
    """The command's own children go too (cmake --build: ninja and the
    compilers, not only cmake): SIGTERM mid-command leaves no grandchild."""
    pidfile = tmp_path / "grandchild.pid"
    cc = tmp_path / "compiler.py"
    cc.write_text("import os, time\nopen(%r, 'w').write(str(os.getpid()))\ntime.sleep(60)\n" % str(pidfile))
    script = tmp_path / "build.py"
    # A "cmake" that starts a "compiler" and waits for it, like cmake --build.
    script.write_text("import subprocess, sys\nsubprocess.Popen([sys.executable, %r]).wait()\n" % str(cc))
    code = ("import sys, signal; sys.path.insert(0, %r)\n"
            "from tools.prepare import __main__ as prep\n"
            "from tools.prepare.progress import DESKTOP_STEP_IDS, Reporter\n"
            "prep._REP = Reporter(ids=DESKTOP_STEP_IDS)\n"
            "def body(a):\n"
            "    prep.step(5, 'building')\n"
            "    prep.run([sys.executable, %r], %r, on_line=lambda line: None)\n"
            "prep.desktop = body\n"
            "sys.exit(prep.main(['x', '--progress']))\n") % (ROOT, str(script), str(tmp_path / "log"))
    p = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    import time
    for _ in range(200):
        if pidfile.exists() and pidfile.read_text():
            break
        time.sleep(0.05)
    grandchild = int(pidfile.read_text())
    try:
        p.send_signal(signal.SIGTERM)
        out, _ = p.communicate(timeout=10)
        assert p.returncode == 130
        last = parse_line(out.splitlines()[-1])
        assert (last["ok"], last["code"]) == (False, "CANCELLED")
        for _ in range(100):
            if _gone(grandchild):
                break
            time.sleep(0.05)
        assert _gone(grandchild), "the build's grandchild %d still runs" % grandchild
    finally:
        try:
            os.kill(grandchild, signal.SIGKILL)
        except OSError:
            pass


def test_kill_tree_unit():
    """proctree on its own: a child and its grandchild, one kill_tree."""
    from tools.prepare import proctree
    p = subprocess.Popen([sys.executable, "-c",
                          "import subprocess, sys; print(subprocess.Popen([sys.executable, '-c', "
                          "'import time; time.sleep(60)']).pid, flush=True); import time; time.sleep(60)"],
                         stdout=subprocess.PIPE, text=True, **proctree.popen_kw())
    grandchild = int(p.stdout.readline())
    proctree.kill_tree(p)
    p.wait(timeout=10)
    p.stdout.close()
    import time
    for _ in range(100):
        if _gone(grandchild):
            break
        time.sleep(0.05)
    assert _gone(grandchild)
    proctree.kill_tree(p)           # again, on a dead group: no error


# ── --check ──

def _tree(path):
    out = {}
    for d, _, files in os.walk(path):
        for fn in files:
            p = os.path.join(d, fn)
            st = os.stat(p)
            out[p] = (st.st_size, st.st_mtime_ns)
    return out


def _check_cli(*args):
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + [str(a) for a in args] + ["--check", "--json"],
                       cwd=ROOT, capture_output=True, text=True)
    lines = p.stdout.splitlines()
    assert len(lines) == 1, p.stdout + p.stderr
    return p.returncode, json.loads(lines[0])


def test_check_absent_writes_nothing(tmp_path):
    iso = _image(tmp_path)
    before = _tree(tmp_path)
    rc, st = _check_cli(iso)
    assert rc == 0
    assert st == {"state": "absent", "reasons": [], "exe": None, "title": "SYNTH", "title_id": "12345678",
                  "env_force": {}, "work": str(tmp_path / "12345678-work")}
    assert _tree(tmp_path) == before and not (tmp_path / "12345678-work").exists()


def test_check_errors(tmp_path):
    rc, st = _check_cli(tmp_path)
    assert rc == 1 and st["state"] == "error" and st["code"] == "NO_XBE"


def _ready_work(tmp_path, monkeypatch):
    """A work folder as a prepare leaves it, without running one."""
    work = tmp_path / "w"
    (work / "project" / "build").mkdir(parents=True)
    (work / "project" / "CMakeLists.txt").write_text("project(%s C)\n" % NAME)
    (work / "project" / "build" / NAME).write_text("")
    monkeypatch.setattr(prep, "toolkit_version", lambda: "c0ffee")
    stamp = {"xbe": prep.sha256(TEST_XBE), "toolkit": "c0ffee", "profile": prep.profile_digest(None),
             "discovered": "", "analysis": True, "lift": True, "built": True}
    (work / "stamp.json").write_text(json.dumps(stamp))
    return work, stamp


def test_check_states(tmp_path, monkeypatch):
    work, stamp = _ready_work(tmp_path, monkeypatch)
    exe = str(work / "project" / "build" / NAME)

    def state():
        before = _tree(work)
        st = prep.check_state(TEST_XBE, str(work))
        assert _tree(work) == before
        return st["state"], st["reasons"], st["exe"]
    assert state() == ("ready", [], exe)
    monkeypatch.setattr(prep, "toolkit_version", lambda: "beef")
    assert state() == ("stale", ["toolkit"], exe)
    monkeypatch.setattr(prep, "toolkit_version", lambda: "c0ffee")
    # A run found something new: "improvements found while playing".
    (work / "discovered.jsonl").write_text('{"t": "icall", "va": "0x00011000"}\n{"t": "icall", "va": "0x0001')
    assert state() == ("stale", ["discovered"], exe)
    (work / "discovered-rejected.json").write_text('["0x00011000"]')
    assert state() == ("ready", [], exe)
    (work / "discovered-seeds.json").write_text('[{"start": "0x00011000"}]')
    assert state() == ("stale", ["discovered"], exe)
    os.remove(work / "discovered-seeds.json")
    # A lift whose build did not finish.
    (work / "stamp.json").write_text(json.dumps(dict(stamp, built=False)))
    assert state() == ("stale", ["not_built"], exe)
    # An older stamp without "built" is taken as built.
    (work / "stamp.json").write_text(json.dumps({k: v for k, v in stamp.items() if k != "built"}))
    assert state() == ("ready", [], exe)
    (work / "stamp.json").write_text(json.dumps(dict(stamp, xbe="0" * 64)))
    assert state() == ("stale", ["xbe"], exe)
    os.remove(work / "project" / "build" / NAME)
    assert state() == ("absent", ["not_built"], None)


def test_check_keys_on_the_content_digests(tmp_path, monkeypatch):
    """XBA-134: the toolkit is compared by its content digest (the Android packs' one), not by the commit: a commit
    that changes no file of the toolkit tree keeps the title ready. The runtime the executable links (src/) has its
    own digest: a change there asks for a rebuild ("runtime"), not a lift. A stamp from before keeps the commit rule
    (test_check_states)."""
    from tools.prepare import toolkit
    work, stamp = _ready_work(tmp_path, monkeypatch)
    exe = str(work / "project" / "build" / NAME)
    monkeypatch.setattr(toolkit, "digest", lambda root=None: "d" * 64)
    monkeypatch.setattr(toolkit, "runtime_digest", lambda root=None: "r" * 64)
    (work / "stamp.json").write_text(json.dumps(dict(stamp, toolkit_sha256="d" * 64, runtime_sha256="r" * 64)))

    def state():
        st = prep.check_state(TEST_XBE, str(work))
        return st["state"], st["reasons"], st["exe"]
    assert state() == ("ready", [], exe)
    monkeypatch.setattr(prep, "toolkit_version", lambda: "beef")          # another commit, the same files
    assert state() == ("ready", [], exe)
    monkeypatch.setattr(toolkit, "digest", lambda root=None: "e" * 64)    # the toolkit's files changed
    assert state() == ("stale", ["toolkit"], exe)
    monkeypatch.setattr(toolkit, "digest", lambda root=None: "d" * 64)
    monkeypatch.setattr(toolkit, "runtime_digest", lambda root=None: "s" * 64)   # only the runtime
    assert state() == ("stale", ["runtime"], exe)
    (work / "stamp.json").write_text(json.dumps(dict(stamp, toolkit_sha256="d" * 64, runtime_sha256="r" * 64,
                                                     built=False)))
    assert state() == ("stale", ["not_built"], exe)                       # the build is coming anyway


def test_the_runtime_digest_covers_the_runtime_only(tmp_path):
    from tools.prepare import toolkit
    (tmp_path / "src" / "kernel").mkdir(parents=True)
    (tmp_path / "src" / "kernel" / "a.c").write_text("int a;\n")
    (tmp_path / "CMakeLists.txt").write_text("project(x C)\n")
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "x.md").write_text("doc\n")
    toolkit._cache.clear()
    a = toolkit.runtime_digest(str(tmp_path))
    (tmp_path / "docs" / "x.md").write_text("another doc\n")
    toolkit._cache.clear()
    assert toolkit.runtime_digest(str(tmp_path)) == a
    (tmp_path / "src" / "kernel" / "a.c").write_text("int b;\n")
    toolkit._cache.clear()
    assert toolkit.runtime_digest(str(tmp_path)) != a
    toolkit._cache.clear()


def test_check_env_force_and_title(tmp_path, monkeypatch):
    t = tmp_path / "profiles" / "titles" / "12345678"
    t.mkdir(parents=True)
    (t / "title.json").write_text(json.dumps({"name": "Synthetic Title", "env_force": {"RECOMP_LSFG": "0"}}))
    monkeypatch.setattr(prep, "PROFILES", str(tmp_path / "profiles"))
    work, stamp = _ready_work(tmp_path, monkeypatch)
    st = prep.check_state(TEST_XBE, str(work))
    assert (st["title"], st["env_force"]) == ("Synthetic Title", {"RECOMP_LSFG": "0"})
    assert (st["state"], st["reasons"]) == ("stale", ["profile"])     # the stamp has the profile-less digest
    (t / "title.json").write_text(json.dumps({"env_force": {"lsfg": "0"}}))
    rc = prep.main([TEST_XBE, "--work", str(work), "--check", "--json"])
    assert rc == 1


def test_a_second_prepare_of_the_same_work_folder_is_refused(tmp_path, capsys):
    # A launcher and a terminal preparing one title at once rewrote the same
    # gen.new under each other (KOTOR: LIFT_FAILED). The second one now stops.
    import sys
    if sys.platform == "win32":
        import pytest
        pytest.skip("fcntl lock")
    from tools.prepare import __main__ as pm
    held = pm._lock_work(str(tmp_path))
    assert held
    try:
        rc = pm.main([str(tmp_path / "missing.iso"), "--work", str(tmp_path), "--progress"])
    finally:
        held.close()
    out = capsys.readouterr().out
    assert rc == 1 and "XBA_RESULT error code=BUSY" in out, out
    again = pm._lock_work(str(tmp_path))
    assert again
    again.close()


def test_a_long_silent_step_ticks_with_elapsed_time(tmp_path, monkeypatch):
    # Fable's disassembly is minutes of silence; the launcher sat on
    # "analyse 5 % disassembling". With tick=, progress creeps and says how long.
    import sys
    from tools.prepare import __main__ as pm
    from tools.prepare.progress import Reporter, parse_line
    import io
    buf = io.StringIO()
    monkeypatch.setattr(pm, "_REP", Reporter(stream=buf))
    monkeypatch.setattr(pm, "_STEP", 2)
    real_wait = pm.subprocess.Popen.wait
    calls = {"n": 0}

    def fast_wait(self, timeout=None):
        if timeout is not None and calls["n"] < 3:
            calls["n"] += 1
            raise pm.subprocess.TimeoutExpired(self.args, timeout)
        return real_wait(self)
    monkeypatch.setattr(pm.subprocess.Popen, "wait", fast_wait)
    clock = {"t": 1000.0}

    class FakeTime:
        @staticmethod
        def time():
            clock["t"] += 30.0          # each look at the clock: 30 s later
            return clock["t"]
    monkeypatch.setattr(pm, "time", FakeTime)
    pm.run([sys.executable, "-c", "pass"], str(tmp_path / "log"), tick=(5, 58, "disassembling", 90))
    lines = [parse_line(l) for l in buf.getvalue().splitlines() if l.startswith("XBA_PROGRESS")]
    assert len(lines) == 3, buf.getvalue()
    pcts = [l["pct"] for l in lines]
    assert all(5 <= p < 58 for p in pcts) and pcts == sorted(pcts), pcts
    assert all("disassembling (" in l["msg"] for l in lines), lines
