"""tools.pe_parser on synthetic PEs from our own writer (tools/pe_parser/synth.py, XBA-233): sections, entry point,
imports by name and by ordinal, base relocations, the analysis JSON, and the pipeline's readers taking a PE where they
took an XBE. No third-party binary."""

import json
import os
import struct
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("pefile")

from tools.pe_parser.pe_parser import analysis, parse_pe              # noqa: E402
from tools.pe_parser.synth import BASE, HELLO_MESSAGE, build_pe, hello_pe  # noqa: E402

EXTRA = {"WS2_32.dll": [115, "select"]}


@pytest.fixture(scope="module")
def hello():
    return hello_pe(extra_imports=EXTRA)


def test_header_and_sections(hello):
    img = parse_pe(hello)
    assert (img.machine, img.subsystem, img.base_address) == (0x14C, 3, BASE)
    assert img.entry_point == BASE + 0x1000 and img.image_size == 0x6000
    assert [s.name for s in img.sections] == [".text", ".rdata", ".data", ".idata", ".reloc"]
    text, rdata, data = img.sections[:3]
    assert text.executable and not text.writable and text.flags == 0x2 | 0x4
    assert not rdata.executable and not rdata.writable and rdata.flags == 0x2
    assert data.writable and data.flags == 0x2 | 0x1
    # .data: 4 bytes of data and 0x100 of zero fill, all inside one file-aligned block (raw is capped at vsize)
    assert (data.virtual_size, data.raw_size) == (0x104, 0x104)
    assert img.read(BASE + 0x2000, len(HELLO_MESSAGE)) == HELLO_MESSAGE
    assert img.read(BASE + 0x3000 + 0x80, 4) == b"\0" * 4       # zero fill past the file bytes
    assert img.read(BASE, 2) == b"MZ"                            # the headers are mapped too
    assert img.read(BASE + 0x9000, 4) is None and img.warnings == []


def test_imports_by_name_and_ordinal(hello):
    img = parse_pe(hello)
    got = [(i.dll, i.name, i.ordinal) for i in img.imports]
    assert got == [("KERNEL32.dll", "ExitProcess", None), ("KERNEL32.dll", "GetStdHandle", None),
                   ("KERNEL32.dll", "WriteFile", None), ("WS2_32.dll", None, 115), ("WS2_32.dll", "select", None)]
    assert [i.hint for i in img.imports if i.name] == [1, 2, 3, 4]
    assert [i.label for i in img.imports][3] == "WS2_32_ord115"
    # The IAT slots: what `call [slot]` reads, inside .idata and covered by the IAT directory
    slots = [i.thunk_addr for i in img.imports]
    assert slots[:3] == [slots[0], slots[0] + 4, slots[0] + 8]
    assert all(img.section_at(s).name == ".idata" for s in slots)
    assert img.iat_addr == slots[0] and img.iat_addr + img.iat_size >= slots[-1] + 4
    # The code calls through exactly those slots
    code = img.read(img.entry_point, 0x3D)
    called = {struct.unpack_from("<I", code, i + 2)[0] for i in range(len(code) - 6) if code[i:i + 2] == b"\xff\x15"}
    assert called == set(slots[:3])


def test_base_relocations(hello):
    img = parse_pe(hello)
    # The six absolute words of hello_pe's code: three IAT slots, the message, &written twice
    assert len(img.relocations) == 6
    targets = sorted(struct.unpack("<I", img.read(r, 4))[0] for r in img.relocations)
    slots = sorted(i.thunk_addr for i in img.imports[:3])
    assert targets == sorted(slots + [BASE + 0x2000, BASE + 0x3000, BASE + 0x3000])
    assert all(img.section_at(r).name == ".text" for r in img.relocations)


def test_refuses_what_is_not_a_pe32_i386():
    with pytest.raises(ValueError, match="no MZ"):
        parse_pe(b"XBEH" + bytes(0x400))
    pe = bytearray(hello_pe())
    struct.pack_into("<H", pe, 0x44, 0x8664)                     # Machine: x86-64
    with pytest.raises(ValueError, match="not i386"):
        parse_pe(bytes(pe))


def test_analysis_json_is_what_disasm_loads(hello, tmp_path):
    exe = tmp_path / "hello.exe"
    exe.write_bytes(hello)
    js = tmp_path / "hello_analysis.json"
    r = subprocess.run([sys.executable, "-m", "tools.pe_parser", str(exe), "--json", str(js)], cwd=ROOT,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "WS2_32.dll!#115" in r.stdout
    data = json.loads(js.read_text())
    assert data == json.loads(json.dumps(analysis(parse_pe(str(exe)))))
    assert data["format"] == "pe" and data["entry_point"] == "0x00401000"
    assert [k["name"] for k in data["kernel_imports"]] == ["ExitProcess", "GetStdHandle", "WriteFile",
                                                          "WS2_32_ord115", "select"]
    # The XBE parser's CLI hands a PE to this front end
    r = subprocess.run([sys.executable, "-m", "tools.xbe_parser", str(exe), "--quiet"], cwd=ROOT,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    from tools.disasm.loader import load_image
    image = load_image(str(exe))                                 # finds hello_analysis.json beside it
    assert image.entry_point == BASE + 0x1000 and image.base_address == BASE
    assert [s.name for s in image.get_code_sections()] == [".text"]
    assert image.get_kernel_import_at_thunk(parse_pe(hello).imports[1].thunk_addr).name == "GetStdHandle"
    assert image.read_bytes_at_va(BASE + 0x2000, 5) == HELLO_MESSAGE[:5]


def test_pipeline_readers_take_a_pe(hello, tmp_path):
    exe = tmp_path / "hello.exe"
    exe.write_bytes(hello)
    from tools.xbe_parser.xbe_parser import open_image
    view = open_image(str(exe))
    assert view.header.entry_point == BASE + 0x1000 and view.header.kernel_thunk_addr == parse_pe(hello).iat_addr
    assert [s.name for s in view.sections][0] == ".text"

    from tools.abi_analysis.xbe_min import XbeFile
    xf = XbeFile(str(exe))
    assert xf.find_section(BASE + 0x1000).name == ".text"
    assert xf.read_bytes_at_va(BASE + 0x2000, 5) == HELLO_MESSAGE[:5]

    from tools.func_id.identify import _parse_xbe_sections
    secs = _parse_xbe_sections(hello)
    assert [(s["name"], s["executable"]) for s in secs][:2] == [(".text", True), (".rdata", False)]

    from tools.recomp import config as rconfig
    saved = (list(rconfig._SECTIONS), rconfig.ENTRY_POINT, rconfig.KERNEL_THUNK_ADDR, rconfig._configured_from)
    try:
        rconfig.configure_from_xbe(str(exe))
        assert (rconfig.TEXT_VA_START, rconfig.ENTRY_POINT) == (BASE + 0x1000, BASE + 0x1000)
    finally:
        rconfig._install(saved[0], saved[1], saved[2], saved[3])


def test_writer_layout_is_fixed():
    def code(layout):
        assert layout.iat["ExitProcess"] > layout.base and layout.text == BASE + 0x1000
        return b"\xc3", []
    img = parse_pe(build_pe(code, data=b"\x11" * 4, bss=0x800))
    assert [i.name for i in img.imports] == ["ExitProcess"] and img.relocations == []
    data = img.sections[2]
    # a zero fill longer than the file block: the loader maps 0x200 file bytes, the rest is zeros
    assert (data.virtual_size, data.raw_size) == (0x804, 0x200)
    assert img.read(data.virtual_addr, 8) == b"\x11" * 4 + b"\0" * 4
    assert img.read(data.virtual_addr + 0x7F0, 0x14) == b"\0" * 0x14
