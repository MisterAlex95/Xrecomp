"""Bit scan lifting: value, the zero case, and the flag it publishes.

Originally contributed in #16 alongside a second implementation. The lifter
already had one, reached earlier in lift_instruction, and the two disagreed on
where ZF lives -- the contributed body wrote `_flags` while the flag path read
the `_fa`/`_fb` snapshot, so a jcc after a bit scan would have read a stale
value. These tests were rewritten against the surviving implementation, which
publishes ZF through the same snapshot a `cmp src, 0` leaves and so composes
with everything else that reads flags.
"""

from tools.recomp.disasm import BasicBlock, Instruction, Operand
from tools.recomp.lifter import Lifter, lift_basic_block


def _reg(name, size=4):
    op = Operand("reg")
    op.reg = name
    op.size = size
    return op


def _scan(mnemonic, dst="eax", src="ecx", size=4):
    insn = Instruction(0, 3, mnemonic, f"{dst}, {src}", "0fbcc1")
    insn.operands = [_reg(dst, size), _reg(src, size)]
    return insn


def test_bsf_scans_from_the_low_bit():
    body = "".join(Lifter().lift_instruction(_scan("bsf")))
    assert "eax = RECOMP_BSF32(_bs_v)" in body and "for (" not in body


def test_bsr_scans_from_the_high_bit():
    body = "".join(Lifter().lift_instruction(_scan("bsr")))
    assert "eax = RECOMP_BSR32(_bs_v)" in body and "for (" not in body


def test_scans_compiled_at_O2_against_the_runtime_header():
    """Real bytes, translated, built at -O2 against recomp_types.h: every
    single bit, a few mixed values, the 16-bit forms, and the zero source
    (destination kept, ZF set)."""
    import pytest
    pytest.importorskip("capstone")
    from tools.recomp.lift_fixture import build_and_run, gnu_cc, translate
    if not gnu_cc():
        pytest.skip("no GNU C compiler available")
    functions = [
        translate("0fbcc1 0f94c3 c3", name="bsf32"),     # bsf eax, ecx ; sete bl
        translate("0fbdc1 0f94c3 c3", name="bsr32"),     # bsr eax, ecx ; sete bl
        translate("660fbcc1 c3", name="bsf16"),          # bsf ax, cx
        translate("660fbdc1 c3", name="bsr16"),          # bsr ax, cx
    ]
    body = r"""
    int bad = 0; unsigned i;
    static volatile uint32_t mixed[] = {0x80000001u, 0x00F00F00u, 0xFFFFFFFFu, 0x00010000u};
    static const uint32_t lo[] = {0, 8, 0, 16}, hi[] = {31, 23, 31, 16};
    for (i = 0; i < 32; i++) {
        guest_reset(); ecx = 1u << i; eax = 99; ebx = 7; bsf32();
        if (eax != i || (ebx & 0xFF) != 0) { printf("bsf bit %u -> %u\n", i, eax); bad++; }
        guest_reset(); ecx = 1u << i; eax = 99; bsr32();
        if (eax != i) { printf("bsr bit %u -> %u\n", i, eax); bad++; }
    }
    for (i = 0; i < 4; i++) {
        guest_reset(); ecx = mixed[i]; bsf32();
        if (eax != lo[i]) { printf("bsf %08X -> %u\n", mixed[i], eax); bad++; }
        guest_reset(); ecx = mixed[i]; bsr32();
        if (eax != hi[i]) { printf("bsr %08X -> %u\n", mixed[i], eax); bad++; }
    }
    guest_reset(); ecx = 0; eax = 0x1234u; ebx = 0; bsf32();
    if (eax != 0x1234u || (ebx & 0xFF) != 1) { printf("bsf 0 -> %08X zf %u\n", eax, ebx & 0xFF); bad++; }
    guest_reset(); ecx = 0; eax = 0x1234u; ebx = 0; bsr32();
    if (eax != 0x1234u || (ebx & 0xFF) != 1) { printf("bsr 0 -> %08X zf %u\n", eax, ebx & 0xFF); bad++; }
    guest_reset(); ecx = 0xFFFF0300u; eax = 0xAAAA0000u; bsf16();
    if (eax != 0xAAAA0008u) { printf("bsf16 -> %08X\n", eax); bad++; }
    guest_reset(); ecx = 0xFFFF0300u; eax = 0xAAAA0000u; bsr16();
    if (eax != 0xAAAA0009u) { printf("bsr16 -> %08X\n", eax); bad++; }
    return bad != 0;"""
    rc, out = build_and_run(functions, body)
    assert rc == 0, out


def test_zero_source_leaves_the_destination_untouched():
    # The architecture leaves the destination undefined for a zero source, so
    # the lift must not invent a value -- the write sits behind the guard.
    body = "".join(Lifter().lift_instruction(_scan("bsf")))
    assert "if (_bs_v) {" in body


def test_zero_flag_is_published_through_the_compare_snapshot():
    body = "".join(Lifter().lift_instruction(_scan("bsf")))
    assert "_fa = _bs_v" in body and "_fb = 0" in body


def test_jnz_after_a_scan_reads_that_snapshot():
    scan = _scan("bsr")
    jump = Instruction(3, 2, "jne", "0x10", "750b")
    jump.jump_target = 0x10
    lifter = Lifter()
    lifter.func_start = 0
    lifter.func_end = 0x20
    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[scan, jump]))
    assert "_fa" in generated[-1] and "goto loc_00000010;" in generated[-1]
