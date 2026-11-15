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
    # No profile layer: the toolkit's defaults (the XDK switches, the D3D service, the DMA walk) do it all.
    assert "no profile layer for this XBE: the generic template only" in r.stdout
    assert "the toolkit's defaults apply (XDK switches, D3D service, DMA walk)" in r.stdout
    gen = work / "project" / "src" / "recomp" / "gen"
    text = "".join(p.read_text(errors="replace") for p in gen.glob("*.c"))
    assert "xbe_entry_point" in text
    title = work / "project" / "src" / "title"
    assert not title.is_dir() or not [f for f in os.listdir(title) if f.endswith(".c")]
    main_c = (work / "project" / "src" / "main.c").read_text()
    assert "    recomp_d3d_configure();" in main_c and "    recomp_d3d_start(s_game_dir, s_save_dir);" in main_c


def test_prepare_summary_counts_what_the_title_cannot_reach(tmp_path):
    """XBA-139: the lift's counts by class reach the prepare summary and
    the stamp (--check --json reports them): here one `outsb` taken for
    misdecoding and one call into a data section."""
    from tools.disasm.synthetic import DATA, Title
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec").call("f").call("g").hex("ebfe").align(16)
    x.label("f").hex("8b442404 6e c3").align(16)          # mov eax, [esp+4] ; outsb ; ret
    x.label("g").hex("e8").rel("blob").hex("c3").align(16)  # call into .rdata
    r.label("blob").u32(0x12345678).u32(0x9ABCDEF0)
    d = tmp_path / "disc"
    d.mkdir()
    xbe = t.write(str(d / "default.xbe"), "entry", title_id=0x58424139, title="Health")
    work = tmp_path / "work"
    r = _prepare(xbe, work, "--no-build")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    stamp = json.loads((work / "stamp.json").read_text())
    health = stamp["lift_health"]
    assert health["misdecoded"] == 1 and health["unresolved_calls"] == 1, health
    assert "lift health: " in r.stdout and "1 misdecoded sites (string port I/O)" in r.stdout
    c = subprocess.run([sys.executable, "-m", "tools.prepare", xbe, "--work", str(work), "--check", "--json"],
                       cwd=ROOT, capture_output=True, text=True, timeout=300)
    assert json.loads(c.stdout)["lift_health"] == health, c.stdout + c.stderr


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
    """A title whose code the static analysis finds one thread at a time
    (XBA-64, XBA-129). Each start routine's address is computed (mov eax,
    0x11000; add eax, N) and handed to PsCreateSystemThreadEx through the kernel
    thunk table (ordinal 255), so only a run sees it:

      entry    0x11000  starts A, then loops
      A        0x11040  starts B, then waits on a zero dword for ever
      B        0x11080  starts C, then waits
      C        0x11108  not code: zeros up to the end of the section, which
                        seed_from_log's gates refuse
    """
    import struct

    def starter(off, wait):
        code = bytes.fromhex("b800100100" "05") + struct.pack("<I", off) + bytes.fromhex(
            "50" + "6a00" * 9 + "ff1500080100")
        return code + (bytes.fromhex("833d0011010000" "74f7" "c3") if wait else bytes.fromhex("ebfe"))
    entry = bytes.fromhex("558bec") + starter(0x40, False)
    text = bytearray(0x110)
    text[0:len(entry)] = entry
    a, b = starter(0x80, True), starter(0x108, True)
    text[0x40:0x40 + len(a)] = a
    text[0x80:0x80 + len(b)] = b
    d = tmp_path / "heal"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=bytes(text), title_id=0x58424102, title="Heal")
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<II", b, 0x800, 0x80000000 | 255, 0)   # thunk table: PsCreateSystemThreadEx, end
    open(p, "wb").write(b)
    return str(d)


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_the_self_healing_loop(tmp_path):
    """XBA-64 + XBA-97 + XBA-65 + XBA-129 end to end, through `tools.compat
    heal`: round 1 meets thread A with no dispatch entry and records it; the
    next prepare seeds it and rebuilds, round 2 starts A, which meets B; round
    3 starts B, which meets C; C is not code, the gates refuse it, so round 4's
    prepare accepts nothing: the loop stops there, a round without progress,
    not at a fixed count. The result carries the recall measure (3 misses, all
    thread starts). Static discovery off (RECOMP_STATIC_DISCOVERY=0): its gap
    pass would find A and B at the first prepare (XBA-137,
    test_static_discovery_needs_no_round)."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    src, out, cw = _heal_xbe(tmp_path), tmp_path / "results.jsonl", tmp_path / "cw"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "heal", src, "--work-root", str(cw),
                        "--seconds", "6", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=3600,
                       env=dict(os.environ, RECOMP_STATIC_DISCOVERY="0"))
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    res = json.loads(out.read_text().splitlines()[-1])
    h = res["heal"]
    work = cw / "heal"
    lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    for va in ("0x00011040", "0x00011080", "0x00011108"):
        assert {"t": "thread", "va": va, "ctx1": "0x00000000"} in lines, lines
    assert [x["new_seeds"] for x in h["history"]] == [1, 1, 1, 0], h
    assert [x["accepted"] for x in h["history"]] == [None, 1, 1, 0], h
    assert h["rounds"] == 4 and h["stop"] == "no progress: the prepare accepted no new seed", h
    seeds = json.loads((work / "discovered-seeds.json").read_text())
    assert [s["start"] for s in seeds] == ["0x00011040", "0x00011080"]
    assert json.loads((work / "discovered-rejected.json").read_text()) == ["0x00011108"]
    assert res["runs"][0]["threads"] >= 2 and res["stage"] == "boots", res
    rc = res["recall"]
    assert rc["misses"] == 3 and rc["seeded"] == 2 and rc["rejected"] == 1, rc
    assert rc["categories"]["thread start"] == 3, rc
    # The runtime's own count of what it met that the analysis missed (the status line's "seeds").
    assert [x["run_seeds"] for x in h["history"]] == [1, 1, 1, 0], h


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


def _launcher_disc(tmp_path):
    """A two-image synthetic disc: default.xbe launches d:\\game.xbe as the
    XDK's XLaunchNewImage does (a contiguous page holding the path at +8,
    stored in LaunchDataPage, then HalReturnToFirmware(2)); game.xbe spins."""
    import struct
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
    return d


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_launch_of_another_image_hands_over(tmp_path):
    """XBA-82 (desktop): tools.prepare prepares both images of _launcher_disc
    (game.xbe is named in default.xbe's strings) and writes images.map; the
    kernel execs game.xbe's executable in the same process."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    d = _launcher_disc(tmp_path)
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


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_multi_image_disc_boots_from_its_image(tmp_path):
    """XBA-126: the same two-image disc as a split, compressed image (Disc.1.cso,
    Disc.2.cso): tools.prepare reads default.xbe and game.xbe out of it (nothing
    else is extracted), each build's game links to the image, the runtime
    mounts it (kernel_disc.c) for D:\\ and the XBE load, and the hand-over to
    game.xbe runs from the image too."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    sys.path.insert(0, os.path.join(ROOT, "tools", "xiso"))
    import mkcso
    import mkxiso
    d = _launcher_disc(tmp_path)
    iso = tmp_path / "plain.iso"
    mkxiso.build(str(d), str(iso))
    cso = mkcso.compress(iso.read_bytes(), "cso2")
    parts = mkcso.write_split(cso, str(tmp_path / "Disc.cso"), len(cso) // 2 + 1, "dot")
    assert len(parts) == 2
    shutil.rmtree(d)                                 # only the image is left
    iso.unlink()
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", parts[0], "--work-root", str(tmp_path / "cw"),
                        "--seconds", "8", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "Disc.1.cso"
    assert not (work / "game").exists()                              # nothing extracted
    assert [f for f in os.listdir(work / "disc-xbe") if f.endswith(".xbe")] == ["game.xbe"]   # only that copied out
    assert os.path.realpath(work / "project" / "build" / "game") == os.path.realpath(parts[0])
    imap = (work / "images.map").read_text().splitlines()
    assert imap[0].startswith("default.xbe\t") and imap[1].startswith("game.xbe\t"), imap
    log = (work / "run-0.log").read_text(errors="replace")
    assert "[DISC] mounted" in log and "cso split, 2 part(s)" in log, log[-3000:]
    assert "launch of 'D:\\game.xbe': handing over to" in log, log[-3000:]
    assert "Game2" in log.split("handing over to", 1)[1], log[-3000:]
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
def test_a_stripped_library_table_still_gets_the_xdk_switches(tmp_path):
    """XBA-106 in the real runtime: no library table at all, but D3D's kernel imports (AvSetDisplayMode,
    AvSendTVEncoderOption, MmClaimGpuInstanceMemory). No layer is needed: the title project sets the XDK switches
    itself (the DISPATCH gate, the pad, AC'97), and the D3D service runs for every title."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    d = tmp_path / "bare"
    d.mkdir()
    make_xbe(d / "default.xbe", libs=(), text=SPIN, title_id=0x58424107, title="Bare", imports=(3, 2, 168))
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "4", "--sustain", "2", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "bare"
    prep = (work / "compat-prepare.log").read_text()
    assert "XDK: no library table; the toolkit's defaults apply" in prep, prep
    log = (work / "run-0.log").read_text()
    xdk = [line for line in log.splitlines() if line.startswith("[XDK] defaults:")]
    assert xdk and all(k in xdk[0] for k in ("RECOMP_DISPATCH_GATE=1", "RECOMP_KEYBOARD=1", "RECOMP_AC97_READY=1")), \
        log[-3000:]
    assert "emulated APU up" in log, log[-3000:]          # RECOMP_AC97_READY took effect


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_code_written_and_run_at_run_time_are_named(tmp_path):
    """XBA-118 in the real runtime: a title that patches its own code (a store into its second code page) and then
    calls into the heap. The store is trapped once through the write-protected code page and goes on; the call is
    skipped as before; both are {"t":"smc"} records, and tools.compat names them in the blocker with the count."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    code = bytes.fromhex("558bec"                   # push ebp; mov ebp, esp
                         "c705" "00200100" "90909090"   # mov dword [0x12000], 0x90909090: into the code
                         "b8" "00004000"                # mov eax, 0x00400000
                         "ffd0"                         # call eax: the heap
                         "ebfe")                        # jmp $
    text = code + b"\xcc" * (0x2000 - len(code))       # two whole code pages, 0x11000 and 0x12000
    d = tmp_path / "smc"
    d.mkdir()
    make_xbe(d / "default.xbe", libs=LIBS, text=text, title_id=0x58424106, title="SMC")
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "5", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "smc"
    log = (work / "run-0.log").read_text()
    assert "[SMC] 2 code page(s) of 4096 bytes write-protected" in log or "[SMC] 0 code page" in log, log[-3000:]
    lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    smc = [l for l in lines if l.get("t") == "smc"]
    assert {"t": "smc", "va": "0x00400000", "kind": "exec_outside_image", "where": "heap",
            "bytes": "0000000000000000"}.items() <= smc[-1].items(), lines
    res = json.loads(out.read_text().splitlines()[-1])
    if "[SMC] 2 code page(s)" in log:              # 4 KB host pages: the store is trapped
        assert {"t": "smc", "va": "0x00012000", "kind": "code_write", "at": "0x00012000",
                "section": ".text", "by": "a store"}.items() <= smc[0].items(), lines
        assert res["smc"] == 2 and "code written or run at run time x2: code_write 0x00012000 (.text)" \
            in res["blocker"], res
    assert res["blocker"].startswith("no flip") and "exec_outside_image 0x00400000 (heap" in res["blocker"], res


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


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_static_discovery_needs_no_round(tmp_path):
    """XBA-137: a title whose run reaches only code the analysis used to miss
    (tools/disasm/synthetic.py discovery_e2e_title: a vtable method, a thread
    routine packed after a noreturn kernel call, a gap function reached through
    a computed address, and a table of decoys) is found at the first prepare:
    the run records nothing to seed, the loop stops after its first round, the
    recall has no miss, and no decoy became a function."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    from tools.disasm.synthetic import E2E_DECOYS, E2E_IMPORTS, discovery_e2e_title
    d = tmp_path / "disco"
    d.mkdir()
    t = discovery_e2e_title()
    t.write(d / "default.xbe", "entry", imports=E2E_IMPORTS, title_id=0x58424137, title="Disco")
    out, cw = tmp_path / "results.jsonl", tmp_path / "cw"
    env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_STATIC_DISCOVERY")}
    r = subprocess.run([sys.executable, "-m", "tools.compat", "heal", str(d), "--work-root", str(cw),
                        "--seconds", "6", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=3600, env=env)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    res = json.loads(out.read_text().splitlines()[-1])
    h = res["heal"]
    assert h["rounds"] == 1 and h["history"][0]["new_seeds"] == 0, h
    assert "recall" not in res, res["recall"]                  # no miss: compat attaches none
    # The thread the title starts runs (its routine was found: no "thread" entry).
    assert res["runs"][0]["threads"] >= 1 and res["stage"] == "boots", res
    disc = cw / "disco" / "discovered.jsonl"
    lines = disc.read_text().splitlines() if disc.exists() else []
    assert not [l for l in lines if '"t":"icall"' in l or '"t":"thread' in l], lines
    work = cw / "disco"
    rc = subprocess.run([sys.executable, "-m", "tools.compat", "recall", str(work), str(d / "default.xbe")],
                        cwd=ROOT, capture_output=True, text=True, timeout=120)
    assert rc.returncode == 0 and rc.stdout.strip() == "no miss", rc.stdout + rc.stderr
    funcs = {int(f["start"], 16) for f in json.loads((work / "analysis" / "disasm" / "functions.json").read_text())}
    assert {t.va(n) for n in ("thread_fn", "m1", "gap_fn")} <= funcs
    # The run needs no round in the default mode. Keeping the title's decoys
    # out of the functions is RECOMP_STATIC_DISCOVERY_STRICT's (off by
    # default, wave 5): tools/disasm/test_static_discovery.py's
    # test_the_e2e_title_needs_no_run checks them with it on.
    assert E2E_DECOYS
    summary = json.loads((work / "analysis" / "disasm" / "summary.json").read_text())
    assert summary["discovery"]["added"]["gap_body"] >= 1, summary["discovery"]


def _movie_disc(tmp_path, **kw):
    """tools/xmv/synth_title.py's movie title, with one of our own XMV fixtures as its D:\\intro.xmv."""
    from tools.xmv.synth_title import movie_title
    d = tmp_path / "movie"
    d.mkdir()
    p, labels = movie_title(d / "default.xbe", **kw)
    shutil.copy(os.path.join(ROOT, "tests", "xmv_decode", "fixtures", "basic.xmv"), d / "intro.xmv")
    return d, p, labels


def test_prepare_binds_the_xmv_api_it_finds(tmp_path):
    """XBA-199: prepare finds the fake XMV library's GetNextFrame and CloseDecoder by structure, and the lift
    wraps them: their bodies are sub_X_gen, recomp_xmv_bind.c defines sub_X. No title layer."""
    d, xbe, l = _movie_disc(tmp_path)
    work = tmp_path / "work"
    r = _prepare(xbe, work, "--no-build")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    gnf, close = "%08X" % l["get_next_frame"], "%08X" % l["close"]
    assert "XMV: XMVDecoder_GetNextFrame at 0x%s and XMVDecoder_CloseDecoder at 0x%s" % (gnf, close) in r.stdout
    gen = work / "project" / "src" / "recomp" / "gen"
    bind = (gen / "recomp_xmv_bind.c").read_text()
    assert "void sub_%s(void)" % gnf in bind and "void sub_%s(void)" % close in bind
    text = "".join(p.read_text(errors="replace") for p in gen.glob("recomp_0*.c"))
    assert "void sub_%s_gen(void)" % gnf in text and "void sub_%s_gen(void)" % close in text
    assert "XMV: bound XMVDecoder_CloseDecoder" in (work / "prepare.log").read_text()
    assert json.loads((work / "analysis" / "xmv_api.json").read_text())["high"]
    title = work / "project" / "src" / "title"
    assert not title.is_dir()                                   # no title layer

    # RECOMP_XMV=native at prepare: nothing bound, the lift is redone without it.
    r = subprocess.run([sys.executable, "-m", "tools.prepare", xbe, "--work", str(work), "--no-build"],
                       cwd=ROOT, capture_output=True, text=True, timeout=900, env=dict(os.environ, RECOMP_XMV="native"))
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "RECOMP_XMV=native, the title's own movie decoder is kept" in r.stdout
    assert not (gen / "recomp_xmv_bind.c").exists()


def test_prepare_binds_nothing_without_an_xmv_library(tmp_path):
    d, xbe, _ = _movie_disc(tmp_path, movie=False)
    work = tmp_path / "work"
    r = _prepare(xbe, work, "--no-build")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "XMV:" not in r.stdout
    assert not (work / "project" / "src" / "recomp" / "gen" / "recomp_xmv_bind.c").exists()


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_a_bound_title_plays_its_movie(tmp_path):
    """XBA-199 in the real runtime: the title's own GetNextFrame never gives a frame (a decoder that cannot
    run), so lifted as is it polls for ever. Bound by Prepare, the runtime plays the .xmv the title opened
    into its D3DSurface (12 frames, paced), the movie ends, and the title goes on (its DbgPrint). Then the same
    executable with RECOMP_XMV=native runs the title's own decoder: no movie, the title stays in its loop."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    from tools.xmv.synth_title import DONE
    d, xbe, l = _movie_disc(tmp_path)
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", str(d), "--work-root", str(tmp_path / "cw"),
                        "--seconds", "6", "--sustain", "3", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    work = tmp_path / "cw" / "movie"
    log = (work / "run-0.log").read_text(errors="replace")
    assert "[XMV] XMVDecoder_GetNextFrame at %08X bound by Prepare" % l["get_next_frame"] in log, log[-3000:]
    assert "the runtime decodes the title's movies" in log
    assert "intro.xmv: 96x64" in log, log[-3000:]
    assert "[XMV] movie ended: 12 frames (12 decoded, 0 X8 intra not supported, 0 damaged)" in log, log[-3000:]
    assert DONE in log.split("[XMV] movie ended", 1)[1], log[-3000:]
    assert "[XMV] XMVDecoder_CloseDecoder at %08X bound by Prepare" % l["close"] in log

    from tools.compat.__main__ import project_exe
    exe = project_exe(str(work))
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=dict(os.environ, RECOMP_XMV="native", SDL_VIDEODRIVER="dummy",
                                     SDL_AUDIODRIVER="dummy"))
    try:
        native, _ = proc.communicate(timeout=6)
    except subprocess.TimeoutExpired:
        proc.kill()                     # by PID: the title polls for ever, as expected
        native, _ = proc.communicate()
    native = native.decode(errors="replace")
    assert "RECOMP_XMV=native, the title's own decoder runs" in native, native[-3000:]
    assert "movie ended" not in native and DONE not in native, native[-3000:]
