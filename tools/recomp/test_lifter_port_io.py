"""`in` and `out` reach the runtime instead of vanishing.

Lifted as no-ops, `in al, dx` left AL holding whatever it held before.
D3D's vblank ISR reads the video encoder's field pin that way (port 0x80C0,
bit 5), so the field it recorded never changed, D3DDevice_GetDisplayFieldStatus
said "odd" for ever, and the XMV player -- which starts its clock on an even
field -- never showed Burnout 3's first intro frame.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str, operands):
    lifter = Lifter()
    insn = Instruction(0x00356B4C, 1, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


class PortIoTest(unittest.TestCase):

    def test_in_al_dx_writes_only_al(self):
        lifter, out = _lift("in", "al, dx", [_reg("al"), _reg("dx")])
        self.assertIn("SET_LO8(eax, xbox_PortIn((edx & 0xFFFFu), 1));", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_in_eax_imm_port(self):
        _, out = _lift("in", "eax, 0x61", [_reg("eax"), _imm(0x61)])
        self.assertIn("xbox_PortIn(0x61u, 4)", out)

    def test_out_dx_al(self):
        lifter, out = _lift("out", "dx, al", [_reg("dx"), _reg("al")])
        self.assertIn("xbox_PortOut((edx & 0xFFFFu), (uint32_t)(LO8(eax)), 1);",
                      out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_helpers_are_declared_for_generated_code(self):
        import pathlib
        hdr = (pathlib.Path(__file__).resolve().parents[2] / "templates" /
               "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        self.assertIn("uint32_t xbox_PortIn(uint32_t port, int width);", hdr)
        self.assertIn("void     xbox_PortOut(", hdr)
        self.assertIn("void recomp_misdecoded(const char *text, uint32_t va);", hdr)


class StringPortIoTest(unittest.TestCase):
    """ins*/outs* in lifted code: taken for misdecoding (XBA-139). A title
    had one `outsb` in its prepare log, printed as unimplemented and lifted
    as a no-op the run would walk through."""

    def test_outsb_stops_at_run_time_and_is_counted_apart(self):
        lifter, out = _lift("outsb", "dx, byte ptr [esi]",
                            [_reg("dx"), Operand(type="mem", mem_base="esi", mem_size=1)])
        self.assertIn('recomp_misdecoded("outsb dx, byte ptr [esi]", 0x00356B4Cu);', out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})
        self.assertEqual(lifter.misdecoded, {0x00356B4C: ("outsb dx, byte ptr [esi]", 0)})

    def test_every_form_with_or_without_rep(self):
        for m in ("insb", "insw", "insd", "outsw", "outsd", "rep insw", "rep outsb"):
            lifter, out = _lift(m, "", [])
            self.assertIn("recomp_misdecoded(", out, m)
            self.assertEqual(len(lifter.misdecoded), 1, m)

    def test_an_unknown_rep_form_is_counted_not_dropped(self):
        """`rep lodsb` was a bare comment nothing counted."""
        lifter, out = _lift("rep lodsb", "al, byte ptr [esi]", [])
        self.assertIn("RECOMP_UNIMPL(", out)
        self.assertIn("rep lodsb", lifter.unimplemented)


class CallSiteTest(unittest.TestCase):
    def test_a_call_target_keeps_its_first_call_site(self):
        lifter = Lifter()
        lifter.func_start = 0x00011000
        for at in (0x00011010, 0x00011020):
            insn = Instruction(at, 5, "call", "0x12000", "",
                               operands=[_imm(0x00012000)])
            insn.call_target = 0x00012000
            lifter.lift_instruction(insn)
        self.assertEqual(lifter.call_sites[0x00012000], (0x00011010, 0x00011000))


if __name__ == "__main__":
    unittest.main()
