"""End to end on a synthetic XBE of our own (no game data): prepare, and with
XBA_E2E=1 also build and boot it through the compat runner.

The XBE (tools.xbe_ident's make_xbe) has a retail-encoded entry point, an
empty kernel thunk table and a library table naming XAPILIB, D3D8 and
DSOUND, so the XDK profile layers apply and their C goes into the project;
its code is one function, `jmp $`.

- Always: tools.prepare --no-build lifts it: the generated C defines
  xbe_entry_point, and src/title holds the layers' C and xba_layers.c.
- XBA_E2E=1 (needs CMake, a compiler and SDL2, as in tools/docker's images):
  the full prepare builds the title against the runtime, and tools.compat
  boots it headless: the runtime starts and writes its status file. A loop
  that never flips is "prepares" with blocker "no flip".
"""

import json
import os
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.xbe_ident.test_xbe_ident import make_xbe     # noqa: E402

LIBS = (("XAPILIB", 5849, 0), ("D3D8", 5849, 0), ("DSOUND", 5849, 0))
SPIN = b"\x55\x8b\xec\xeb\xfe"          # push ebp; mov ebp, esp; jmp $


def _xbe(tmp_path):
    d = tmp_path / "disc"
    d.mkdir()
    return make_xbe(d / "default.xbe", libs=LIBS, text=SPIN, title_id=0x58424101, title="Smoke")


def _prepare(xbe, work, *extra):
    return subprocess.run([sys.executable, "-m", "tools.prepare", xbe, "--work", str(work)] + list(extra),
                          cwd=ROOT, capture_output=True, text=True, timeout=900)


def test_prepare_lifts_a_synthetic_title(tmp_path):
    work = tmp_path / "work"
    r = _prepare(_xbe(tmp_path), work, "--no-build")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "profile layers: xdk/d3d8 xdk/dsound xdk/xapi" in r.stdout
    gen = work / "project" / "src" / "recomp" / "gen"
    text = "".join(p.read_text(errors="replace") for p in gen.glob("*.c"))
    assert "xbe_entry_point" in text
    title = sorted(os.listdir(work / "project" / "src" / "title"))
    assert "xba_layers.c" in title and "xdk_d3d8__d3d8.c" in title


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_build_and_boot_through_the_compat_runner(tmp_path):
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    xbe = _xbe(tmp_path)
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", os.path.dirname(xbe),
                        "--work-root", str(tmp_path / "cw"), "--seconds", "8", "--sustain", "4", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    res = json.loads(out.read_text().splitlines()[-1])
    assert res["stage"] == "prepares" and res["blocker"].startswith("no flip"), res
    work = tmp_path / "cw" / "disc"
    status = (work / "status-0.jsonl").read_text().splitlines()
    assert status and json.loads(status[-1])["ms"] > 0          # the runtime started and reported


def _heal_xbe(tmp_path):
    """A title whose thread routine the static analysis cannot see: the entry
    computes its address (mov eax, 0x11000; add eax, 0x40) and hands it to
    PsCreateSystemThreadEx through the kernel thunk table (ordinal 255); the
    routine waits on a zero dword for ever. Then loops."""
    import struct
    entry = bytes.fromhex("558bec" "b800100100" "83c040" "50" + "6a00" * 9 + "ff1500080100" "ebfe")
    entry += b"\x90" * (0x40 - len(entry))
    routine = bytes.fromhex("833d0011010000" "74f7" "c3")   # L: cmp dword [0x11100], 0 / je L / ret
    text = entry + routine
    text += b"\x00" * (0x110 - len(text))
    d = tmp_path / "heal"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=text, title_id=0x58424102, title="Heal")
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<II", b, 0x800, 0x80000000 | 255, 0)   # thunk table: PsCreateSystemThreadEx, end
    open(p, "wb").write(b)
    return str(d)


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_the_self_healing_loop(tmp_path):
    """XBA-64 + XBA-97 + XBA-65 end to end: the first run meets a thread
    routine with no dispatch entry and records it; the next prepare seeds it
    from discovered.jsonl and rebuilds; the second run starts the thread."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    disc, out, cw = _heal_xbe(tmp_path), tmp_path / "results.jsonl", tmp_path / "cw"

    def run():
        r = subprocess.run([sys.executable, "-m", "tools.compat", "run", disc, "--work-root", str(cw),
                            "--seconds", "6", "--sustain", "3", "--out", str(out)],
                           cwd=ROOT, capture_output=True, text=True, timeout=1800)
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
        return json.loads(out.read_text().splitlines()[-1])

    first = run()
    work = cw / "heal"
    disc_lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    assert {"t": "thread", "va": "0x00011040", "ctx1": "0x00000000"} in disc_lines, disc_lines
    assert first["runs"][0]["threads"] == 0
    second = run()
    seeds = json.loads((work / "discovered-seeds.json").read_text())
    assert [s["start"] for s in seeds] == ["0x00011040"]
    assert second["runs"][0]["threads"] >= 1 and second["stage"] == "boots", second


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_the_d3d_device_is_found_at_run_time(tmp_path):
    """XBA-80 in the real runtime: a title that links D3D8 and whose image
    holds a D3D-like device (its pointer slot at +0x24A8 naming it, a ring in
    the contiguous window, the put pointer inside) has it found by the XDK D3D
    layer's scan and recorded in discovered.jsonl -- no title profile."""
    import struct
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    dev_off = 0x400                                   # in .text, at 0x11400
    text = bytearray(SPIN + b"\x00" * (dev_off + 0x24A8 + 0x10 - len(SPIN)))
    dev = 0x00011000 + dev_off
    struct.pack_into("<I", text, dev_off + 0x00, 0x80012340)     # put
    struct.pack_into("<II", text, dev_off + 0x24, 0x80010000, 0x80090000)   # ring
    struct.pack_into("<I", text, dev_off + 0x24A8, dev)          # the pointer slot
    d = tmp_path / "d3d"
    d.mkdir()
    make_xbe(d / "default.xbe", libs=LIBS, text=bytes(text), title_id=0x58424103, title="D3D")
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "5", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "d3d"
    lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    assert {"t": "d3d", "va": "0x%08X" % dev, "slot": "0x%08X" % (dev + 0x24A8)} in lines, lines
    assert "[D3D] device found at 0x%08X" % dev in (work / "run-0.log").read_text()


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_wait_spin_is_caught_and_promoted(tmp_path):
    """XBA-93 in the real runtime: a title that builds a KEVENT by writing its
    header (no KeInitializeEvent), calls KeSetEvent on it once, then polls it
    with KeWaitForSingleObject in a loop. The waits fail at once (no host
    event yet), the spin is reported and recorded, and since a set was seen
    the event is promoted. Run with RECOMP_TITLE_KEVENTS=0: since title-built
    KEVENTs are guest events by default (d38f10b) there is nothing left to
    promote otherwise, and the loop is only a zero-timeout poll."""
    import struct
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    evt, tmo = 0x00011300, 0x00011320
    code = bytes.fromhex("558bec" "6a00" "6a00" "68") + struct.pack("<I", evt) + bytes.fromhex("ff1500080100")
    loop = (bytes.fromhex("68") + struct.pack("<I", tmo) + bytes.fromhex("6a00" "6a00" "6a00" "68")
            + struct.pack("<I", evt) + bytes.fromhex("ff1504080100"))
    loop += bytes([0xEB, (-(len(loop) + 2)) & 0xFF])            # jmp back to the push
    text = bytearray(code + loop)
    text += b"\x00" * (0x340 - len(text))
    struct.pack_into("<IIII", text, evt - 0x11000, 0x00040001, 0, evt + 8, evt + 8)   # sync event, unset
    d = tmp_path / "spin"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=bytes(text), title_id=0x58424104,
                 title="Spin")
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<III", b, 0x800, 0x80000000 | 145, 0x80000000 | 159, 0)   # KeSetEvent, KeWaitForSingleObject
    open(p, "wb").write(b)
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "5", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800,
                       env=dict(os.environ, RECOMP_TITLE_KEVENTS="0"))
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "spin"
    log = (work / "run-0.log").read_text()
    assert "wait spin: 2000 waits" in log and "on 0x%08X (synchronization event" % evt in log, log[-3000:]
    assert "promoted to a guest event" in log, log[-3000:]
    lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    assert any(l.get("t") == "wait_spin" and l.get("va") == "0x%08X" % evt for l in lines), lines


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_launch_of_another_image_hands_over(tmp_path):
    """XBA-82 (desktop): default.xbe launches d:\\game.xbe as the XDK's
    XLaunchNewImage does (a contiguous page holding the path at +8, stored in
    LaunchDataPage, then HalReturnToFirmware(2)). tools.prepare prepares both
    images (game.xbe is named in default.xbe's strings) and writes images.map;
    the kernel execs game.xbe's executable in the same process."""
    import struct
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    code = bytes.fromhex(
        "558bec"
        "6800100000" "ff1500080100"          # push 0x1000; call [MmAllocateContiguousMemory]
        "c74008443a5c67"                     # mov dword [eax+8], "D:\\g"
        "c7400c616d652e"                     # mov dword [eax+12], "ame."
        "c7401078626500"                     # mov dword [eax+16], "xbe\\0"
        "8b0d04080100" "8901"                # mov ecx, [LaunchDataPage]; mov [ecx], eax
        "6a02" "ff1508080100"                # push 2; call [HalReturnToFirmware]
        "ebfe")
    text = bytearray(code + b"\x00" * (0x100 - len(code)) + b"d:\\game.xbe\x00" + b"\x00" * 0x20)
    d = tmp_path / "launcher"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=bytes(text), title_id=0x58424105,
                 title="Launcher")
    b = bytearray(open(p, "rb").read())
    # MmAllocateContiguousMemory, LaunchDataPage (data export), HalReturnToFirmware, end
    struct.pack_into("<IIII", b, 0x800, 0x80000000 | 165, 0x80000000 | 164, 0x80000000 | 49, 0)
    open(p, "wb").write(b)
    make_xbe(d / "game.xbe", libs=(("XAPILIB", 5849, 0),), text=SPIN, title_id=0x58424105, title="Game2")
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "8", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "launcher"
    imap = (work / "images.map").read_text().splitlines()
    assert imap[0].startswith("default.xbe\t") and imap[1].startswith("game.xbe\t"), imap
    log = (work / "run-0.log").read_text(errors="replace")
    assert "launch of 'D:\\game.xbe': handing over to" in log, log[-3000:]
    assert "Game2" in log.split("handing over to", 1)[1], log[-3000:]     # game.xbe's runtime started
    res = json.loads(out.read_text().splitlines()[-1])
    assert "launches another XBE" not in (res.get("blocker") or ""), res


def _coroutine_xbe(tmp_path):
    """Two coroutines on two guest stacks (XBA-101). The entry (A) builds B's
    stack -- the four registers the switch pops, then B's entry, stored as an
    immediate as CreateFiber would -- and calls the switch 1000 times,
    checking after each that B ran once more; then HalReturnToFirmware(2),
    which ends the run with « [EXIT] ». B counts and switches back, for ever.
    The switch is Fable's shape: push ebx, ebp, esi, edi / xchg [slot], esp /
    pop them / ret. If B never runs (the switch returns as a plain C return),
    A's check fails and it spins."""
    import struct
    slot, a_n, b_n, b_esp = 0x11F00, 0x11F10, 0x11F14, 0x11EE8
    sw, b = 0x11040, 0x11060

    def rel(at, target):                 # e8 rel32 at `at`
        return struct.pack("<i", target - (at + 5))

    a = bytearray(bytes.fromhex("558bec"))
    a += b"\xc7\x05" + struct.pack("<II", slot, b_esp)           # mov [slot], B's esp
    a += b"\xc7\x05" + struct.pack("<II", b_esp + 16, b)         # mov [B's ret slot], B
    loop = len(a)
    a += b"\xff\x05" + struct.pack("<I", a_n)                   # inc dword [a_n]
    a += b"\xe8" + rel(0x11000 + len(a), sw)                    # call sw
    a += b"\xa1" + struct.pack("<I", a_n)                       # mov eax, [a_n]
    a += b"\x3b\x05" + struct.pack("<I", b_n)                   # cmp eax, [b_n]
    a += b"\x75\x0f"                                            # jne hang
    a += b"\x3d" + struct.pack("<I", 1000)                      # cmp eax, 1000
    a += b"\x72" + bytes([(loop - (len(a) + 2)) & 0xFF])        # jb loop
    a += bytes.fromhex("6a02" "ff1500080100")                   # push 2; call [HalReturnToFirmware]
    a += b"\xeb\xfe"                                            # hang: jmp $
    text = bytearray(a.ljust(sw - 0x11000, b"\xcc"))
    text += bytes.fromhex("53555657") + b"\x87\x25" + struct.pack("<I", slot) + bytes.fromhex("5f5e5d5b" "c3")
    text = text.ljust(b - 0x11000, b"\xcc")
    bb = b"\xff\x05" + struct.pack("<I", b_n)                   # inc dword [b_n]
    bb += b"\xe8" + rel(b + len(bb), sw)                        # call sw
    bb += b"\xeb" + bytes([(-(len(bb) + 2)) & 0xFF])            # jmp B
    text += bb
    text = text.ljust(0xF20, b"\x00")
    d = tmp_path / "coro"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=bytes(text), title_id=0x58424106,
                 title="Coroutines")
    xbe = bytearray(open(p, "rb").read())
    struct.pack_into("<II", xbe, 0x800, 0x80000000 | 49, 0)     # HalReturnToFirmware, end
    open(p, "wb").write(xbe)
    return d


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_two_coroutines_switch_stacks(tmp_path):
    """XBA-101 in the real pipeline: the lifter marks the switch, the runtime
    gives B a host stack of its own, and A sees B run between each of its
    1000 switches, then leaves. With RECOMP_STACK_SWITCH=0 (the behaviour
    before) B never runs and A spins."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    d, out, cw = _coroutine_xbe(tmp_path), tmp_path / "results.jsonl", tmp_path / "cw"

    def run(**env):
        r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(cw),
                            "--seconds", "5", "--sustain", "3", "--out", str(out)],
                           cwd=ROOT, capture_output=True, text=True, timeout=1800, env=dict(os.environ, **env))
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
        return (cw / "coro" / "run-0.log").read_text(errors="replace")

    log = run()
    assert "[STACK] context 1: new guest stack at esp 0x00011EFC, starts at 0x00011060" in log, log[-3000:]
    assert "[EXIT] the game returned to the dashboard (routine 2)" in log, log[-3000:]
    disc = cw / "coro" / "discovered.jsonl"
    lines = [json.loads(l) for l in disc.read_text().splitlines()] if disc.exists() else []
    assert not [l for l in lines if l.get("t") == "stack_switch"], lines

    log = run(RECOMP_STACK_SWITCH="0")
    assert "[STACK] context" not in log and "[EXIT]" not in log, log[-3000:]


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_check_run_as_the_device_makes_it(tmp_path):
    """XBA-130: the app's library check boots a title headed with
    device.check_env (no RECOMP_HEADLESS, no screenshots), presses through
    RECOMP_PAD_LIVE on the status clock, ends it by PID, and grades the files
    with `tools.compat grade`: same stage and blocker as `tools.compat run`,
    the desktop schema plus the device's fields."""
    import signal
    import time
    from tools.compat import device
    from tools.compat.__main__ import PRESS_EVERY, PRESS_FIRST, project_exe, read_jsonl
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    disc = os.path.dirname(_xbe(tmp_path))
    out = tmp_path / "compat-results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", disc, "--work-root", str(tmp_path / "cw"),
                        "--seconds", "8", "--sustain", "4", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    desk = json.loads(out.read_text().splitlines()[-1])
    work = tmp_path / "cw" / "disc"
    exe = project_exe(str(work))
    run = tmp_path / "check" / "run" / "58424101"
    run.mkdir(parents=True)
    (run / "pad-live.txt").write_text("")
    env = {k: v for k, v in os.environ.items() if k not in ("RECOMP_HEADLESS", "RECOMP_FB_DUMP", "DISPLAY")}
    env.update(device.check_env(str(run)))   # headed; this host has no screen, SDL falls back by itself
    log = run / "run.log"
    t0 = time.time()
    with open(log, "w") as f:
        p = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=f, stderr=subprocess.STDOUT,
                             start_new_session=True)
    presses, origin, n = [], None, 0
    while time.time() - t0 < 12 and p.poll() is None:
        st = read_jsonl(str(run / "status.jsonl"))
        if st and origin is None:
            origin = time.time() - st[-1]["ms"] / 1000.0      # the status clock, as the app finds it
        if origin is not None:
            sms = int((time.time() - origin) * 1000)
            if sms >= (PRESS_FIRST + n * PRESS_EVERY) * 1000:
                with open(run / "pad-live.txt", "a") as f:
                    f.write(("start" if n < 3 else "a") + "\n")
                presses.append(sms)
                n += 1
        time.sleep(0.2)
    run_s = time.time() - t0
    os.kill(p.pid, signal.SIGKILL)                           # by PID, as the app's watchdog
    p.wait(10)
    assert presses and read_jsonl(str(run / "status.jsonl")), log.read_text(errors="replace")[-3000:]
    g = subprocess.run([sys.executable, "-m", "tools.compat", "grade", "--out", str(out), "--source", "Smoke",
                        "--work", str(work), "--status", str(run / "status.jsonl"), "--log", str(log),
                        "--presses", ",".join(map(str, presses)), "--exit", "none", "--run-s", "%.1f" % run_s,
                        "--sustain", "4", "--field", "device=Test host", "--field", "android=0"],
                       cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert g.returncode == 0, g.stderr
    dev = json.loads(out.read_text().splitlines()[-1])
    assert (dev["stage"], dev["blocker"].split(";")[0]) == (desk["stage"], desk["blocker"].split(";")[0]), (dev, desk)
    assert dev["title_id"] == desk["title_id"] == "58424101" and dev["xdk"] == desk["xdk"] == 5849
    assert set(desk) - {"prepare_s"} <= set(dev) and dev["device"] == "Test host"
    m = subprocess.run([sys.executable, "-m", "tools.compat", "report", str(out), "--matrix"], cwd=ROOT,
                       capture_output=True, text=True, timeout=60)
    assert "Test host" in m.stdout, m.stdout


def _asm(items):
    """Bytes from a list of byte strings, ("label", name) and (opcode bytes,
    label) for a rel32 branch to a label (jmp E9, jcc 0F 8x, call E8)."""
    out, labels, fix = bytearray(), {}, []
    for it in items:
        if isinstance(it, (bytes, bytearray)):
            out += it
        elif it[0] == "label":
            labels[it[1]] = len(out)
        else:
            out += it[0] + b"\0\0\0\0"
            fix.append((len(out) - 4, it[1]))
    for at, name in fix:
        out[at:at + 4] = struct.pack("<i", labels[name] - (at + 4))
    return bytes(out)


NIC_OK, NIC_FAIL = 0x00011300, 0x00011320


def _nic_xbe(tmp_path):
    """XBA-117: a title of ours that does what a forcedeth-style driver does
    at start-up, through the NIC's registers at 0xFEF00000: reset and wait
    for UnknownSetupReg5 bit 31, read the PHY's BMSR through the MII
    registers (wait for INUSE to clear) and require the link down, start the
    transmitter and queue one frame on a ring in contiguous memory, wait for
    the controller to hand the descriptor back and require "carrier lost",
    then ask the kernel's PhyGetLinkState and require 0. Every wait is a
    bounded loop. DbgPrint says which way it went, then the title spins."""
    imm = lambda v: struct.pack("<I", v)                         # noqa: E731
    poll = lambda test, ok, fail: [b"\xb9" + imm(100000), ("label", ok + "_top"), test,  # noqa: E731
                                   (b"\x0f\x84" if ok.startswith("z") else b"\x0f\x85", ok),
                                   b"\x49", (b"\x0f\x85", ok + "_top"), (b"\xe9", fail)]
    code = _asm([
        b"\x55\x8b\xec\x56\x57",                                 # push ebp; mov ebp, esp; push esi; push edi
        b"\xbe" + imm(0xFEF00000),                               # mov esi, NIC
        b"\xc7\x86\x44\x01\x00\x00" + imm(0x12),                 # TxRxControl = BIT1 | RESET
        *poll(b"\xf7\x86\x30\x01\x00\x00" + imm(0x80000000), "nz_reset", "fail"),  # test [Reg5], bit31
        ("label", "nz_reset"),
        b"\xc7\x86\x44\x01\x00\x00" + imm(0),                    # TxRxControl = 0
        b"\xc7\x86\x90\x01\x00\x00" + imm(0x21),                 # MIIControl = read PHY 1 reg 1
        *poll(b"\xf7\x86\x90\x01\x00\x00" + imm(0x8000), "z_mii", "fail"),         # test INUSE
        ("label", "z_mii"),
        b"\x8b\x86\x94\x01\x00\x00",                             # mov eax, [MIIData]
        b"\xa9" + imm(0x24), (b"\x0f\x85", "fail"),              # link or autoneg complete: fail
        b"\xbf" + imm(0x80300000),                               # mov edi, ring (phys 0x300000)
        b"\xc7\x07" + imm(0x00301000),                           # desc.buffer
        b"\xc7\x47\x04" + imm(0x8001003B),                       # desc: VALID | LAST, 60 bytes
        b"\xc7\x86\x00\x01\x00\x00" + imm(0x00300000),           # TxRingPhysAddr
        b"\xc7\x86\x08\x01\x00\x00" + imm(0x000F0003),           # RingSizes: 16 rx, 4 tx
        b"\xc7\x86\x84\x00\x00\x00" + imm(1),                    # TransmitterControl = START
        b"\xc7\x86\x44\x01\x00\x00" + imm(1),                    # TxRxControl = KICK
        *poll(b"\xf7\x47\x04" + imm(0x80000000), "z_tx", "fail"),                 # test VALID
        ("label", "z_tx"),
        b"\xf7\x47\x04" + imm(0x08000000), (b"\x0f\x84", "fail"),   # carrier lost, or fail
        b"\x6a\x01\xff\x15\x00\x08\x01\x00",                     # push 1; call [PhyGetLinkState]
        b"\x85\xc0", (b"\x0f\x85", "fail"),                      # link flags must be 0
        b"\x68" + imm(NIC_OK), b"\xff\x15\x04\x08\x01\x00", b"\x83\xc4\x04",       # DbgPrint(ok)
        ("label", "spin"), (b"\xe9", "spin"),
        ("label", "fail"),
        b"\x68" + imm(NIC_FAIL), b"\xff\x15\x04\x08\x01\x00", b"\x83\xc4\x04",     # DbgPrint(fail)
        ("label", "spin2"), (b"\xe9", "spin2"),
    ])
    text = bytearray(code + b"\x00" * (0x340 - len(code)))
    text[NIC_OK - 0x11000:NIC_OK - 0x11000 + 14] = b"XBA117 NIC OK\0"
    text[NIC_FAIL - 0x11000:NIC_FAIL - 0x11000 + 16] = b"XBA117 NIC FAIL\0"
    d = tmp_path / "nic"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=bytes(text), title_id=0x58424106,
                 title="Nic")
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<III", b, 0x800, 0x80000000 | 252, 0x80000000 | 8, 0)    # PhyGetLinkState, DbgPrint
    open(p, "wb").write(b)
    return d


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_title_with_a_network_stack_finds_no_cable(tmp_path):
    """XBA-117 in the real runtime: the lifted driver sequence of _nic_xbe
    completes with the link down, quickly; the log and the status file say
    the title has a network stack, and so does the compat result. With
    RECOMP_NIC=0 (the behaviour before) the registers are plain memory: the
    reset bit never comes up and the same title takes its failure path."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    d, out, cw = _nic_xbe(tmp_path), tmp_path / "results.jsonl", tmp_path / "cw"

    def run(*extra, **env):
        r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(cw),
                            "--seconds", "6", "--sustain", "3", "--out", str(out)] + list(extra),
                           cwd=ROOT, capture_output=True, text=True, timeout=1800, env=dict(os.environ, **env))
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
        status = [json.loads(l) for l in (cw / "nic" / "status-0.jsonl").read_text().splitlines()]
        return (cw / "nic" / "run-0.log").read_text(errors="replace"), status, \
            json.loads(out.read_text().splitlines()[-1])

    log, status, res = run()
    assert "XBA117 NIC OK" in log and "XBA117 NIC FAIL" not in log, log[-3000:]
    assert "[NET] nic probed (registers), link down" in log, log[-3000:]
    net = [s for s in status if s.get("net")]
    assert net and net[0]["net"] == "nic probed, link down" and net[0]["ms"] < 5000, status
    assert res.get("net") == "nic probed, link down", res

    log, status, res = run("--no-prepare", RECOMP_NIC="0")
    assert "XBA117 NIC FAIL" in log and "XBA117 NIC OK" not in log, log[-3000:]
