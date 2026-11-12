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
    assert "engines/renderware" in r["layers"] and r["title_profile"] is False
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
    assert "| broken.iso | | | | error: no default.xbe | |" in md
    assert "**By XDK build** (biggest first): 5849: 2; 4627: 1" in md
    assert "**Discs that launch another XBE** (XBA-82): 0 of 4" in md
