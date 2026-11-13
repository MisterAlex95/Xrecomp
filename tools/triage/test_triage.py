"""tools.triage on synthetic discs (no game data): folders and an image, a
secondary XBE named by default.xbe, the groups and the table."""

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.triage import groups, markdown, triage_disc    # noqa: E402
from tools.xbe_ident.test_xbe_ident import make_xbe     # noqa: E402


def disc(root, name, libs, text=b"", others=()):
    d = root / name
    d.mkdir()
    make_xbe(d / "default.xbe", libs=libs, text=text)
    for rel in others:
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        make_xbe(p)
    return d


def test_folder_with_a_launched_xbe(tmp_path):
    d = disc(tmp_path, "A", (("XAPILIB", 5849, 0), ("D3D8LTCG", 5849, 0)),
             text=b"//RenderWare/rwsdk/\0d:\\\\game.xbe\0", others=("game.xbe", "extras/demo.xbe"))
    r = triage_disc(str(d))
    assert not r.get("error") and r["xdk"] == 5849 and r["engines"] == ["RenderWare"]
    # No engine layer is shipped since WP4: the engine is information only.
    assert "engines/renderware" not in r["layers"] and r["title_profile"] is False
    by = {x["path"]: x["named_by_default"] for x in r["xbes"]}
    assert by == {"game.xbe": True, "extras/demo.xbe": False} and r["multi_xbe"] is True


def test_utf16_name_and_single_xbe(tmp_path):
    d = disc(tmp_path, "B", (("XAPILIB", 4627, 0),), text="d:\\menu.xbe".encode("utf-16-le") + b"\0\0",
             others=("menu.xbe",))
    assert triage_disc(str(d))["multi_xbe"] is True
    e = disc(tmp_path, "C", (("XAPILIB", 5849, 0),))
    r = triage_disc(str(e))
    assert r["xbes"] == [] and r["multi_xbe"] is False


def test_image(tmp_path):
    from tools.xiso import mkxiso
    d = disc(tmp_path, "D", (("XAPILIB", 5558, 0),), text=b"launch.xbe\0", others=("launch.xbe",))
    iso = tmp_path / "D.iso"
    mkxiso.build(str(d), str(iso))
    r = triage_disc(str(iso))
    assert not r.get("error"), r
    assert r["xdk"] == 5558 and r["multi_xbe"] is True and r["source"] == "D.iso"


def test_errors_do_not_stop_the_run(tmp_path):
    empty = tmp_path / "E"
    empty.mkdir()
    assert triage_disc(str(empty))["error"] == "no default.xbe"
    bad = tmp_path / "F.txt"
    bad.write_text("x")
    assert triage_disc(str(bad))["error"]


def test_groups_and_table(tmp_path):
    recs = [triage_disc(str(disc(tmp_path, n, libs, text)))
            for n, libs, text in (("G1", (("XAPILIB", 5849, 0),), b"//RenderWare/\0"),
                                  ("G2", (("XAPILIB", 5849, 0),), b""),
                                  ("G3", (("XAPILIB", 4627, 0),), b""))]
    g = groups(recs)
    assert g["xdk"][0][0] == 5849 and len(g["xdk"][0][1]) == 2
    md = markdown(recs + [{"source": "broken.iso", "xbes": [], "error": "no default.xbe"}])
    assert "| broken.iso | | | | | | | | | | error: no default.xbe | | |" in md
    assert "**By XDK build** (biggest first): 5849: 2; 4627: 1" in md
    assert "**Discs that launch another XBE** (XBA-82): 0 of 4" in md
    assert "**Libraries by build**: XAPILIB 5849: 2; XAPILIB 4627: 1" in md
    assert "**Blockers**: none" in md
    header = md.splitlines()[0]
    assert all(len(l.split("|")) == len(header.split("|")) for l in md.splitlines() if l.startswith("| "))


# ── XBA-124: the image facts, stub imports, updates and content ─────────────

def test_the_image_facts(tmp_path):
    """Preload flags, overlapping sections (code in both: a blocker), TLS, the key pair, sizes, the stub
    kernel routes the title imports, libraries with their builds."""
    from tools.xbe_parser.synth import BASE, PRELOAD, W, X, Sec, write_xbe
    d = tmp_path / "H"
    d.mkdir()
    write_xbe(d / "default.xbe", [Sec(".text", b"\xc3" * 64), Sec("D3D", b"\xc3" * 32),
                                  Sec("OVA", b"\x90" * 0x100, flags=X, va=BASE + 0x8000),
                                  Sec("OVB", b"\x90" * 0x80, flags=X, va=BASE + 0x8040),
                                  Sec(".data", b"\1" * 16, flags=W | PRELOAD, vsize=0x1000)],
              debug=True, libs=(("XAPILIB", 5849, 0), ("D3D8", 5849, 1)), kernel_imports=(49, 25, 327, 377),
              title="Facts", title_id=0x58424142)
    stubs = {25: "ExReadWriteRefurbInfo", 377: "MmDbgReleaseAddress"}
    r = triage_disc(str(d), stubs=stubs)
    assert not r.get("error"), r
    assert (r["key"], r["sections"], r["tls"]) == ("debug", 5, False)
    assert r["not_preloaded"] == ["OVA", "OVB"]
    assert r["overlaps"] == [{"read": "OVB", "a": "OVA", "b": "OVB", "va": "0x00018040", "size": 0x80, "code": True}]
    assert r["blockers"] and "OVA and OVB" in r["blockers"][0]
    assert r["kernel_imports"] == 4 and r["stub_imports"] == ["ExReadWriteRefurbInfo", "MmDbgReleaseAddress"]
    assert r["libraries"] == {"D3D8": 5849, "XAPILIB": 5849}
    assert r["probes"] == {"d3d_latch_wait": 0} and r["updates"] == [] and r["content"] == []
    md = markdown([r])
    assert "| Facts | 58424142 | 5849 |" in md and "OVA/OVB (code: **blocker**)" in md
    assert "debug keys" in md and "ExReadWriteRefurbInfo, MmDbgReleaseAddress" in md
    assert "**Blockers**: Facts: sections OVA and OVB" in md
    assert "**Stub kernel routes imported** (titles): ExReadWriteRefurbInfo: 1; MmDbgReleaseAddress: 1" in md
    assert "**Images without TLS** (a TIB per thread all the same): 1 of 1" in md


def test_the_stub_table_is_read_from_the_tree():
    from tools.triage import kernel_stubs
    s = kernel_stubs()
    assert s and all(isinstance(o, int) and n for o, n in s.items())
    assert kernel_stubs("/nonexistent/kernel_stub_table.c") == {}


def test_a_title_update_and_content_are_reported_not_used(tmp_path):
    """An update XBE under $u is not one of the disc's images: reported as an update (version, newer or not),
    in a disc folder and under --tdata; $c sub-folders are content packages."""
    from tools.xbe_parser.synth import write_xbe
    d = tmp_path / "U"
    (d / "$u").mkdir(parents=True)
    write_xbe(d / "default.xbe", title_id=0x58424143, title="Upd")
    write_xbe(d / "$u" / "default.xbe", title_id=0x58424143, title="Upd", version=2)
    r = triage_disc(str(d), stubs={})
    assert r["xbes"] == [] and r["multi_xbe"] is False
    assert r["updates"] == [{"path": "$u/default.xbe", "used": False, "title_id": "58424143", "version": 2,
                             "xdk": 5849, "same_title": True, "newer": True}]
    tdata = tmp_path / "TDATA"
    (tdata / "58424143" / "$u").mkdir(parents=True)
    (tdata / "58424143" / "$c" / "0000000000000001").mkdir(parents=True)
    (tdata / "58424143" / "$c" / "0000000000000002").mkdir(parents=True)
    write_xbe(tdata / "58424143" / "$u" / "default.xbe", title_id=0x58424143, version=1)
    e = tmp_path / "V"
    e.mkdir()
    write_xbe(e / "default.xbe", title_id=0x58424143, title="Upd")
    r = triage_disc(str(e), tdata=str(tdata), stubs={})
    assert [u["path"] for u in r["updates"]] == ["TDATA/58424143/$u/default.xbe"]
    assert r["content"] == ["0000000000000001", "0000000000000002"]
    md = markdown([r])
    assert "update TDATA/58424143/$u/default.xbe v1 (not used); 2 content package(s)" in md
    assert "**Title updates found** (not used): 1; **content packages**: 2" in md
    # Another title's folder under TDATA is not this title's.
    assert triage_disc(str(e), tdata=str(tmp_path / "nothing"), stubs={})["updates"] == []


def test_cli_json_is_sorted_and_complete(tmp_path):
    import json
    from tools.triage.__main__ import main
    d = disc(tmp_path, "J", (("XAPILIB", 5849, 0),))
    out = tmp_path / "t.json"
    md = tmp_path / "t.md"
    assert main([str(d), "--json", str(out), "--markdown", str(md)]) == 0
    rec = json.loads(out.read_text())[0]
    for k in ("key", "sections", "section_bytes", "not_preloaded", "overlaps", "tls", "stub_imports", "libraries",
              "probes", "blockers", "updates", "content", "warnings", "section_table", "kernel_imports"):
        assert k in rec, k
    assert out.read_text() == json.dumps(json.loads(out.read_text()), indent=1, sort_keys=True)
    assert md.read_text().startswith("| Title | Title ID | XDK |")
