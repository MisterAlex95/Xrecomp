"""Tests of the XEX2 parser (XBA-230) on synthetic files from synth.py: no console data, no real key."""

import json
import os
import struct
import subprocess
import sys

import pytest

from tools.xex import aes
from tools.xex.synth import BASE, TEST_KEY, build_xex
from tools.xex.xex import (COMP_BASIC, COMP_NONE, COMP_NORMAL, H_FILE_FORMAT_INFO, IMPORT_THUNK, IMPORT_VARIABLE, XexError,
                           load_keys, parse_xex, summary)

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
IMPORTS = {"xboxkrnl.exe": [(0x5A, "thunk"), (0x1B, "variable"), (0x197, "thunk")], "xam.xex": [(0x200, "thunk")]}
CODE = [0x38600001, 0x4E800020, 0x38600002, 0x4E800020]  # li r3, 1; blr; li r3, 2; blr


def make(**kw):
    args = dict(imports=IMPORTS, functions=[(0, 8), (8, 8)], tls=(64, b"\1\2\3\4\5\6\7\x08"), entry=8,
                rdata=b"hello\0", data=b"\xAA" * 40)
    args.update(kw)
    return build_xex(CODE, **args)


def test_aes_known_answer_and_round_trip():
    a = aes.AES128(bytes(range(16)))
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    ct = a.encrypt_block(pt)
    assert ct.hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"  # FIPS-197 appendix C.1
    assert a.decrypt_block(ct) == pt
    data = bytes(range(256)) * 3
    assert aes.cbc_decrypt(TEST_KEY, aes.cbc_encrypt(TEST_KEY, data)) == data
    assert aes.ecb_decrypt(TEST_KEY, aes.ecb_encrypt(TEST_KEY, data)) == data


@pytest.mark.parametrize("compression", ["none", "basic", "normal"])
@pytest.mark.parametrize("encrypted", [False, True])
def test_round_trip(compression, encrypted):
    data, lay = make(compression=compression, key=TEST_KEY if encrypted else None)
    img = parse_xex(data, [bytes(16), TEST_KEY] if encrypted else [])
    assert img.image == lay.image
    assert img.key_used == (1 if encrypted else None)
    assert img.compression == {"basic": COMP_BASIC, "none": COMP_NONE, "normal": COMP_NORMAL}[compression]
    assert img.encryption == (1 if encrypted else 0)
    assert img.entry == lay.text + 8 and img.base == BASE and img.image_size == lay.image_size
    assert img.page_size == 0x1000 and len(img.pages) == lay.image_size // 0x1000
    assert img.pages[1].info == 1 and img.pages[-1].info == 2  # code page, then data


def test_headers_and_imports():
    data, lay = make(compression="basic", key=TEST_KEY)
    img = parse_xex(data, [TEST_KEY])
    assert [lib.name for lib in img.libraries] == ["xboxkrnl.exe", "xam.xex"]
    k = img.libraries[0]
    assert len(k.record_addrs) == 5  # two thunks (two records each) and a variable
    funcs = [(r.ordinal, r.kind) for r in k.records]
    assert funcs == [(0x5A, IMPORT_VARIABLE), (0x5A, IMPORT_THUNK), (0x1B, IMPORT_VARIABLE),
                     (0x197, IMPORT_VARIABLE), (0x197, IMPORT_THUNK)]
    assert {r.addr for r in k.records if r.kind == IMPORT_THUNK} == {lay.thunks[("xboxkrnl.exe", 0x5A)],
                                                                    lay.thunks[("xboxkrnl.exe", 0x197)]}
    assert img.libraries[1].records[1].lib_index == 1
    assert img.tls["slots"] == 64 and img.tls["data_size"] == 8
    assert img.read(img.tls["raw_data_address"], 8) == b"\1\2\3\4\5\6\7\x08"
    assert img.execution["title_id"] == 0x58425A30 and img.execution["version"] == "1.0.3.0"
    assert img.original_name == "synth.exe" and img.stack_size == 0x40000
    assert img.pdata == [(lay.text, 8), (lay.text + 8, 8)]
    names = [s.name for s in img.sections]
    assert names == [".text", ".rdata", ".data", ".pdata"]
    assert img.sections[0].code and not img.sections[1].code
    assert img.in_code(lay.text) and img.u32(lay.text) == CODE[0]
    assert not img.in_code(lay.rdata)
    assert img.read(lay.rdata, 6) == b"hello\0"
    assert img.pe_machine == 0x01F2
    s = summary(img)
    assert s["libraries"][0]["version"] == "2.0.1888.0" and s["pdata_functions"] == 2
    json.dumps(s)


def test_encrypted_without_or_with_wrong_key():
    data, _ = make(compression="basic", key=TEST_KEY)
    img = parse_xex(data)
    assert img.image is None and "key" in img.image_note
    assert img.libraries and img.libraries[0].records == []  # the records live in the encrypted image
    assert img.entry and img.tls
    img = parse_xex(data, [bytes(16)])
    assert img.image is None and "none of the keys" in img.image_note


def _patch_compression(data, comp):
    count = struct.unpack_from(">I", data, 0x14)[0]
    for k in range(count):
        key, value = struct.unpack_from(">II", data, 0x18 + 8 * k)
        if key == H_FILE_FORMAT_INFO:
            out = bytearray(data)
            struct.pack_into(">H", out, value + 6, comp)
            return bytes(out)
    raise AssertionError("no file format header")


def test_lzx_errors_are_reported():
    data, lay = make(compression="normal")
    # a damaged compressed block: its SHA-1 no longer matches
    bad = bytearray(data)
    bad[-100] ^= 0xFF
    img = parse_xex(bytes(bad))
    assert img.image is None and "SHA-1" in img.image_note
    assert img.libraries and img.entry  # the headers are still there
    # "normal" announced over a basic image: no block chain to walk
    data, _ = make()
    img = parse_xex(_patch_compression(data, 2))
    assert img.image is None and "normal (LZX)" in img.image_note
    # a wrong key on an encrypted LZX image fails fast and cleanly
    data, _ = make(compression="normal", key=TEST_KEY)
    img = parse_xex(data, [bytes(16)])
    assert img.image is None and "none of the keys" in img.image_note


def test_bad_files():
    with pytest.raises(XexError):
        parse_xex(b"XEX1" + bytes(64))
    with pytest.raises(XexError):
        parse_xex(b"XEX2" + bytes(8))
    data, _ = make()
    with pytest.raises(XexError):
        parse_xex(data[:0x30])


def test_load_keys(tmp_path, monkeypatch):
    raw = tmp_path / "k.bin"
    raw.write_bytes(TEST_KEY)
    hexf = tmp_path / "k.txt"
    hexf.write_text(TEST_KEY.hex().upper() + "  # comment\n")
    bad = tmp_path / "bad.txt"
    bad.write_text("1234\n")
    monkeypatch.delenv("XEX_KEY_FILE", raising=False)
    assert load_keys([str(raw), str(hexf)]) == [TEST_KEY, TEST_KEY]
    with pytest.raises(XexError):
        load_keys([str(bad)])
    monkeypatch.setenv("XEX_KEY_FILE", str(hexf))
    assert load_keys([]) == [TEST_KEY]


def test_cli(tmp_path):
    data, lay = make(compression="basic", key=TEST_KEY)
    xex = tmp_path / "t.xex"
    xex.write_bytes(data)
    key = tmp_path / "key"
    key.write_bytes(TEST_KEY)
    env = dict(os.environ, PYTHONPATH=REPO)
    env.pop("XEX_KEY_FILE", None)
    r = subprocess.run([sys.executable, "-m", "tools.xex", str(xex), "--key-file", str(key), "--json",
                        str(tmp_path / "s.json"), "--image", str(tmp_path / "img.bin")], env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert "import xboxkrnl.exe 2.0.1888.0: 5 records (2 thunks)" in r.stdout
    assert ".pdata: 2 functions" in r.stdout
    assert (tmp_path / "img.bin").read_bytes() == lay.image
    assert json.loads((tmp_path / "s.json").read_text())["entry"] == lay.text + 8
    r = subprocess.run([sys.executable, "-m", "tools.xex", str(xex)], env=env, capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0 and "image not available" in r.stdout
