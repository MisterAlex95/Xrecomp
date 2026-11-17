"""tools.prepare for a GameCube disc or DOL (tools/prepare/gc.py, docs/android/prepare.md « GameCube packs »): the
title key and source identity (the main DOL's SHA-256, the same for ISO / CISO / GCZ / RVZ), the REL modules copied
from the FST, the discovered file (addresses and REL records) merged, and a desktop pack built end to end with the real
lifter on a disc made here (consoles/gc/tools/gc_tools: a synthetic DOL whose REL module is lifted at the load address
the runtime recorded), run back by gc_run built from source: same result, the module's code native."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)
GC_TOOLS = os.path.join(ROOT, "consoles", "gc", "tools")
sys.path.insert(0, GC_TOOLS)

from tools.prepare import gc                        # noqa: E402
from tools.prepare.progress import parse_line       # noqa: E402
from gc_tools import disc as gcdisc                 # noqa: E402
from gc_tools import rel                            # noqa: E402
from gc_tools import test_gc_rel as trel            # noqa: E402

CC = shutil.which(os.environ.get("CC", "cc"))


def prepare(args, env=None):
    e = dict(os.environ)
    e.pop("XBA_GC_RECOMP", None)
    e.update(env or {})
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + args, cwd=ROOT, env=e, capture_output=True,
                       text=True, timeout=900)
    lines = [parse_line(l) for l in p.stdout.splitlines()]
    lines = [l for l in lines if l]
    assert lines and lines[-1]["kind"] == "result", p.stdout + p.stderr
    return p.returncode, lines


def progress(lines):
    return [l["msg"] for l in lines if l["kind"] == "progress"]


def make_disc(tmp_path):
    """A GCM with the synthetic DOL of test_gc_rel (its module linked at BASE) and the module as stage/m.rel."""
    raw = trel.module()
    lo = rel.Loaded(rel.Rel(raw), trel.BASE, trel.BSS).link()
    dol = trel.program(lo.os_image(), lo.entry_points()[0])
    g = gcdisc.build_gcm(dol, [("stage/m.rel", raw), ("readme.txt", b"hello")], game_id=b"GXRP", maker=b"01",
                         revision=1)
    p = tmp_path / "Game.iso"
    p.write_bytes(g.data)
    return p, g, dol, raw, lo


def test_identify_disc_formats_and_dol(tmp_path):
    p, g, dol, raw, _lo = make_disc(tmp_path)
    assert gc.is_gc(str(p))
    key, title, got, rels = gc.read_source(str(p), str(tmp_path))
    assert key == "GXRP01-1" and title == "XRecomp disc test" and got == dol and rels == {"stage/m.rel": raw}
    for name, data in (("g.ciso", gcdisc.to_ciso(g.data)), ("g.gcz", gcdisc.to_gcz(g.data))):
        (tmp_path / name).write_bytes(data)
        assert gc.is_gc(str(tmp_path / name))
        assert gc.read_source(str(tmp_path / name), str(tmp_path))[:3] == (key, title, dol)
    d = tmp_path / "prog.dol"
    d.write_bytes(dol)
    assert gc.is_gc(str(d))
    assert gc.read_source(str(d)) == ("DOL-%08X" % (zlib.crc32(dol) & 0xFFFFFFFF), "prog", dol, {})
    (tmp_path / "x.bin").write_bytes(b"\x00" * 0x400)
    assert not gc.is_gc(str(tmp_path / "x.bin"))
    # an Xbox (or PSP) CSO starts with "CISO" too: its header size where a GameCube CISO has its block size
    sys.path.insert(0, os.path.join(ROOT, "tools", "xiso"))
    import mkcso
    for fmt in ("cso1", "cso2"):
        (tmp_path / "x.cso").write_bytes(mkcso.compress(b"\x00" * 0x10000, fmt))
        assert not gc.is_gc(str(tmp_path / "x.cso")), fmt
    # an RVZ without the runtime's reader: why, and what to do
    (tmp_path / "g.rvz").write_bytes(gcdisc.to_wia(g, "zstd", rvz=True))
    assert gc.is_gc(str(tmp_path / "g.rvz"))
    old = os.environ.pop(gc.GC_RUN_ENV, None)
    try:
        if not shutil.which("gc_run"):
            with pytest.raises(ValueError, match="XBA_GC_RUN"):
                gc.read_source(str(tmp_path / "g.rvz"), str(tmp_path))
    finally:
        if old is not None:
            os.environ[gc.GC_RUN_ENV] = old


def test_discovered_merge(tmp_path):
    a = tmp_path / "a.txt"
    b = tmp_path / "b.txt"
    a.write_text("# x\n80003104\nREL 5 80100000 80108000 DEADBEEF\n80003100\nzz\n80003101\n")
    b.write_text("80003100\nREL  5 80100000 80108000 DEADBEEF\nREL 6 80100000 80108000 00000001 5@80100000\n")
    assert gc.merged_lines(str(a), str(b)) == ["80003100", "80003104", "REL 5 80100000 80108000 DEADBEEF",
                                               "REL 6 80100000 80108000 00000001 5@80100000"]


def test_compile_flags():
    flags = gc.compile_flags(gc.lifted_opt("3"), ROOT, "/gen")
    assert "-O2" in flags and "-ffp-contract=off" in flags and "-frounding-math" in flags
    assert "-I" + os.path.join(ROOT, "consoles", "gc", "runtime") in flags


def test_check_refuses(tmp_path):
    d = tmp_path / "gc-GXRP01-1"
    d.mkdir()
    (d / "libtitle.so").write_bytes(b"lib")
    meta = gc.make_meta(str(d / "libtitle.so"), "GXRP01-1", "T", "ab" * 32, "main.dol", "x86_64", 1, "c",
                        "d" * 64, "cc -O2", 0)
    assert meta["console"] == "gc" and meta["entry"] == "gc_recomp_title_table"
    (d / "pack.json").write_text(json.dumps(meta))
    assert gc.check(str(d), "GXRP01-1", "ab" * 32, "x86_64") is None
    assert "another main DOL" in gc.check(str(d), "GXRP01-1", "cd" * 32, "x86_64")
    assert "title_key" in gc.check(str(d), "GXRP01-0", "ab" * 32, "x86_64")


def _gc_run(tmp_path):
    from gc_tools import cbuild
    return cbuild.gc_run(str(tmp_path / "build"), CC or "cc", ["-O2", "-ffp-contract=off", "-frounding-math"])


def _peek(out):
    for line in out.splitlines():
        if line.startswith("peek "):
            return [int(x, 16) for x in line[5:].split()]
    return None


@pytest.mark.skipif(not CC or not sys.platform.startswith("linux"), reason="no host C compiler / not Linux")
def test_desktop_pack_with_a_rel_module(tmp_path):
    p, g, dol, raw, lo = make_disc(tmp_path)
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [str(p), "--work", str(work), "--pack-dir", str(packs), "--jobs", "2", "--opt", "1"]
    rc, lines = prepare(args)
    assert rc == 0, lines[-1]
    final = packs / "gc-GXRP01-1"
    assert lines[-1]["pack"] == str(final)
    meta = json.loads((final / "pack.json").read_text())
    sha = hashlib.sha256(dol).hexdigest()
    assert meta["console"] == "gc" and meta["title_key"] == "GXRP01-1" and meta["source_sha256"] == sha
    assert gc.check(str(final), "GXRP01-1", sha, gc.gba.host_abi()) is None
    assert (work / "main.dol").read_bytes() == dol and (work / "rel" / "stage_m.rel").read_bytes() == raw
    assert any("1 REL module(s)" in m for m in progress(lines))

    # The runtime boots the disc (apploader, no IPL), records the module; GX shader list written by a run too.
    exe = _gc_run(tmp_path)
    peek = ["--peek", "%08X:1" % trel.OUT]
    r = subprocess.run([exe, str(p), "--frames", "6", "--discovered", str(work / "discovered.txt")] + peek,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == [122]
    assert "REL 5 80100000 80108000 %08X" % lo.exec_hash() in (work / "discovered.txt").read_text()
    (work / "gx_shaders.txt").write_text("# gx shader list\n")

    # The next Prepare lifts the module at its load address; the pack runs it natively.
    rc, lines = prepare(args)
    assert rc == 0, lines[-1]
    assert any("1 REL module load(s) lifted" in m for m in progress(lines)), progress(lines)
    assert (final / "gx_shaders.txt").read_text() == "# gx shader list\n"
    assert json.loads((final / "pack.json").read_text())["discovered"] >= 1
    r = subprocess.run([exe, str(p), "--frames", "6", "--stats", "--pack", str(final / "libtitle.so"),
                        "--missed", str(tmp_path / "missed.txt")] + peek, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stderr
    assert _peek(r.stdout) == [122]
    missed = [int(x, 16) for x in (tmp_path / "missed.txt").read_text().splitlines() if x and x[0] != "#"]
    assert not [a for a in missed if trel.BASE <= a < trel.BASE + len(raw)], missed

    # Again: up to date.
    rc, lines = prepare(args)
    assert rc == 0 and "pack up to date" in progress(lines)

    # The same disc as an RVZ through gc_run --extract: the same key, identity and pack.
    (tmp_path / "Game.rvz").write_bytes(gcdisc.to_wia(g, "zstd", rvz=True))
    rc, lines = prepare([str(tmp_path / "Game.rvz")] + args[1:], {gc.GC_RUN_ENV: exe})
    assert rc == 0, lines[-1]
    assert lines[-1]["pack"] == str(final) and "pack up to date" in progress(lines)
