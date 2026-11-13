"""tools.xbe_ident on synthetic XBEs (no game data): XDK version, engines, layers, CLI."""
import json
import struct

import pytest

from tools.xbe_ident import engines_in, identify, load_engines, summary
from tools.xbe_ident.__main__ import main

BASE = 0x10000
HDR = 0x1000


def make_xbe(path, libs=(("XAPILIB", 5849, 0), ("D3D8", 5849, 0)), xapi_index=0, text=b"",
             title_id=0x4D530001, title="Test", imports=()):
    """Header + certificate + library table in the first page, one section holding `text`.
    xapi_index: the library entry the header's XAPI pointer names (None: no pointer).
    imports: kernel ordinals for the thunk table (at most 63)."""
    assert len(imports) < 64
    raw = text + b"\0" * 16
    buf = bytearray(HDR + len(raw))
    buf[0:4] = b"XBEH"
    struct.pack_into("<4I", buf, 0x104, BASE, HDR, HDR + len(raw), 0x178)
    struct.pack_into("<I", buf, 0x118, BASE + 0x200)                    # certificate
    struct.pack_into("<II", buf, 0x11C, 1, BASE + 0x600)                # one section header
    struct.pack_into("<I", buf, 0x128, (BASE + HDR) ^ 0xA8FC57AB)       # retail entry
    struct.pack_into("<I", buf, 0x158, (BASE + 0x800) ^ 0x5B6D40B6)     # thunk table
    for i, o in enumerate(imports):
        struct.pack_into("<I", buf, 0x800 + 4 * i, 0x80000000 | o)
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
    assert r["xdk"]["build"] == 0 and r["layers"] == [] and r["imports"] == []
    # Stripped, but D3D's kernel calls are there (XBA-106): the D3D8 layer by its imports.
    r = identify(make_xbe(tmp_path / "e2.xbe", libs=(), imports=(3, 1, 168, 49)))
    assert r["imports"] == [1, 3, 49, 168] and r["layers"] == ["xdk/d3d8"]
    assert summary(r).startswith("XDK unknown, no known engine")


def test_engines_need_their_minimum(tmp_path):
    text = (b"@@(#)$Id: //RenderWare/RW36Active/rwsdk/src/babinary.c#1 $\0"
            b"NiNode\0"                       # one Gamebryo marker of the two needed
            b"Copyright RAD Game Tools\0Bink\0")
    r = identify(make_xbe(tmp_path / "f.xbe", text=text))
    names = [e["name"] for e in r["engines"]]
    assert names == ["RenderWare", "Bink"]
    # profiles/engines/renderware exists (XBA-95); middleware (Bink) is never a layer.
    assert r["layers"] == ["xdk/d3d8", "xdk/xapi", "engines/renderware"]
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


# D3D's far-behind probe as docs/technical/d3d8ltcg-device-context.md shows it (our own bytes of that shape).
LATCH_LOOP = bytes.fromhex(
    "8b9634090000"      # mov edx, [esi+0x934]      register base
    "8b7e30"            # mov edi, [esi+0x30]       GPU semaphore
    "8b82100b4000"      # loop: mov eax, [edx+0x400B10]
    "8b0f"              # mov ecx, [edi]
    "c1e102"            # shl ecx, 2
    "33c8"              # xor ecx, eax
    "f6c17c"            # test cl, 0x7C
    "75ef")             # jne loop


def test_the_d3d_latch_wait_probe(tmp_path):
    """XBA-105: the DMA walk is chosen by what the title's D3D does. The loop in code counts; the same
    bytes in a data section, or the register read without the fence-bit test, do not."""
    from tools.xbe_parser.synth import PRELOAD, W, X, Sec, write_xbe
    p = write_xbe(tmp_path / "a.xbe", [Sec(".text", b"\x90" * 7 + LATCH_LOOP + b"\xc3")],
                  libs=(("XAPILIB", 5455, 0), ("D3D8LTCG", 5455, 0)))
    r = identify(p)
    assert r["probes"] == {"d3d_latch_wait": 1}
    assert "xdk/d3d8ltcg" in r["layers"]                     # any D3D8LTCG build with the loop
    absolute = bytes.fromhex("a1100b40fd" "a87c" "75f9")     # mov eax, [0xFD400B10]; test al, 0x7C; jne
    p = write_xbe(tmp_path / "b.xbe", [Sec(".text", absolute + b"\xc3")], libs=(("D3D8LTCG", 5849, 0),))
    assert identify(p)["probes"] == {"d3d_latch_wait": 1}
    p = write_xbe(tmp_path / "c.xbe", [Sec(".text", b"\xc3" * 16), Sec(".rdata", LATCH_LOOP, flags=PRELOAD)],
                  libs=(("D3D8LTCG", 5788, 0),))
    r = identify(p)
    assert r["probes"] == {"d3d_latch_wait": 0} and "xdk/d3d8ltcg" not in r["layers"]
    # Build 5849 keeps the layer when the scan finds nothing (or_builds: Black, Burnout 3).
    p = write_xbe(tmp_path / "c2.xbe", [Sec(".text", b"\xc3" * 16)], libs=(("D3D8LTCG", 5849, 0),))
    r = identify(p)
    assert r["probes"] == {"d3d_latch_wait": 0} and "xdk/d3d8ltcg" in r["layers"]
    no_test = LATCH_LOOP.replace(bytes.fromhex("f6c17c"), bytes.fromhex("85c9" "90"))
    p = write_xbe(tmp_path / "d.xbe", [Sec(".text", no_test, flags=PRELOAD | X | W)], libs=(("D3D8", 5849, 0),))
    assert identify(p)["probes"] == {"d3d_latch_wait": 0}


def test_overlapping_code_sections_are_a_blocker(tmp_path):
    from tools.xbe_parser.synth import BASE as B, X, Sec, write_xbe
    from tools.xbe_ident import blockers
    p = write_xbe(tmp_path / "o.xbe", [Sec(".text", b"\xc3" * 16), Sec("OVA", b"\x90" * 64, flags=X, va=B + 0x3000),
                                       Sec("OVB", b"\x90" * 64, flags=X, va=B + 0x3000)])
    r = identify(p)
    assert r["image"]["overlaps"][0]["code"] is True and r["image"]["not_preloaded"] == ["OVA", "OVB"]
    assert blockers(r) == ["sections OVA and OVB both hold code at 0x00013000 (+0x40): only one of them is lifted "
                           "(overlapping code sections, XBA-124)"]
    assert blockers(identify(write_xbe(tmp_path / "n.xbe"))) == []
