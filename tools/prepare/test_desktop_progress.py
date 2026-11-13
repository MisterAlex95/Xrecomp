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

    def run(args, log, cwd=prep.ROOT, on_line=None):
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

    def run(args, log, cwd=prep.ROOT, on_line=None):
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
