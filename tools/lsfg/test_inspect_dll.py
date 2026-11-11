"""tools/lsfg/inspect_dll.py on synthetic PE files built here (no Lossless.dll)."""

import json
import struct

import pytest

from tools.lsfg import inspect_dll


def build_pe(resources):
    """A PE32+ with one .rsrc section holding RT_RCDATA entries {id: bytes}."""
    ids = sorted(resources)
    n = len(ids)
    name_dir = 16 + 8
    lang = name_dir + 16 + 8 * n
    data_e = lang + 24 * n
    blobs = data_e + 16 * n
    sec = bytearray(blobs)
    va, raw = 0x1000, 0x400
    struct.pack_into("<HH", sec, 12, 0, 1)
    struct.pack_into("<II", sec, 16, 10, 0x80000000 | name_dir)
    struct.pack_into("<HH", sec, name_dir + 12, 0, n)
    for i, rid in enumerate(ids):
        struct.pack_into("<II", sec, name_dir + 16 + 8 * i, rid, 0x80000000 | (lang + 24 * i))
        struct.pack_into("<HH", sec, lang + 24 * i + 12, 0, 1)
        struct.pack_into("<II", sec, lang + 24 * i + 16, 0x409, data_e + 16 * i)
        blob = len(sec)
        sec += resources[rid] + b"\0" * (-len(resources[rid]) % 4)
        struct.pack_into("<II", sec, data_e + 16 * i, va + blob, len(resources[rid]))
    b = bytearray(raw)
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, 0x80)
    b[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", b, 0x84, 0x8664, 1)
    optsz = 112 + 16 * 8
    struct.pack_into("<H", b, 0x84 + 16, optsz)
    opt = 0x84 + 20
    struct.pack_into("<H", b, opt, 0x20B)
    struct.pack_into("<I", b, opt + 108, 16)
    struct.pack_into("<II", b, opt + 112 + 16, va, len(sec))
    s = opt + optsz
    b[s:s + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", b, s + 8, len(sec), va, len(sec), raw)
    return bytes(b + sec)


SPV = struct.pack("<5I", 0x07230203, 0x10000, 0, 1, 0)
DXBC = b"DXBC" + b"\0" * 28


def test_summary_and_listing(tmp_path, capsys):
    res = {101: b"x", 255: DXBC, 256: DXBC, 280: DXBC, 304: SPV, 353: SPV}
    res.update({i: DXBC for i in range(257, 280)})
    p = tmp_path / "fake.dll"
    p.write_bytes(build_pe(res))
    assert inspect_dll.main([str(p), "--json"]) == 0
    d = json.loads(capsys.readouterr().out)
    assert d["rcdata"] == len(res)
    assert d["kinds"] == {"dxbc": 26, "spirv": 2, "other": 1}
    assert d["lsfg_3_1"] == {"needed": 25, "dxbc": 25, "spirv_fp16": 1, "spirv_fp32": 1}
    assert d["lsfg_3_1p"]["dxbc"] == 3
    assert 101 not in [r["id"] for r in d["resources"]]
    assert {"id": 304, "size": len(SPV), "kind": "spirv"} in d["resources"]
    assert inspect_dll.main([str(p), "--all"]) == 0
    out = capsys.readouterr().out
    assert "lsfg_3_1   25/25 DXBC" in out
    assert "   101         1  other" in out


def test_not_a_pe(tmp_path, capsys):
    p = tmp_path / "junk.dll"
    p.write_bytes(b"\0" * 100)
    assert inspect_dll.main([str(p)]) == 1
    assert "no MZ header" in capsys.readouterr().err


def test_truncated_fails_cleanly():
    pe = build_pe({255: DXBC})
    for k in range(0x40, len(pe) - 4, 7):
        with pytest.raises(inspect_dll.PeError):
            inspect_dll.rcdata(pe[:k])
