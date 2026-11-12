"""tools.compat: stages and blockers from a run's status lines (no title needed)."""

import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.compat.__main__ import classify, report      # noqa: E402


def line(ms, flips=0, frames=0, lit=0, pad=0, threads=0, audio=0):
    return {"ms": ms, "flips": flips, "frames": frames, "lit": lit, "pad_reads": pad,
            "threads": threads, "audio_ms": audio, "discovered": 0}


def playing(seconds, start_ms=1000):
    """A title flipping at 60 Hz with a picture that keeps changing, reading the pad."""
    return [line(t, flips=t * 60 // 1000, frames=t // 500, lit=1, pad=t // 16, threads=4)
            for t in range(start_ms, seconds * 1000 + 1, 1000)]


def test_runtime_never_started():
    assert classify([], [], None, 180, 60)[:2] == ("prepares", "no status line: the runtime did not start")


def test_boots_but_no_flip_names_the_untranslated_instruction():
    st = [line(t, threads=2) for t in range(1000, 60001, 1000)]
    log = "[UNIMPL] untranslated instruction REACHED: `invlpg [eax]` at 0x00012345 (a no-op)"
    stage, blocker, summary = classify(st, [], None, 60, 60, log)
    assert stage == "boots" and blocker.startswith("no flip; first untranslated instruction reached: invlpg [eax]")
    stage, blocker, _ = classify(st, [], None, 60, 60, "", [{"t": "icall", "va": "0x00045678"}])
    assert blocker == "no flip; last discovery: icall 0x00045678"


def test_black_screen_is_boots():
    st = [line(t, flips=t // 16, frames=1, lit=0, threads=3) for t in range(1000, 30001, 1000)]
    assert classify(st, [8000], None, 30, 60)[:2] == ("boots", "nothing lit on screen")


def test_renders_without_response():
    # Lit and flipping, but the picture never changes after a press (a frozen logo).
    st = [line(t, flips=t // 16, frames=2, lit=1, pad=t // 16) for t in range(1000, 30001, 1000)]
    assert classify(st, [8000, 10000], None, 30, 60)[:2] == ("renders", "no response to the pad")


def test_sustained():
    st = playing(180)
    stage, blocker, summary = classify(st, [8000, 10000], None, 180, 60)
    assert (stage, blocker) == ("sustained", None) and summary["lit"] == 1


def test_responsive_but_too_short_to_be_sustained():
    st = playing(30)
    assert classify(st, [8000], None, 30, 60)[:2] == ("responsive", "not sustained")


def test_hang_after_rendering():
    st = playing(100) + [line(t, flips=6000, frames=200, lit=1, pad=t // 16) for t in range(101000, 180001, 1000)]
    stage, blocker, _ = classify(st, [8000], None, 180, 60)
    assert stage == "responsive" and blocker.startswith("hang: no flip for the last")


def test_crash_and_exit():
    st = playing(40)
    stage, blocker, _ = classify(st, [8000], ("signal", 11), 40, 60, "  [FAULT] write at 0x00000000 from sub_00012345")
    assert stage == "responsive" and blocker.startswith("crash (signal 11): [FAULT] write at 0x00000000")
    assert classify(st, [8000], ("exit", 0), 40, 60)[1] == "exited with code 0 after 40 s"


def test_a_called_stub_is_named_as_a_blocker():
    # XBA-115: the runtime records {"t":"stub","ord":N,...} the first time a
    # stub kernel route is called; the matrix names it instead of a bare hang.
    st = [line(t, threads=2) for t in range(1000, 60001, 1000)]
    disc = [{"t": "icall", "va": "0x00045678"},
            {"t": "stub", "ord": 136, "name": "KeRemoveQueue", "ret": "0x00012345"},
            {"t": "stub", "ord": 206, "name": "NtQueueApcThread", "ret": "0x00012399"}]
    stage, blocker, _ = classify(st, [], None, 60, 60, "", disc)
    assert stage == "boots"
    assert blocker == ("no flip; last discovery: icall 0x00045678; calls stub ordinal 136 (KeRemoveQueue),"
                       " ordinal 206 (NtQueueApcThread)")
    # Also beside a stage blocker, but not beside a crash, which has its own cause.
    st2 = [line(t, flips=t // 16, frames=1, lit=0, threads=3) for t in range(1000, 30001, 1000)]
    assert classify(st2, [8000], None, 30, 60, "", disc[1:2])[1] == \
        "nothing lit on screen; calls stub ordinal 136 (KeRemoveQueue)"
    assert "stub" not in classify(playing(40), [8000], ("signal", 11), 40, 60, "", disc)[1]
    # Only stubs: no "last discovery: stub None".
    assert classify(st, [], None, 60, 60, "", disc[1:2])[1] == "no flip; calls stub ordinal 136 (KeRemoveQueue)"


def test_a_launch_of_another_image_is_named():
    st = playing(20)
    log = "  [KERNEL] launch of 'd:\\game.xbe' is not this title (default.xbe): exiting"
    assert classify(st, [8000], ("exit", 0), 20, 60, log)[1] == \
        "launches another XBE: d:\\game.xbe (multi-XBE discs, XBA-82)"


def test_report_table():
    rows = [{"source": "a", "stage": "boots", "blocker": "no flip", "toolkit": "abc"},
            {"source": "b", "title_id": "4D5300D1", "stage": "sustained", "toolkit": "abc", "xdk": 5849},
            {"source": "a", "stage": "renders", "blocker": "x|y", "toolkit": "def"}]
    t = report(rows).splitlines()
    assert t[2].startswith("| b | sustained |") and "| a | renders | renders | x/y |" in t[3]


FAKE_TITLE = r'''#!/usr/bin/env python3
# A stand-in for a title binary: writes the status file like the runtime and
# changes its picture when the runner presses (RECOMP_PAD_LIVE). MODE: play, hang, crash.
import json, os, signal, sys, time
st, live, mode = os.environ["RECOMP_STATUS"], os.environ["RECOMP_PAD_LIVE"], sys.argv[0].rsplit("-", 1)[-1]
t0, frames, pad, seen = time.time(), 1, 0, 0
while True:
    time.sleep(0.25)
    ms = int((time.time() - t0) * 1000)
    with open(live) as f:
        n = len(f.read().split())
    if n > seen:
        frames, seen = frames + (n - seen), n
    pad += 15
    flips = ms * 60 // 1000 if mode != "hang" or ms < 3000 else 180
    with open(st, "a") as f:
        f.write(json.dumps({"ms": ms, "flips": flips, "frames": frames, "lit": 1, "audio_ms": 0,
                            "pad_reads": pad, "threads": 3, "discovered": 0}) + "\n")
    if mode == "crash" and ms > 4000:
        os.kill(os.getpid(), signal.SIGSEGV)
'''


def _fake(tmp_path, mode):
    p = tmp_path / ("title-" + mode)
    p.write_text(FAKE_TITLE)
    p.chmod(0o755)
    return str(p)


def test_boot_presses_reads_and_kills(tmp_path, monkeypatch):
    import tools.compat.__main__ as cm
    if sys.platform == "win32":
        import pytest
        pytest.skip("POSIX process control")
    monkeypatch.setattr(cm, "PRESS_FIRST", 1.0)
    monkeypatch.setattr(cm, "PRESS_EVERY", 0.5)
    status, presses, exit_info, run_s, _ = cm.boot(_fake(tmp_path, "play"), str(tmp_path), 6)
    assert exit_info is None and 5.5 < run_s < 9 and len(presses) >= 8
    assert (tmp_path / "pad-live-0.txt").read_text().split()[:4] == ["start", "start", "start", "a"]
    stage, blocker, summary = classify(status, presses, exit_info, run_s, 3)
    assert stage == "sustained" and summary["frames"] > 5, (stage, blocker, status[-1])
    status, presses, exit_info, run_s, _ = cm.boot(_fake(tmp_path, "crash"), str(tmp_path), 10)
    assert exit_info == ("signal", 11) and run_s < 9
    assert classify(status, presses, exit_info, run_s, 3)[1].startswith("crash (signal 11)")


def test_matrix_and_regressions():
    from tools.compat.__main__ import matrix, plane_sync, regressions
    res = [{"source": "Fable", "title_id": "4D5300D1", "stage": "sustained", "toolkit": "aaa", "xdk": 5849},
           {"source": "Burnout 3", "title_id": "4541005B", "stage": "renders", "toolkit": "aaa",
            "layers": ["xdk/d3d8", "engines/renderware"]},
           {"source": "Fable", "title_id": "4D5300D1", "stage": "boots", "blocker": "hang: no flip", "toolkit": "bbb"},
           {"source": "Burnout 3", "title_id": "4541005B", "stage": "sustained", "toolkit": "bbb"}]
    reg = regressions(res)
    assert reg == [{"title": "4D5300D1", "source": "Fable", "from": "sustained", "to": "boots",
                    "blocker": "hang: no flip", "toolkit_from": "aaa", "toolkit_to": "bbb"}]
    md = matrix(res)
    assert md.startswith("**2 titles.** sustained: 1 (50 %); boots: 1 (50 %)")
    assert "| Burnout 3 | 4541005B | sustained | up from renders |" in md
    assert "| Fable | 4D5300D1 | boots | **down from sustained** | hang: no flip |  |" in md
    assert "**Regressions** (a Bug each): Fable sustained -> boots (aaa -> bbb)" in md
    calls = []

    class R:
        stdout = "Regression: Fable sustained -> boots (toolkit bbb)"

    def fake_run(cmd, **kw):
        calls.append(cmd)
        return R()
    # The page is written; the Bug is already open, so none is created.
    assert plane_sync(res, run=fake_run) == []
    assert calls[0][:3] == ["plane", "page", "write"]
    R.stdout = ""
    calls.clear()
    assert plane_sync(res, run=fake_run) == ["Regression: Fable sustained -> boots (toolkit bbb)"]
    assert any(c[:2] == ["plane", "create"] and "Bug" in c for c in calls)
    assert plane_sync(res, dry_run=True) == ["Regression: Fable sustained -> boots (toolkit bbb)"]


def test_the_last_screenshot_is_kept(tmp_path):
    from tools.compat.__main__ import keep_last_shot
    shots = tmp_path / "shots-0"
    assert keep_last_shot(str(shots), str(tmp_path / "shot-0.bmp")) is None      # no folder
    shots.mkdir()
    assert keep_last_shot(str(shots), str(tmp_path / "shot-0.bmp")) is None      # nothing dumped
    for n in range(3):
        (shots / ("f%05d.bmp" % n)).write_bytes(b"BM%d" % n)
    assert keep_last_shot(str(shots), str(tmp_path / "shot-0.bmp")) == str(tmp_path / "shot-0.bmp")
    assert (tmp_path / "shot-0.bmp").read_bytes() == b"BM2" and not shots.exists()
