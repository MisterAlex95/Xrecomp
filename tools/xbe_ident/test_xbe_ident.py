"""tools.xbe_ident on synthetic XBEs (no game data): XDK version, engines, layers, CLI."""
import json
import struct

import pytest

from tools.xbe_ident import engines_in, identify, load_engines, summary
from tools.xbe_ident.__main__ import main

BASE = 0x10000
HDR = 0x1000


def make_xbe(path, libs=(("XAPILIB", 5849, 0), ("D3D8", 5849, 0)), xapi_index=0, text=b"",
             title_id=0x4D530001, title="Test"):
    """Header + certificate + library table in the first page, one section holding `text`.
    xapi_index: the library entry the header's XAPI pointer names (None: no pointer)."""
    raw = text + b"\0" * 16
    buf = bytearray(HDR + len(raw))
    buf[0:4] = b"XBEH"
    struct.pack_into("<4I", buf, 0x104, BASE, HDR, HDR + len(raw), 0x178)
    struct.pack_into("<I", buf, 0x118, BASE + 0x200)                    # certificate
    struct.pack_into("<II", buf, 0x11C, 1, BASE + 0x600)                # one section header
    struct.pack_into("<I", buf, 0x128, (BASE + HDR) ^ 0xA8FC57AB)       # retail entry
    struct.pack_into("<I", buf, 0x158, (BASE + 0x800) ^ 0x5B6D40B6)     # empty thunk table
    lib_va = BASE + 0x900
    struct.pack_into("<II", buf, 0x160, len(libs), lib_va)
    if xapi_index is not None:
        struct.pack_into("<I", buf, 0x16C, lib_va + 16 * xapi_index)
    for i, (name, build, flags) in enumerate(libs):
        o = 0x900 + 16 * i
        buf[o:o + 8] = name.encode().ljust(8, b"\0")
        struct.pack_into("<4H", buf, o + 8, 1, 0, build, flags)
    struct.pack_into("<I", buf, 0x200 + 8, title_id)
    t = title.encode("utf-16-le")
    buf[0x20C:0x20C + len(t)] = t
    struct.pack_into("<6I", buf, 0x600, 0x6, BASE + HDR, len(raw), HDR, len(raw), BASE + 0x700)
    buf[0x700:0x706] = b".text\0"
    buf[HDR:] = raw
    path.write_bytes(bytes(buf))
    return str(path)


def test_xdk_from_the_header_pointer(tmp_path):
    p = make_xbe(tmp_path / "a.xbe", libs=(("D3D8", 5933, 0), ("XAPILIB", 5849, 3)), xapi_index=1)
    r = identify(p)
    assert r["title_id"] == "4D530001" and r["title"] == "Test"
    x = r["xdk"]
    assert (x["build"], x["source"], x["mixed"], x["qfe"]) == (5849, "header", True, 3)
    assert x["libraries"] == {"D3D8": 5933, "XAPILIB": 5849}
    assert r["layers"] == ["xdk/d3d8", "xdk/xapi"]      # profiles/xdk/<name>/layer.json "applies"


def test_xdk_by_name_then_most_common(tmp_path):
    p = make_xbe(tmp_path / "b.xbe", libs=(("D3D8", 4627, 0), ("XAPILIB", 4361, 0)), xapi_index=None)
    assert identify(p)["xdk"]["build"] == 4361 and identify(p)["xdk"]["source"] == "XAPILIB"
    p = make_xbe(tmp_path / "c.xbe", libs=(("D3D8", 4627, 0), ("DSOUND", 4627, 0), ("XNET", 4361, 0)),
                 xapi_index=None)
    x = identify(p)["xdk"]
    assert (x["build"], x["source"], x["mixed"]) == (4627, "most common", True)


def test_a_header_pointer_outside_the_table_is_ignored(tmp_path):
    p = make_xbe(tmp_path / "d.xbe", libs=(("XAPILIB", 5558, 0),), xapi_index=7)
    assert identify(p)["xdk"]["source"] == "XAPILIB"


def test_no_library_table(tmp_path):
    r = identify(make_xbe(tmp_path / "e.xbe", libs=()))
    assert r["xdk"]["build"] == 0 and r["layers"] == []
    assert summary(r).startswith("XDK unknown, no known engine")


def test_engines_need_their_minimum(tmp_path):
    text = (b"@@(#)$Id: //RenderWare/RW36Active/rwsdk/src/babinary.c#1 $\0"
            b"NiNode\0"                       # one Gamebryo marker of the two needed
            b"Copyright RAD Game Tools\0Bink\0")
    r = identify(make_xbe(tmp_path / "f.xbe", text=text))
    names = [e["name"] for e in r["engines"]]
    assert names == ["RenderWare", "Bink"]
    # No profiles/engines/renderware yet: not a layer (tools/prepare/test_profiles.py has one that is).
    assert r["layers"] == ["xdk/d3d8", "xdk/xapi"]
    assert summary(r) == "XDK 5849, RenderWare; middleware Bink"


def test_two_markers_make_a_match():
    found = engines_in(b"..VEngineClient013..VClient017..")
    assert [e["layer"] for e in found] == ["source"]
    assert found[0]["markers"] == ["VEngineClient", "VClient0"]


def test_the_table_is_well_formed():
    table = load_engines()
    layers = [e["layer"] for e in table]
    assert len(layers) == len(set(layers))
    assert all(e["kind"] in ("engine", "middleware") for e in table)
    assert all(m.isascii() for e in table for m in e["markers"])


def test_not_an_xbe(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"MZ" + b"\0" * 4096)
    with pytest.raises(ValueError):
        identify(str(p))


def test_cli_triage_keeps_going_past_a_bad_file(tmp_path, capsys):
    good = make_xbe(tmp_path / "g.xbe", title="Good")
    bad = tmp_path / "bad.xbe"
    bad.write_bytes(b"nope")
    out = tmp_path / "out.json"
    assert main([good, str(bad), "--json", str(out)]) == 1
    rows = json.loads(out.read_text())
    assert [r["title"] for r in rows] == ["Good"]
    assert "4D530001  Good" in capsys.readouterr().out


def test_cli_one_xbe(tmp_path, capsys):
    p = make_xbe(tmp_path / "h.xbe")
    out = tmp_path / "one.json"
    assert main([p, "--json", str(out)]) == 0
    assert json.loads(out.read_text())["layers"] == ["xdk/d3d8", "xdk/xapi"]
    assert "layers: xdk/d3d8 xdk/xapi" in capsys.readouterr().out


def test_prepare_writes_ident_json(tmp_path):
    from tools.prepare.__main__ import identify_xbe
    p = make_xbe(tmp_path / "p.xbe", text=b"Miles Sound System\0")
    r = identify_xbe(p, str(tmp_path))
    assert r["summary"] == "XDK 5849, no known engine; middleware Miles Sound System"
    assert json.loads((tmp_path / "ident.json").read_text())["layers"] == ["xdk/d3d8", "xdk/xapi"]
    bad = tmp_path / "bad.xbe"
    bad.write_bytes(b"nope")
    assert identify_xbe(str(bad), str(tmp_path)) is None      # the prepare goes on without it
