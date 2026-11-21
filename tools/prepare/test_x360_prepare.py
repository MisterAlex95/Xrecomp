"""tools.prepare for an Xbox 360 XEX or disc (tools/prepare/x360.py, XBA-282): identification (an XEX, a disc image
holding default.xex at a 360 partition offset; not an original Xbox image), the player's key through $XEX_KEY_FILE
(NO_KEY without it), the discovered seeds that only grow, the shader cache translated at Prepare time, and a desktop
pack built end to end with the real lifter from the synthetic test title (consoles/x360/tests/make_hello.py), run back
by x360_run when $X360_RUN names one built from this tree."""

import json
import os
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
X360_TESTS = os.path.join(ROOT, "consoles", "x360", "tests")
X360_TOOLS = os.path.join(ROOT, "consoles", "x360", "tools")
sys.path[:0] = [X360_TOOLS, X360_TESTS]

from tools.prepare import x360                      # noqa: E402
from tools.prepare.progress import parse_line       # noqa: E402

CC = shutil.which(os.environ.get("CC", "cc"))
X360_RUN = os.environ.get("X360_RUN", "")
KEY = b"xba-270-made-up!"


def hello(compression="normal", key=KEY):
    import make_hello
    return make_hello.build(compression, key)


def prepare(args, env=None):
    e = dict(os.environ)
    e.pop("XEX_KEY_FILE", None)
    e.update(env or {})
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + args + ["--progress"], cwd=ROOT, env=e,
                       capture_output=True, text=True, timeout=900)
    lines = [parse_line(l) for l in p.stdout.splitlines()]
    lines = [l for l in lines if l]
    assert lines and lines[-1]["kind"] == "result", p.stdout + p.stderr
    return p.returncode, lines


def progress(lines):
    return [l["msg"] for l in lines if l["kind"] == "progress"]


def make_disc(tmp_path, xex, base=0x0FD90000, name="default.xex"):
    """An Xbox 360 image: an XDVDFS volume at a 360 partition offset with the XEX at its root (sparse file)."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "xiso"))
    import mkxiso
    src = tmp_path / ("disc_src_%x_%s" % (base, name))
    src.mkdir(exist_ok=True)
    (src / name).write_bytes(xex)
    (src / "readme.txt").write_text("readme\n")
    out = tmp_path / ("disc%x.iso" % base)
    mkxiso.build(str(src), str(out), base=base)
    return out


def test_identify_xex_and_key(tmp_path):
    raw = hello("none", None)
    p = tmp_path / "t.xex"
    p.write_bytes(raw)
    assert x360.is_x360(str(p))
    key = x360.title_key(raw)
    assert key and key.startswith("X360-58425A30-")
    data, img = x360.plain(raw)
    assert data[:4] == b"XEX2" and not img.encryption
    (tmp_path / "x.bin").write_bytes(b"\x00" * 0x400)
    assert not x360.is_x360(str(tmp_path / "x.bin"))


def test_encrypted_needs_the_key(tmp_path, monkeypatch):
    raw = hello("normal", KEY)
    monkeypatch.delenv("XEX_KEY_FILE", raising=False)
    with pytest.raises(x360.NoKey):
        x360.plain(raw)
    (tmp_path / "k.key").write_bytes(KEY)
    monkeypatch.setenv("XEX_KEY_FILE", str(tmp_path / "k.key"))
    data, img = x360.plain(raw)
    assert img.encryption and data[:4] == b"XEX2"
    assert x360.title_key(data) == x360.title_key(raw)      # the plain rewrite keeps the execution info


def test_plain_cached(tmp_path, monkeypatch):
    """The plain XEX is decrypted once per source and tools/xex code: later Prepares read plain.xex back (no key, no
    AES) while its sidecar names this source, this code and the bytes plain.xex still has."""
    raw = hello("normal", KEY)
    (tmp_path / "k.key").write_bytes(KEY)
    monkeypatch.setenv("XEX_KEY_FILE", str(tmp_path / "k.key"))
    out = str(tmp_path / "plain.xex")
    data, img, enc = x360.plain_cached(raw, out, "tk1")
    assert enc and open(out, "rb").read() == data
    calls = []
    real = x360.plain
    monkeypatch.setattr(x360, "plain", lambda r: calls.append(1) or real(r))
    monkeypatch.delenv("XEX_KEY_FILE")              # the cache needs no key
    d2, img2, enc2 = x360.plain_cached(raw, out, "tk1")
    assert (d2, enc2, calls) == (data, True, []) and img2.image == img.image and img2.resources == img.resources
    monkeypatch.setenv("XEX_KEY_FILE", str(tmp_path / "k.key"))
    assert x360.plain_cached(raw, out, "tk2")[0] == data and calls == [1]       # other tools/xex: decrypted again
    with open(out, "r+b") as f:                     # plain.xex changed behind the sidecar's back: decrypted again
        f.seek(len(data) - 1)
        f.write(bytes([data[-1] ^ 1]))
    assert x360.plain_cached(raw, out, "tk2")[0] == data and calls == [1, 1]
    other = hello("none", None)                     # another source XEX
    assert x360.plain_cached(other, out, "tk2")[0] == real(other)[0] and calls == [1, 1, 1]


def test_disc_image(tmp_path):
    raw = hello("none", None)
    d = make_disc(tmp_path, raw)
    assert x360.is_x360(str(d))
    assert x360.read_xex_bytes(str(d)) == raw
    xbox = make_disc(tmp_path, b"XBEH" + b"\x00" * 4092, base=0, name="default.xbe")
    assert not x360.is_x360(str(xbox))


def test_seeds_and_compile_flags(tmp_path):
    a = tmp_path / "a.txt"
    a.write_text("# x\n82000010\nzz\n82000011\n82000020\n")
    assert x360.read_addresses(str(a)) == {0x82000010, 0x82000020}
    flags = x360.compile_flags(x360.lifted_opt("2"), ROOT, "/gen")
    assert "-O1" in flags and "-DXE_RUNTIME=1" in flags and "-ffp-contract=off" in flags
    assert "-I" + os.path.join(ROOT, "consoles", "x360", "runtime") in flags
    assert "-I" + os.path.join(ROOT, "consoles", "gc", "runtime") in flags
    page = x360.host_page_size()                     # this machine's: the dynamic view unless its pages are 4 KB
    assert ("-DXE_VIEW4K_DYNAMIC=0" if page == 4096 else "-DXE_VIEW4K_DYNAMIC=1") in flags


@pytest.mark.parametrize("page,static", [(4096, True), (16384, False), (65536, False)])
def test_view4k_by_page_size(page, static):
    """WP-33: a pack for a device of 4 KB pages leaves view4k_skew out; 16 KB (the Fold8's Android 15+, the Mac) and
    64 KB keep it; the flag follows the page size given, the Android sysroot's flags too."""
    flags = x360.compile_flags("1", ROOT, "/gen", page=page)
    want, other = ("-DXE_VIEW4K_DYNAMIC=0", "-DXE_VIEW4K_DYNAMIC=1")[::1 if static else -1]
    assert want in flags and other not in flags
    assert x360.view4k_static(page) is static
    meta = {"view4k": "static" if static else "dynamic", "host_page": page}
    for run_on in (4096, 16384):                     # a static pack refused on a device of larger pages only
        why = x360.view4k_refusal(meta, run_on)
        assert (why is not None) == (static and run_on > 4096), (page, run_on, why)
        if why:
            assert "Prepare it again" in why and str(run_on) in why
    assert x360.view4k_refusal({}, 16384) is None    # a pack from before WP-33: left to x360_bind


def test_host_page_size(monkeypatch):
    monkeypatch.delenv("X360_HOST_PAGE", raising=False)
    real = os.sysconf("SC_PAGE_SIZE")
    assert x360.host_page_size() == real              # the real one, never a constant
    monkeypatch.setenv("X360_HOST_PAGE", "0x10000")   # raised as the runtime raises it (x360_mem.c)
    assert x360.host_page_size() == 0x10000
    monkeypatch.setenv("X360_HOST_PAGE", "1024")      # never lowered
    assert x360.host_page_size() == real
    monkeypatch.setenv("X360_HOST_PAGE", "12288")     # not a power of two: ignored
    assert x360.host_page_size() == real
    monkeypatch.setattr(x360.os, "sysconf", lambda name: 4096)
    monkeypatch.delenv("X360_HOST_PAGE")
    assert x360.host_page_size() == 4096
    assert "-DXE_VIEW4K_DYNAMIC=0" in x360.compile_flags("1", ROOT, "/gen")
    monkeypatch.setattr(x360.os, "sysconf", lambda name: 16384)
    assert "-DXE_VIEW4K_DYNAMIC=1" in x360.compile_flags("1", ROOT, "/gen")


def test_static_pack_refused_on_larger_pages(tmp_path, monkeypatch):
    """A pack prepared on a device of 4 KB pages, restored on one of 16 KB (a backup): check() says Prepare again, so
    the next Prepare builds it again instead of « pack up to date »."""
    monkeypatch.delenv("X360_HOST_PAGE", raising=False)
    monkeypatch.setattr(x360.os, "sysconf", lambda name: 4096)
    d = tmp_path / "x360-K"
    d.mkdir()
    lib = d / "libtitle.so"
    lib.write_bytes(b"\x7fELF lifted")
    (d / "default.xex").write_bytes(b"XEX2")
    meta = x360.make_meta(str(lib), "K", "t", "s" * 64, "default.xex", "abi", x360.pack.SHELL_ABI, "c", "t" * 64,
                          "clang -O1", 0)
    assert meta["view4k"] == "static" and meta["host_page"] == 4096
    (d / "pack.json").write_text(json.dumps(meta))
    assert x360.check(str(d), "K", "s" * 64, "abi") is None
    monkeypatch.setattr(x360.os, "sysconf", lambda name: 16384)
    why = x360.check(str(d), "K", "s" * 64, "abi")
    assert why and "4 KB" in why and "16384" in why and "Prepare it again" in why


def test_shaders_translated_once(tmp_path):
    from x360_tools.xenos_asm import alu_words, export, program
    good = program([(2, [("alu", alu_words(**export(0, 1)))], {})])
    bad = program([(2, [("alu", alu_words(vop=2, vdest=40, vmask=0xF, export=1))], {})])   # no such register
    dump = tmp_path / "dump"
    dump.mkdir()
    for name, words in (("vs_00000001.bin", good), ("ps_00000002.bin", good), ("vs_00000003.bin", bad)):
        (dump / name).write_bytes(struct.pack(">%dI" % len(words), *words))
    (dump / "notes.txt").write_text("x")
    out = tmp_path / "out"
    assert x360.translate_shaders(str(dump), str(out)) == (3, 2)
    assert (out / "vs_00000001.vert").exists() and (out / "ps_00000002.frag").exists()
    assert (out / "vs_00000003.bin").exists() and not (out / "vs_00000003.vert").exists()
    assert x360.translate_shaders(str(dump), str(out)) == (0, 0)       # known: not tried again
    assert x360.shader_counts(str(out)) == (3, 2)


@pytest.mark.skipif(not CC or not sys.platform.startswith("linux"), reason="no host C compiler / not Linux")
def test_desktop_pack(tmp_path):
    src = tmp_path / "DEFAULT.XEX"
    src.write_bytes(hello("normal", KEY))
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [str(src), "--work", str(work), "--pack-dir", str(packs), "--jobs", "3", "--opt", "1"]

    rc, lines = prepare(args)                       # no key: NO_KEY, nothing built
    assert rc != 0 and lines[-1]["code"] == "NO_KEY", lines[-1]

    (tmp_path / "k.key").write_text(KEY.hex() + "\n")
    env = {"XEX_KEY_FILE": str(tmp_path / "k.key")}
    rc, lines = prepare(args, env)
    assert rc == 0, lines[-1]
    key = x360.title_key(src.read_bytes())
    final = packs / ("x360-" + key)
    assert lines[-1]["pack"] == str(final)
    meta = json.loads((final / "pack.json").read_text())
    assert meta["console"] == "x360" and meta["title_key"] == key and meta["entry"] == "xe_rc_resume"
    assert (final / "default.xex").read_bytes() == (work / "plain.xex").read_bytes()
    assert x360.check(str(final), key, meta["source_sha256"], x360.gba.host_abi()) is None
    page = x360.host_page_size()
    assert meta["host_page"] == page and meta["view4k"] == ("static" if page == 4096 else "dynamic")

    # A run met code the pack lacks and two shaders: the next Prepare lifts the address and translates the shaders.
    from x360_tools.xenos_asm import alu_words, export, program
    good = program([(2, [("alu", alu_words(**export(0, 1)))], {})])
    (work / "shaders").mkdir(exist_ok=True)
    for name in ("vs_0000000A.bin", "ps_0000000B.bin"):
        (work / "shaders" / name).write_bytes(struct.pack(">%dI" % len(good), *good))
    entry = int(meta.get("discovered", 0))
    assert entry == 0
    import make_hello
    (work / "discovered.txt").write_text("# x360_runtime\n%08X\n" % make_hello.TEXT)
    rc, lines = prepare(args, env)
    assert rc == 0, lines[-1]
    meta = json.loads((final / "pack.json").read_text())
    assert meta["discovered"] == 1
    assert (final / "shaders" / "vs_0000000A.vert").exists() and (final / "shaders" / "ps_0000000B.frag").exists()
    assert any("2 shader(s), 2 translated" in m for m in progress(lines)), progress(lines)

    # The runtime rewrote discovered.txt with nothing (a later run): the seeds stay.
    (work / "discovered.txt").write_text("# x360_runtime\n")
    rc, lines = prepare(args, env)
    assert rc == 0 and "pack up to date" in progress(lines), progress(lines)
    assert json.loads((final / "pack.json").read_text())["discovered"] == 1

    # Another toolkit: the translations are made again (a stale stamp clears work/glsl).
    (work / "glsl" / "vs_0000000A.vert").write_text("stale")
    st = json.loads((work / "stamp.json").read_text())
    st["toolkit_sha256"] = "0" * 64
    (work / "stamp.json").write_text(json.dumps(st))
    rc, lines = prepare(args, env)
    assert rc == 0, lines[-1]
    assert (work / "glsl" / "vs_0000000A.vert").read_text() != "stale"

    # A new shader alone: the link stays, the shader goes into the published pack.
    (work / "shaders" / "vs_0000000C.bin").write_bytes(struct.pack(">%dI" % len(good), *good))
    rc, lines = prepare(args, env)
    assert rc == 0 and "pack up to date" in progress(lines)
    assert (final / "shaders" / "vs_0000000C.vert").exists()

    if os.path.isfile(X360_RUN):                    # the pack runs: the title ends with exit code 0
        disc = tmp_path / "disc"
        (disc / "data").mkdir(parents=True)
        (disc / "data" / "hello.txt").write_text("Hello from the disc!")
        r = subprocess.run([X360_RUN, str(final / "default.xex"), "--pack", str(final / "libtitle.so"), "--disc",
                            str(disc), "--dbg", str(tmp_path / "dbg.txt")], capture_output=True, text=True,
                           timeout=300)
        assert "exit code 0" in r.stdout + r.stderr, r.stdout + r.stderr


def test_title_image():
    png = b"\x89PNG\r\n\x1a\n" + b"pixels"
    other = b"not an image"
    # XDBF: 2 entries of a table of 4, a free table of 2; data: the other entry, then the PNG
    blob = b"XDBF" + struct.pack(">IIIII", 0x10000, 4, 2, 2, 0)
    blob += struct.pack(">HQII", 1, 0x8000, 0, len(other)) + struct.pack(">HQII", 2, 0x8000, len(other), len(png))
    blob += b"\0" * (18 * 2 + 8 * 2) + other + png
    assert x360.xdbf_entry(blob, 2, 0x8000) == png
    assert x360.xdbf_entry(blob, 2, 0x8001) is None
    assert x360.xdbf_entry(b"XDBF" + b"\0" * 4, 2, 0x8000) is None

    class Img:
        base = 0x82000000
        image = b"\0" * 0x100 + blob
        execution = {"title_id": 0x58425A30}
        resources = [("FFFFFFFF", 0x82000000, 0x10), ("58425A30", 0x82000100, len(blob))]
    assert x360.title_image(Img) == png
    Img.resources = [("58425A30", 0x82000100, len(blob) + 0x1000)]     # out of the image: none
    assert x360.title_image(Img) is None
