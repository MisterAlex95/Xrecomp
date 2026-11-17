"""bts/btr/btc report the bit they found, not the bit they left.

All four bit-test instructions copy the tested bit into CF. Only `bt` leaves
the bit alone; `bts` sets it, `btr` clears it and `btc` flips it. The lifter
answered a following carry condition by reading the bit a second time, at the
consumer -- which for the three modifying forms reads back whatever they just
wrote:

    bts -> the bit is now 1, so "jb" was ALWAYS taken
    btr -> the bit is now 0, so "jb" was NEVER taken
    btc -> the bit is inverted, so the branch went exactly the wrong way

That is the test-and-set idiom -- "did I claim this, or was it already taken?"
-- reading its own answer. MSVC emits it for lock acquisition and for the
character-map loops behind strpbrk/strspn/strcspn, where `bts [esp], eax`
populates a 256-bit set and the carry says whether the character was already
present. Against the old code every character in such a set reported "already
present" on the first insertion.

The lift already captured the pre-write value into _cf. Two things stopped it
being used: the consumer preferred its own re-read, and _function_needs_cf did
not count the bt family as CF producers -- CF_TRACKED is only add/sub/adc/sbb/
shl/shr/sar -- so for a function whose sole carry consumer followed a `bts`,
the capture was never even emitted. Both halves are fixed; this pins both.

Plain `bt` kept the re-read for a while, on the grounds that it was exact. It
is exact only until something between the bt and its consumer writes the bit
base, the offset or an address register: `bt eax, 3; mov eax, ebx; jc` tested
ebx's bit. All four forms now answer from _cf; the compiled arm below runs
that sequence's real bytes at -O2.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import FunctionTranslator


def _reg_block(mnemonic, jcc, needs_cf=True):
    bit = Instruction(0, 3, mnemonic, "edx, eax", "0fabc2")
    bit.operands = [Operand(type="reg", reg="edx"),
                    Operand(type="reg", reg="eax")]
    branch = Instruction(3, 2, jcc, "0x20", "7219")
    branch.operands = []
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    lifted, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[bit, branch]))
    return "\n".join(lifted)


def _insns(mnemonic, jcc):
    bit = Instruction(0, 3, mnemonic, "edx, eax", "0fabc2")
    bit.operands = [Operand(type="reg", reg="edx"),
                    Operand(type="reg", reg="eax")]
    branch = Instruction(3, 2, jcc, "0x20", "7219")
    branch.operands = []
    return [bit, branch]


class BitTestCarryTest(unittest.TestCase):
    def test_modifying_forms_answer_from_the_snapshot(self):
        # The condition must be _cf, and the capture must precede the write.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                generated = _reg_block(mnemonic, "jb")
                self.assertIn("if (_cf)", generated)
                capture = generated.index("_cf = ")
                write = generated.index("edx = (edx")
                self.assertLess(capture, write,
                                "CF must be captured before the bit is written")

    def test_modifying_forms_do_not_reread_the_bit(self):
        # The exact defect: the condition re-reading the operand it just wrote.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                generated = _reg_block(mnemonic, "jb")
                condition = generated.splitlines()[-1]
                self.assertNotIn(">>", condition)

    def test_inverted_condition_negates_the_snapshot(self):
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                self.assertIn("if (!_cf)", _reg_block(mnemonic, "jae"))

    def test_plain_bt_answers_from_the_snapshot_too(self):
        generated = _reg_block("bt", "jb")
        self.assertIn("_cf = ", generated)
        self.assertIn("if (_cf)", generated.splitlines()[-1])
        self.assertIn("if (!_cf)", _reg_block("bt", "jae").splitlines()[-1])

    def test_bit_string_form_also_answers_from_the_snapshot(self):
        # bts with a memory base and a register offset is the strpbrk shape.
        dst = Operand(type="mem", mem_base="esp", mem_index=None,
                      mem_scale=1, mem_disp=0, mem_size=4)
        bit = Instruction(0, 4, "bts", "dword ptr [esp], eax", "0fab0424")
        bit.operands = [dst, Operand(type="reg", reg="eax")]
        branch = Instruction(4, 2, "jb", "0x20", "7219")
        branch.operands = []
        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[bit, branch]))
        generated = "\n".join(lifted)
        self.assertIn("if (_cf)", generated)
        self.assertLess(generated.index("_cf = "), generated.index("= (MEM32"))

    def test_the_snapshot_is_actually_emitted_for_these_functions(self):
        # The other half of the bug. Without this, the consumer would name a
        # _cf the translator never published.
        for mnemonic in ("bts", "btr", "btc"):
            with self.subTest(mnemonic=mnemonic):
                self.assertTrue(
                    FunctionTranslator._function_needs_cf(
                        _insns(mnemonic, "jb")),
                    "a carry branch after %s must publish _cf" % mnemonic)

    def test_plain_bt_publishes_cf_for_a_carry_test(self):
        self.assertTrue(
            FunctionTranslator._function_needs_cf(_insns("bt", "jb")))

    def test_a_carry_test_placed_before_its_bt_still_declares_cf(self):
        # The scan is in address order; a loop can put the bt below the jc.
        jc, bt = _insns("bt", "jb")[::-1]
        self.assertTrue(FunctionTranslator._function_needs_cf([jc, bt]))

    def test_bt_without_a_carry_test_publishes_nothing(self):
        ret = Instruction(3, 1, "ret", "", "c3")
        ret.operands = []
        self.assertFalse(
            FunctionTranslator._function_needs_cf([_insns("bt", "jb")[0], ret]))


# bt eax, 3 ; mov eax, ebx ; jc L ; mov eax, 0 ; ret ; L: mov eax, 1 ; ret
BT_THEN_OVERWRITE = "0fbae003 89d8 7206 b800000000 c3 b801000000 c3"
# bt [esi], ecx ; mov esi, edi ; jc L ; (same tail) -- the address moves
BT_MEM_THEN_MOVE = "0fa30e 89fe 7206 b800000000 c3 b801000000 c3"
# jmp B ; A: jc T ; mov eax, 0 ; ret ; B: bt eax, 3 ; jmp A ; T: mov eax, 1 ; ret
BT_BELOW_ITS_JC = "eb08 720c b800000000 c3 0fbae003 ebf2 b801000000 c3"


class BitTestCarryCompiledTest(unittest.TestCase):
    """Real bytes, translated as a function, built at -O2, run."""

    def setUp(self):
        from .lift_fixture import gnu_cc
        if not gnu_cc():
            self.skipTest("no GNU C compiler available")

    def _check(self, hexcode, rows):
        from .lift_fixture import build_and_run, translate
        code = translate(hexcode)
        self.assertIn("_cf", code)
        body = ["    int bad = 0;"]
        for setup, want in rows:
            body.append("    guest_reset(); %s lifted();" % setup)
            body.append('    if (eax != %du) { printf("%s -> %%u\\n", eax); bad++; }'
                        % (want, setup.replace('"', "")))
        body.append("    return bad != 0;")
        rc, out = build_and_run([code], "\n".join(body))
        self.assertEqual(rc, 0, out + code)

    def test_the_bit_of_the_tested_value_not_of_its_overwriter(self):
        self._check(BT_THEN_OVERWRITE, [
            ("eax = 8; ebx = 0;", 1),      # bit 3 of eax set, of ebx clear
            ("eax = 0; ebx = 8;", 0),
            ("eax = 0xFFFFFFF7u; ebx = 0xFFFFFFFFu;", 0),
        ])

    def test_the_bit_string_at_the_address_before_it_moved(self):
        self._check(BT_MEM_THEN_MOVE, [
            ("esi = 0x1000; edi = 0x2000; ecx = 37;"
             " g_ram[0x1004] = 0x20;", 1),  # bit 37 = dword 1, bit 5
            ("esi = 0x1000; edi = 0x2000; ecx = 37;"
             " g_ram[0x2004] = 0x20;", 0),
        ])

    def test_a_jc_above_its_bt(self):
        self._check(BT_BELOW_ITS_JC, [("eax = 8;", 1), ("eax = 7;", 0)])


if __name__ == "__main__":
    unittest.main()
