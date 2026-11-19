"""tools.xmv.find_api on synthetic titles of our own (tools/xmv/synth_title.py): the XMV API is found by
structure and by recorded signature, nothing is found in a title without movies, and the lifter's
binding (tools/recomp/xmv_bind.py) is what Prepare hands on (XBA-199)."""

import json
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.recomp import xmv_bind                       # noqa: E402
from tools.xmv import find_api                          # noqa: E402
from tools.xmv.synth_title import movie_title           # noqa: E402


def _find(tmp_path, sigs=(), **kw):
    p, labels = movie_title(tmp_path / "default.xbe", **kw)
    return find_api.find(p, signatures=list(sigs)), labels, p


def test_the_api_is_found_in_its_section(tmp_path):
    r, l, _ = _find(tmp_path)
    assert r["section"]["name"] == "XMV"
    assert r["high"] and r["confidence"] >= 0.9
    assert r["best"] == {"get_next_frame": l["get_next_frame"], "close_decoder": l["close"],
                         "create_decoder": l["create"]}
    top = r["candidates"][0]["evidence"]
    for k in ("loop", "result", "locals", "create", "close_xmv", "movie", "section"):
        assert k in top, top
    # The title's own stdcall function with four arguments is a candidate too, far behind.
    decoy = next(c for c in r["candidates"] if c["va"] == l["decoy"])
    assert decoy["score"] < 0.3 and "outside_section" in decoy["evidence"]
    assert [s["text"] for s in r["strings"]] == ["D:\\intro.xmv"]
    assert r["movie_functions"] == [l["entry"]]


def test_no_section_of_its_own_still_found_through_the_movie_name(tmp_path):
    r, l, _ = _find(tmp_path, section=".text2")
    assert r["section"] is None
    assert r["high"] and r["best"]["get_next_frame"] == l["get_next_frame"]
    assert "movie" in r["candidates"][0]["evidence"]


def test_shape_alone_is_not_enough(tmp_path):
    """No XMV section and no movie name: the call pattern alone is a guess, not bound."""
    r, l, _ = _find(tmp_path, section=".text2", strings=False)
    assert r["best"]["get_next_frame"] == l["get_next_frame"]
    assert not r["high"] and r["confidence"] < find_api.HIGH
    assert find_api.bind_plan(r) is None


def test_a_title_without_movies_binds_nothing(tmp_path):
    r, _, p = _find(tmp_path, movie=False)
    assert r["best"]["get_next_frame"] is None and not r["high"]
    assert r["strings"] == [] and r["section"] is None and r["library"] is None
    assert find_api.prepare_line(r, None, "auto") is None       # nothing to say
    out = tmp_path / "an"
    assert find_api.prepare_bind(p, str(out), log=lambda m: None) is None
    assert not (out / "xmv_bind.json").exists()
    assert json.loads((out / "xmv_api.json").read_text())["high"] is False


def test_an_xmv_library_entry_counts(tmp_path):
    libs = (("XAPILIB", 5849, 0), ("XMV", 5849, 0))
    r, _, _ = _find(tmp_path, section=".text2", strings=False, libs=libs)
    assert r["library"] == {"name": "XMV", "build": 5849}
    assert "library" in r["candidates"][0]["evidence"]


def test_prepare_bind_and_the_native_switch(tmp_path, monkeypatch):
    p, l = movie_title(tmp_path / "default.xbe")
    lines = []
    monkeypatch.delenv("RECOMP_XMV", raising=False)
    bind = find_api.prepare_bind(p, str(tmp_path / "an"), log=lines.append)
    plan = json.loads(open(bind).read())
    assert plan["get_next_frame"] == "0x%08X" % l["get_next_frame"]
    assert plan["close_decoder"] == "0x%08X" % l["close"]
    assert "create_decoder" not in plan                  # found, reported, never bound
    assert "bound to the runtime's decoder" in lines[0], lines
    key = find_api.bind_key(bind)
    assert key and find_api.bind_key(None) == ""

    monkeypatch.setenv("RECOMP_XMV", "native")
    lines.clear()
    assert find_api.prepare_bind(p, str(tmp_path / "an"), log=lines.append) is None
    assert not (tmp_path / "an" / "xmv_bind.json").exists()
    assert "RECOMP_XMV=native" in lines[0]


def test_a_recorded_signature_finds_it_without_the_structure(tmp_path):
    """--record on one title (its XMV section and movie name); another title of the same build, where only the
    call shape is left, is then found with high confidence by the signature."""
    root = tmp_path / "xdk"
    a = tmp_path / "a"
    a.mkdir()
    pa, la = movie_title(a / "default.xbe")
    out, added = find_api.record(pa, {"get_next_frame": la["get_next_frame"], "close_decoder": la["close"]},
                                 root=str(root))
    assert out == str(root / "5849" / "xmv.json") and set(added) == {"get_next_frame", "close_decoder"}
    db = json.loads(open(out).read())
    assert db["build"] == 5849 and len(db["functions"]["get_next_frame"]) == 1
    # Recording again adds nothing.
    assert find_api.record(pa, {"get_next_frame": la["get_next_frame"]}, root=str(root))[1] == []
    sigs = find_api.load_signatures(str(root))
    r, l, _ = _find(tmp_path, sigs=sigs, section=".text2", strings=False)
    assert r["high"] and r["best"]["get_next_frame"] == l["get_next_frame"]
    assert "signature" in r["candidates"][0]["evidence"]


def test_a_title_that_plays_through_the_library_is_never_bound(tmp_path, monkeypatch):
    """XBA-309: the title hands its movie to the library's own player (XMVDecoder_Play's shape), which polls
    GetNextFrame itself: GetNextFrame has no call site outside the XMV section. Bound in a retail title, Play
    presented 157 of the 1765 frames the hook decoded (its clock presumably never started), so it is found but
    never bound, not even with a recorded signature of its build."""
    r, l, p = _find(tmp_path, play=True)
    assert r["best"]["get_next_frame"] == l["get_next_frame"]
    assert r["library_only"] and not r["high"]
    assert find_api.bind_plan(r) is None
    # The plain title (the title polls GetNextFrame itself) is not affected.
    plain = tmp_path / "plain"
    plain.mkdir()
    rp, _, _ = _find(plain)
    assert rp["high"] and not rp["library_only"]
    # A signature of the title's own build lifts the score to the top, still not bound.
    root = tmp_path / "xdk"
    find_api.record(p, {"get_next_frame": l["get_next_frame"]}, root=str(root))
    r = find_api.find(p, signatures=find_api.load_signatures(str(root)))
    assert "signature" in r["candidates"][0]["evidence"] and r["confidence"] >= find_api.HIGH
    assert r["library_only"] and not r["high"]
    # What Prepare logs, and no bind file.
    lines = []
    monkeypatch.delenv("RECOMP_XMV", raising=False)
    assert find_api.prepare_bind(p, str(tmp_path / "an"), log=lines.append) is None
    assert not (tmp_path / "an" / "xmv_bind.json").exists()
    assert "called only by the XMV library" in lines[0] and "XMVDecoder_Play" in lines[0], lines
    assert "never bound" in find_api.format_report(r)


def test_record_refuses_a_function_of_the_wrong_shape(tmp_path):
    p, l = movie_title(tmp_path / "default.xbe")
    with pytest.raises(SystemExit, match="ret 0x10"):
        find_api.record(p, {"get_next_frame": l["close"]}, root=str(tmp_path / "xdk"))


def test_the_cli_report(tmp_path, capsys):
    p, l = movie_title(tmp_path / "default.xbe")
    assert find_api.main([p]) == 0
    text = capsys.readouterr().out
    assert "XMV section: XMV" in text and "confidence" in text and "0x%08X" % l["get_next_frame"] in text
    assert find_api.main([p, "--json"]) == 0
    j = json.loads(capsys.readouterr().out)
    assert j["best"]["get_next_frame"] == "0x%08X" % l["get_next_frame"] and j["high"]


def test_the_generated_binding(tmp_path):
    plan = {"confidence": 0.9, "get_next_frame": 0x12050, "close_decoder": 0x12080}
    bound = xmv_bind.select(plan, {0x12050, 0x12080}, set(), log=lambda m: None)
    assert bound == {"get_next_frame": 0x12050, "close_decoder": 0x12080}
    c = xmv_bind.source(bound, plan)
    assert "void sub_00012050(void)" in c and "sub_00012050_gen();" in c
    assert "xmv_hle_get_next_frame_ex(MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16)," in c
    assert "0u);  /* timestamp: XMV_TIME_MS32" in c        # a plan without a layout: one DWORD in ms
    c64 = xmv_bind.source(bound, dict(plan, timestamp="100ns64"))
    assert "1u);  /* timestamp: XMV_TIME_100NS64" in c64
    assert "esp += 4 + 16;" in c
    assert "xmv_hle_close(MEM32(esp + 4));" in c and "sub_00012080_gen();" in c
    assert "xmv_hle_route(0x00012050u, 90u, decoder)" in c and "xmv_hle_native_result(decoder, result);" in c
    # Not a function the analysis found, or written by hand: nothing bound.
    assert xmv_bind.select(plan, {0x12080}, set(), log=lambda m: None) == {}
    assert xmv_bind.select(plan, {0x12050, 0x12080}, {0x12050}, log=lambda m: None) == {}
    assert xmv_bind.select(plan, {0x12050}, set(), log=lambda m: None) == {"get_next_frame": 0x12050}
    f = tmp_path / "b.json"
    f.write_text(json.dumps({"get_next_frame": "0x00012050", "confidence": 0.8}))
    assert xmv_bind.load(str(f)) == {"get_next_frame": 0x12050, "confidence": 0.8, "xdk": None, "timestamp": "ms32"}
    f.write_text(json.dumps({"get_next_frame": "0x00012050", "confidence": 0.8, "timestamp": "100ns64"}))
    assert xmv_bind.load(str(f))["timestamp"] == "100ns64"
    f.write_text(json.dumps({"get_next_frame": "0x00012050", "confidence": 0.8, "timestamp": "us48"}))
    with pytest.raises(ValueError):
        xmv_bind.load(str(f))


@pytest.mark.parametrize("store, build, layout, source, width", [
    ("dword", 5933, "ms32", "body", 4),          # the store read in the body
    ("qword", 5933, "100ns64", "body", 8),
    ("dword", 5849, "ms32", "body", 4),          # agrees with the build's entry
    ("qword", 5849, "ms32", "conflict", 8),      # disagrees: the DWORD, never past the field
    (None, 5849, "ms32", "build", None),         # esp-only body, not read: the build's entry
    (None, 5933, "ms32", "default", None),       # neither: assumed one DWORD in ms
])
def test_the_timestamp_layout(tmp_path, monkeypatch, store, build, layout, source, width):
    """XBA-316: what the runtime writes at *pTimeStamp, from the title's own GetNextFrame, its XDK build, or the
    default; it reaches the bind plan, the prepare log and the generated binding."""
    libs = (("XAPILIB", build, 0), ("XMV", build, 0))
    r, l, p = _find(tmp_path, libs=libs, time_store=store)
    assert r["high"] and r["best"]["get_next_frame"] == l["get_next_frame"]
    assert r["timestamp"] == {"layout": layout, "source": source, "store_bytes": width,
                              "build_entry": find_api.TIME_BY_BUILD.get(build)}
    monkeypatch.delenv("RECOMP_XMV", raising=False)
    lines = []
    plan = json.loads(open(find_api.prepare_bind(p, str(tmp_path / "an"), log=lines.append)).read())
    assert plan["timestamp"] == layout and plan["timestamp_source"] == source
    assert "frame times written as " in lines[0], lines
    assert ("assumed" in lines[0]) == (source == "default"), lines
    assert "timestamp: " in find_api.format_report(r)
    want = xmv_bind.TIME_LAYOUT[layout][0]
    assert "%du);  /* timestamp:" % want in xmv_bind.source({"get_next_frame": l["get_next_frame"]}, plan)


def test_only_the_plain_store_shape_is_read(tmp_path):
    """No ebp frame, or no 4th argument: no width (the build's entry or the default decides)."""
    p, l = movie_title(tmp_path / "default.xbe", time_store="dword")
    img = find_api.Image(p)
    assert find_api.timestamp_store(img, l["get_next_frame"]) == 4
    assert find_api.timestamp_store(img, l["close"]) is None        # no ebp frame
    assert find_api.timestamp_store(img, l["create"]) is None       # ebp frame, no 4th argument
