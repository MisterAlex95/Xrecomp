"""pytest for the 3DS spike's recompiler (XBA-228): the 3DSX loader and its relocations, the ARMv6K + VFPv2
additions to the DS spike's discovery and lifter. Programs are built here from opcodes (encodings checked against
devkitARM's objdump), no toolchain; the lifted code's behaviour on real code is checked by consoles/3ds/tests
(ctr_rc_hello)."""

import json
import os
import shutil
import struct
import subprocess

import pytest

from ctr_tools.lift import CtrDiscovery, CtrLifter, arm_v6_class, media_known
from ctr_tools.recomp import main as recomp_main
from ctr_tools.threedsx import BASE, Image, ThreeDsxError, fnv1a

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime")


def runs(offsets):
    """Relocation runs ({skip, patch} in words) for sorted word offsets of a segment."""
    out, pos, i = [], 0, 0
    words = sorted(o // 4 for o in offsets)
    while i < len(words):
        j = i
        while j + 1 < len(words) and words[j + 1] == words[j] + 1:
            j += 1
        out.append((words[i] - pos, j - i + 1))
        pos = words[j] + 1
        i = j + 1
    return out


def make_3dsx(code, rodata=b"", data=b"", bss=0, absolute=((), (), ()), relative=((), (), ())):
    """A 3DSX: segments as given (data without its bss), relocation runs for word offsets per segment."""
    tables = [(runs(absolute[s]), runs(relative[s])) for s in range(3)]
    out = bytearray(b"3DSX")
    out += struct.pack("<HHII", 0x20, 8, 0, 0)
    out += struct.pack("<IIII", len(code), len(rodata), len(data) + bss, bss)
    for (a, r) in tables:
        out += struct.pack("<II", len(a), len(r))
    out += code + rodata + data
    for (a, r) in tables:
        for (skip, patch) in a + r:
            out += struct.pack("<HH", skip, patch)
    return bytes(out)


def words(*ws):
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)


def test_relocations():
    # code: b . ; .word 4 (absolute: 0x100004) ; .word 0x1000 (absolute: rodata) ; nop
    code = words(0xEAFFFFFE, 0x4, 0x1000, 0xE320F000)
    # rodata: .word 0 (relative to itself, 32-bit) ; .word 0x2004 (relative, 31-bit kind 1)
    rodata = words(0x0, 0x10002004)
    data = words(0x8)  # absolute: a code pointer
    raw = make_3dsx(code, rodata, data, bss=0x10, absolute=([4, 8], [], [0]), relative=([], [0, 4], []))
    img = Image(raw)
    assert img.u32(BASE + 4) == BASE + 4 and img.u32(BASE + 8) == BASE + 0x1000
    ro = img.data_segments[0]
    assert ro.vma == BASE + 0x1000
    assert struct.unpack_from("<I", ro.data, 0)[0] == (BASE - (BASE + 0x1000)) & 0xFFFFFFFF
    assert struct.unpack_from("<I", ro.data, 4)[0] == ((BASE + 0x2004) - (BASE + 0x1004)) & 0x7FFFFFFF
    dat = img.data_segments[1]
    assert dat.vma == BASE + 0x2000 and len(dat.data) == 0x1000  # page-aligned, bss included
    assert struct.unpack_from("<I", dat.data, 0)[0] == BASE + 8
    # pointers into code: the literal at +4 and the data word (rodata's relative words are not absolute)
    assert sorted((w, t) for (w, t, _th) in img.code_pointers()) == [(BASE + 4, BASE + 4), (BASE + 0x2000, BASE + 8)]
    assert img.contains(BASE) and not img.contains(BASE + 0x1000)  # code only
    lo, hi, h = img.image_hash()
    assert (lo, hi) == (BASE, BASE + 16) and h == fnv1a(img.code.data)


def test_bad_3dsx():
    with pytest.raises(ThreeDsxError):
        Image(b"3DSY" + b"\0" * 60)
    raw = bytearray(make_3dsx(words(0xE12FFF1E), absolute=([0], [], [])))
    struct.pack_into("<I", raw, 0x2C + 4, 0x30000000)  # the relocated word names an address past the image
    with pytest.raises(ThreeDsxError):
        Image(bytes(raw))


def test_v6_classes():
    assert arm_v6_class(0xE6BF5F35) == "media" and media_known(0xE6BF5F35)  # rev r5, r5
    assert arm_v6_class(0xE6EF4872) == "media"                              # uxtb r4, r2, ror #16
    assert arm_v6_class(0xE6525F9C) == "media" and arm_v6_class(0xE6844FBC) == "media"  # uadd8, sel
    assert arm_v6_class(0xE1956F9F) == "ldrex" and arm_v6_class(0xE1852F93) == "strex"
    assert arm_v6_class(0xE320F000) == "hint" and arm_v6_class(0xF57FF01F) == "clrex"
    assert arm_v6_class(0xEE324B05) == "vfp_dp" and arm_v6_class(0xEEF1FA10) == "vfp_mrc"
    assert arm_v6_class(0xEE072A10) == "vfp_mcr" and arm_v6_class(0xEC432B14) == "vfp_mcrr"
    assert arm_v6_class(0xED2D8B02) == "vfp_ls" and arm_v6_class(0xECB28B02) == "vfp_ls"
    assert arm_v6_class(0xEE1D3F70) == "mrc_tls" and arm_v6_class(0xEE070FBA) == "mcr_c7"
    assert arm_v6_class(0xEF000032) == "svc"
    assert arm_v6_class(0xE0810002) is None and arm_v6_class(0xE5921000) is None  # add, ldr: ARMv4T
    assert arm_v6_class(0xE7F000F0) == "udf"


def lift_one(img, entry):
    funcs = CtrDiscovery(img).run()
    f = next(f for f in funcs if f.entry == entry)
    return funcs, CtrLifter(img, False, f.addrs).function("f", 0, comments=False)


def test_v6_discovery_and_lift():
    func = BASE + 0x80
    ops = [
        0xE6BF5F35,  # rev r5, r5
        0xE6EF4872,  # uxtb r4, r2, ror #16
        0xE1956F9F,  # ldrex r6, [r5]
        0xE1852F93,  # strex r2, r3, [r5]
        0xED9F7B04,  # vldr d7, [pc, #16]
        0xEE324B05,  # vadd.f64 d4, d2, d5
        0xEEB47B45,  # vcmp.f64 d7, d5
        0xEEF1FA10,  # vmrs APSR_nzcv, fpscr
        0xEE1D3F70,  # mrc p15, 0, r3, c13, c0, 3
        0xEE070FBA,  # mcr p15, 0, r0, c7, c10, 5
        0xEF000032,  # svc 0x32
        0xE5921000,  # ldr r1, [r2]
        0xE320F000,  # nop
        0xEB000000 | ((func - (BASE + 13 * 4) - 8) >> 2),  # bl func
        0xE12FFF1E,  # bx lr
    ]
    code = bytearray(words(*ops))
    code += b"\0" * (0x80 - len(code))
    code += words(0xEE176A90, 0xE12FFF1E)  # func: vmov r6, s15 ; bx lr
    other = BASE + 0x90  # only a relocated pointer in data names it
    code += b"\0" * (0x90 - len(code)) + words(0xE3A00001, 0xE12FFF1E)  # mov r0, #1 ; bx lr
    img = Image(make_3dsx(bytes(code), data=words(0x90), absolute=([], [], [0])))
    funcs, c = lift_one(img, BASE)
    keys = {(f.entry, f.thumb) for f in funcs}
    assert {(BASE, False), (func, False), (other, False)} <= keys  # call after the SVC; the relocated pointer
    assert "r5 = rc_media(0xE6BF5F35u" in c
    assert "r4 = rc_media(0xE6EF4872u" in c
    assert "h->cpu.excl = 1;" in c and "r6 = RD32(ad_, 0, 0u);" in c
    assert "if (h->cpu.excl) {" in c and "r2 = 0;" in c and "r2 = 1;" in c
    assert "rc_vfp_ls(&h->cpu, mem, 0xED9F7B04u, 0x00100018u)" in c  # PC-relative: (pc + 8) & ~3
    assert "rc_vfp_dp(&h->cpu, 0xEE324B05u);" in c and "rc_vfp_dp(&h->cpu, 0xEEB47B45u);" in c
    assert "fN = f_ >> 31;" in c
    assert "r3 = h->cpu.tls;" in c
    assert "RC_INTERP(0x00100028u);" in c  # the SVC: the interpreter (HLE kernel)
    assert "uint32_t v_ = RD32(ad_, 0, 0u);" in c and "rc_ror(RD32" not in c  # unaligned: no rotation on ARMv6
    _, fc = lift_one(img, func)
    assert "r6 = rc_vfp_mrc(&h->cpu, 0xEE176A90u);" in fc


@pytest.mark.skipif(shutil.which("cc") is None, reason="no C compiler")
def test_cli_writes_c_that_compiles(tmp_path):
    code = words(
        0xE6BF5F35,  # rev r5, r5
        0xE1956F9F,  # ldrex r6, [r5]
        0xE1852F93,  # strex r2, r3, [r5]
        0xEE324B05,  # vadd.f64 d4, d2, d5
        0xEC432B14,  # vmov d4, r2, r3
        0xED2D8B02,  # vpush {d8}
        0xEEF1FA10,  # vmrs APSR_nzcv, fpscr
        0xEE1D3F70,  # mrc p15, 0, r3, c13, c0, 3
        0xEF000032,  # svc 0x32 (interpreter)
        0xE12FFF1E,  # bx lr
    )
    prog = tmp_path / "t.3dsx"
    prog.write_bytes(make_3dsx(code))
    out = tmp_path / "src"
    assert recomp_main([str(prog), str(out), "--report", str(tmp_path / "r.json")]) == 0
    r = json.loads((tmp_path / "r.json").read_text())
    assert r["functions"] == 1 and r["interp_fallback_insns"] == 1
    assert r["image_hash"]["lo"] == "0x00100000" and r["image_hash"]["hi"] == "0x00100028"
    files = sorted(p.name for p in out.iterdir())
    assert files == ["ctr_rc_00100000a.c", "ctr_rc_table.c"]
    for name in files:
        subprocess.run(["cc", "-fsyntax-only", "-Wall", "-Werror", "-Wno-unused-label", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-I", RUNTIME, str(out / name)], check=True)
