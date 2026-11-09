"""`mov` to and from control and debug registers reaches the runtime.

Formatted as ordinary register operands, `mov eax, cr3` / `mov cr3, eax` (the
XDK's TLB flush after a page-table write) came out as `eax = cr3; cr3 = eax;`.
No `cr3` exists in the generated C, so the whole chunk failed to compile on
the first title that links those XDK helpers (Fable: The Lost Chapters).
"""
import pathlib
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str, operands):
    lifter = Lifter()
    insn = Instruction(0x000E0970, 3, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


class ControlRegisterTest(unittest.TestCase):

    def test_read_cr3(self):
        _, out = _lift("mov", "eax, cr3", [_reg("eax"), _reg("cr3")])
        self.assertIn("eax = xbox_ReadCr(3);", out)
        self.assertNotIn(" cr3", out.replace("xbox_ReadCr", ""))

    def test_write_cr3(self):
        _, out = _lift("mov", "cr3, eax", [_reg("cr3"), _reg("eax")])
        self.assertIn("xbox_WriteCr(3, eax);", out)

    def test_cr0_and_cr4(self):
        _, out = _lift("mov", "ecx, cr0", [_reg("ecx"), _reg("cr0")])
        self.assertIn("ecx = xbox_ReadCr(0);", out)
        _, out = _lift("mov", "cr4, edx", [_reg("cr4"), _reg("edx")])
        self.assertIn("xbox_WriteCr(4, edx);", out)

    def test_debug_registers(self):
        _, out = _lift("mov", "eax, dr7", [_reg("eax"), _reg("dr7")])
        self.assertIn("eax = xbox_ReadDr(7);", out)
        _, out = _lift("mov", "dr0, esi", [_reg("dr0"), _reg("esi")])
        self.assertIn("xbox_WriteDr(0, esi);", out)

    def test_ordinary_mov_unchanged(self):
        _, out = _lift("mov", "eax, ecx", [_reg("eax"), _reg("ecx")])
        self.assertIn("eax = ecx;", out)
        self.assertNotIn("xbox_", out)

    def test_helpers_are_declared_for_generated_code(self):
        hdr = (pathlib.Path(__file__).resolve().parents[2] / "templates" /
               "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        for decl in ("uint32_t xbox_ReadCr(int n);",
                     "void     xbox_WriteCr(int n, uint32_t value);",
                     "uint32_t xbox_ReadDr(int n);",
                     "void     xbox_WriteDr(int n, uint32_t value);"):
            self.assertIn(decl, hdr)

    def test_helpers_are_defined_by_the_runtime(self):
        hal = (pathlib.Path(__file__).resolve().parents[2] / "src" /
               "kernel" / "kernel_hal.c").read_text(encoding="utf-8")
        for name in ("xbox_ReadCr", "xbox_WriteCr", "xbox_ReadDr",
                     "xbox_WriteDr"):
            self.assertRegex(hal, rf"\n\w+ {name}\(int n")


if __name__ == "__main__":
    unittest.main()
