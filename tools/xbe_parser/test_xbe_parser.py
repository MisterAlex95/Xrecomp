"""tools.xbe_parser on synthetic XBEs from our own writer (tools/xbe_parser/synth.py,
XBA-124): the key pairs, odd headers, sections loaded on demand and sections
that share a virtual address. No game data."""

import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.xbe_parser.synth import BASE, PRELOAD, W, X, Sec, build_xbe       # noqa: E402
from tools.xbe_parser.xbe_parser import XBEParser, image_report, section_overlaps   # noqa: E402


def parse(data):
    return XBEParser.from_bytes(data).parse()


def test_a_retail_image():
    x = parse(build_xbe([Sec(".text", b"\xc3" * 32), Sec(".data", b"\1" * 8, flags=W | PRELOAD, vsize=64)],
                        kernel_imports=(49, 327, 328), title="Retail", title_id=0x4D530042))
    h = x.header
    assert (h.entry_key, h.is_debug, h.entry_point) == ("retail", False, BASE + 0x1000)
    assert [i.name for i in x.kernel_imports] == ["HalReturnToFirmware", "XeLoadSection", "XeUnloadSection"]
    assert (x.certificate.title_id, x.certificate.title_name) == (0x4D530042, "Retail")
    assert [s.name for s in x.sections] == [".text", ".data"] and x.warnings == []
    assert [(l.name, l.build) for l in x.libraries] == [("XAPILIB", 5849)]


def test_a_debug_image_decodes_with_the_debug_keys():
    x = parse(build_xbe([Sec(".text", b"\xc3" * 32)], debug=True, kernel_imports=(8,)))
    assert (x.header.entry_key, x.header.is_debug) == ("debug", True)
    assert x.header.entry_point == BASE + 0x1000
    assert [i.name for i in x.kernel_imports] == ["DbgPrint"]


def test_a_chihiro_image_decodes_with_the_chihiro_keys():
    from tools.xbe_parser.xbe_parser import (ENTRY_CHIHIRO_XOR, ENTRY_RETAIL_XOR, THUNK_CHIHIRO_XOR,
                                             THUNK_RETAIL_XOR)
    data = bytearray(build_xbe([Sec(".text", b"\xc3" * 32)], kernel_imports=(49,)))
    for off, rk, ck in ((0x128, ENTRY_RETAIL_XOR, ENTRY_CHIHIRO_XOR), (0x158, THUNK_RETAIL_XOR, THUNK_CHIHIRO_XOR)):
        struct.pack_into("<I", data, off, struct.unpack_from("<I", data, off)[0] ^ rk ^ ck)
    x = parse(bytes(data))
    assert (x.header.entry_key, x.header.is_debug, x.header.entry_point) == ("chihiro", False, BASE + 0x1000)
    assert [i.name for i in x.kernel_imports] == ["HalReturnToFirmware"] and x.warnings == []


def test_neither_key_keeps_retail_and_warns():
    data = bytearray(build_xbe())
    struct.pack_into("<I", data, 0x128, 0x12345678)      # an entry no key decodes into the image
    struct.pack_into("<I", data, 0x158, 0x12345678)
    x = parse(bytes(data))
    assert x.header.entry_key == "unknown" and not x.header.is_debug
    assert x.header.entry_point == 0x12345678 ^ 0xA8FC57AB
    assert any("neither the retail nor the debug key" in w for w in x.warnings)


def test_a_header_bigger_than_a_page():
    """Many libraries push the table past the first header page; VAs in the
    header area are the header's, not a section's."""
    libs = tuple(("LIB%d" % i, 5000 + i, 0) for i in range(8))
    x = parse(build_xbe([Sec(".text", b"\xc3" * 16)], header_size=0x3000, libs=libs, library_table_at=0x2400))
    assert x.header.headers_size == 0x3000 and x.header.entry_point == BASE + 0x3000
    assert [l.build for l in x.libraries] == [5000 + i for i in range(8)] and x.warnings == []


def test_no_certificate_parses_with_a_warning():
    x = parse(build_xbe(certificate=False))
    assert x.certificate.title_id == 0 and x.certificate.title_name == ""
    assert any("no certificate" in w for w in x.warnings)


def test_the_certificate_fields():
    """The alternate signature keys are the 16 x 16 bytes that end the oldest
    certificate (0xD0-0x1D0), whatever size a newer one says."""
    data = bytearray(build_xbe(cert_size=0x1D0, version=3))
    data[0x180 + 0xD0:0x180 + 0xE0] = b"K" * 16
    data[0x180 + 0x1C0:0x180 + 0x1D0] = b"L" * 16
    c = parse(bytes(data)).certificate
    assert c.version == 3 and len(c.alt_signature_keys) == 16
    assert c.alt_signature_keys[0] == b"K" * 16 and c.alt_signature_keys[15] == b"L" * 16


def test_a_certificate_cut_by_the_end_of_the_file():
    data = bytearray(build_xbe(sections=[]))
    struct.pack_into("<I", data, 0x118, BASE + len(data) - 40)     # 40 bytes of certificate left
    x = parse(bytes(data))
    assert x.certificate.version == 0
    assert any("cut by the end of the file" in w for w in x.warnings)


def test_no_library_table_and_one_cut_short():
    assert parse(build_xbe(libs=())).libraries == []
    data = bytearray(build_xbe())
    struct.pack_into("<I", data, 0x160, 1000)                       # 1000 entries announced
    x = parse(bytes(data))
    assert 1 <= len(x.libraries) < 1000
    assert any("library table: 1000 entries announced" in w for w in x.warnings)


def test_what_cannot_be_an_xbe_raises():
    with pytest.raises(ValueError):
        parse(b"XBEH" + b"\0" * 64)
    with pytest.raises(ValueError):
        parse(b"MZ" * 400)
    data = bytearray(build_xbe())
    struct.pack_into("<I", data, 0x11C, 100000)                     # a section table past the end
    with pytest.raises(ValueError, match="section table"):
        parse(bytes(data))


def test_tls_present_or_not():
    assert parse(build_xbe(tls=True)).tls.zero_fill_size == 16
    assert parse(build_xbe()).tls is None


def test_overlapping_sections_and_shared_pages():
    """Two sections at one VA (alternatives, loaded one at a time) overlap;
    two that only share a page do not."""
    code = BASE + 0x1000
    secs = [Sec(".text", b"\xc3" * 0x100),
            Sec("OVL_A", b"\x0a" * 0x800, flags=X, va=BASE + 0x3000),
            Sec("OVL_B", b"\x0b" * 0x400, flags=X, va=BASE + 0x3200),
            Sec("D1", b"\1" * 0x10, flags=W | PRELOAD, va=BASE + 0x5000, vsize=0x800),
            Sec("D2", b"\2" * 0x10, flags=W | PRELOAD, va=BASE + 0x5800, vsize=0x800)]
    x = parse(build_xbe(secs))
    assert x.header.entry_point == code
    assert section_overlaps(x.sections) == [(1, 2, BASE + 0x3200, BASE + 0x3600)]
    r = image_report(x)
    assert r["overlaps"] == [{"read": "OVL_B", "a": "OVL_A", "b": "OVL_B", "va": "0x00013200", "size": 0x400,
                              "code": True}]
    assert r["not_preloaded"] == ["OVL_A", "OVL_B"]
    assert (r["section_count"], r["tls"], r["key"]) == (5, False, "retail")
    # D1 and D2 share the page at 0x15000: one reference count for both.
    d1, d2 = x.sections[3], x.sections[4]
    assert d1.tail_shared_page_refcount_addr == d2.head_shared_page_refcount_addr


def test_the_report_keys_are_stable():
    r = image_report(parse(build_xbe(tls=True)))
    assert sorted(r) == ["headers_size", "image_size", "key", "not_preloaded", "overlaps", "section_bytes",
                         "section_count", "sections", "tls", "warnings"]
    assert sorted(r["sections"][0]) == ["executable", "flags", "index", "name", "preload", "raw_size", "va",
                                        "vsize", "writable"]


def test_the_cli_json_carries_preload_and_overlaps(tmp_path):
    import json
    import subprocess
    p = tmp_path / "a.xbe"
    p.write_bytes(build_xbe([Sec(".text", b"\xc3" * 16), Sec("LAZY", b"\0" * 16, flags=X)]))
    out = tmp_path / "a.json"
    subprocess.run([sys.executable, "-m", "tools.xbe_parser", str(p), "--json", str(out), "--quiet"],
                   check=True, cwd=os.path.join(os.path.dirname(__file__), "..", ".."))
    j = json.loads(out.read_text())
    assert [s["preload"] for s in j["sections"]] == [True, False]
    assert j["overlaps"] == [] and j["warnings"] == [] and j["key"] == "retail"


def test_every_static_reader_picks_the_same_overlapping_section(tmp_path):
    """Overlapping sections (XBA-138): every tool that maps an address to file
    bytes reads the same section there. Code before data (the code has one
    meaning); then the one in memory at boot, the last PRELOAD one in table
    order, else the last. Two code sections stay a blocker."""
    import json
    import subprocess
    from tools.abi_analysis.xbe_min import XbeFile
    from tools.disasm.loader import load_image
    from tools.func_id import config as fcfg
    from tools.recomp import config as rcfg

    secs = [Sec(".text", b"\xc3" * 0x100),
            # Each pair puts first in the table the one that must not be read:
            # before XBA-138 every reader took the first covering section.
            # Preloaded data over code loaded on demand: the code.
            Sec("OVD", b"\xdd" * 0x100, flags=W | PRELOAD, va=BASE + 0x3000),
            Sec("OVC", b"\xcc" * 0x100, flags=X, va=BASE + 0x3000),
            # data / data: the preload one, although later in the table
            Sec("DB", b"\xb2" * 0x100, flags=W, va=BASE + 0x5000),
            Sec("DA", b"\xa1" * 0x100, flags=W | PRELOAD, va=BASE + 0x5000),
            # code / code: the preload one is read, and it is a blocker
            Sec("CB", b"\xcb" * 0x100, flags=X, va=BASE + 0x7000),
            Sec("CA", b"\xca" * 0x100, flags=X | PRELOAD, va=BASE + 0x7000)]
    xbe = tmp_path / "ov.xbe"
    xbe.write_bytes(build_xbe(secs))
    want = {BASE + 0x3080: 0xCC, BASE + 0x5080: 0xA1, BASE + 0x7080: 0xCA}

    r = image_report(XBEParser(str(xbe)).parse())
    assert [(o["a"], o["b"], o["read"], o["code"]) for o in r["overlaps"]] == [
        ("OVD", "OVC", "OVC", False), ("DB", "DA", "DA", False), ("CB", "CA", "CA", True)]

    data = xbe.read_bytes()
    seen = {}
    abi = XbeFile(str(xbe))
    seen["abi_analysis"] = {va: abi.read_bytes_at_va(va, 1)[0] for va in want}

    analysis = tmp_path / "ov_analysis.json"
    subprocess.run([sys.executable, "-m", "tools.xbe_parser", str(xbe), "--json", str(analysis), "--quiet"],
                   check=True, cwd=os.path.join(os.path.dirname(__file__), "..", ".."))
    img = load_image(str(xbe), str(analysis))
    seen["disasm"] = {va: img.read_bytes_at_va(va, 1)[0] for va in want}
    assert img.get_section_at_va(BASE + 0x3080).name == "OVC"

    saved_r, saved_f = dict(vars(rcfg)), dict(vars(fcfg))
    saved_rank = dict(fcfg._LOOKUP_RANK)
    try:
        rcfg.configure_from_xbe(str(xbe))
        seen["recomp"] = {va: data[rcfg.va_to_file_offset(va)] for va in want}
        assert rcfg.is_code_address(BASE + 0x3080) and not rcfg.is_code_address(BASE + 0x5080)
        fcfg.configure_from_xbe(str(xbe))
        seen["func_id"] = {va: data[fcfg.va_to_file_offset(va)] for va in want}
    finally:
        vars(rcfg).update(saved_r)
        vars(fcfg).update(saved_f)
        fcfg._LOOKUP_RANK.clear()
        fcfg._LOOKUP_RANK.update(saved_rank)

    for tool, got in seen.items():
        assert got == want, (tool, {hex(k): hex(v) for k, v in got.items()})
