import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class StringCompareLifterTest(unittest.TestCase):
    def test_repne_scasb_scans_until_equal(self):
        scan = Instruction(
            0, 2, "repne scasb", "al, byte ptr es:[edi]", "f2ae")

        lifted = Lifter().lift_instruction(scan)
        generated = "\n".join(lifted)

        self.assertIn("while (ecx != 0)", generated)
        self.assertIn("_x = LO8(eax), _y = MEM8(edi); _flags = (_x == _y);", generated)
        self.assertIn("edi += _st; ecx--;", generated)
        self.assertIn("if (_flags) break;", generated)
        self.assertLess(
            generated.index("edi += _st; ecx--;"),
            generated.index("if (_flags) break;"),
        )

    def test_repe_cmpsb_compares_bytes_and_feeds_equal_jump(self):
        compare = Instruction(
            0, 2, "repe cmpsb", "byte ptr [esi], byte ptr es:[edi]",
            "f3a6")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=1),
            Operand(type="mem", mem_base="edi", mem_size=1),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertIn("_x = MEM8(esi), _y = MEM8(edi); _flags = (_x == _y);", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        self.assertIn("if (!_flags) break;", generated)
        # The je reads the last comparison's snapshot (XBA-317: the loop
        # writes _fa/_fb for every element, as a cmp would).
        self.assertIn("if (CMP_EQ(_fa, _fb)) goto loc_00000010;", generated)

    def test_dword_compare_sets_the_flags_its_jcc_reads(self):
        compare = Instruction(
            0, 2, "repe cmpsd", "dword ptr [esi], dword ptr es:[edi]",
            "f3a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=4),
            Operand(type="mem", mem_base="edi", mem_size=4),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        # Was a bare comment, so the jcc after it read whatever the previous
        # instruction had left in the flags. It compares four bytes at a time
        # and steps esi/edi by four, and the following je reads _flags.
        self.assertIn("_x = MEM32(esi), _y = MEM32(edi); _flags = (_x == _y);", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        # The step is four bytes, in whichever direction EFLAGS.DF says.
        self.assertIn("RECOMP_DF_STEP(4)", generated)
        self.assertIn("if (!_flags) break;", generated)
        # The je reads the flags the loop set: since XBA-317 every element's
        # comparison is snapshotted into _fa/_fb, as a cmp's operands are, so
        # the snapshot is the last comparison's, never an earlier one's.
        jcc = generated.splitlines()[-1]
        self.assertIn("CMP_EQ(_fa, _fb)", jcc)
        self.assertIn("_fa = _x; _fb = _y;", generated)


if __name__ == "__main__":
    unittest.main()
