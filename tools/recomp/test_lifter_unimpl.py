"""An instruction the lifter cannot translate must say so at runtime.

Until 19 Sep 2026 every path out of the lifter for an instruction it had no
translation for ended in a bare `/* TODO: mnemonic ops */` comment, which the
C compiler reads as nothing: the instruction vanished and the guest carried on.
Eleven such sites, three of which recorded the mnemonic in the translator's
end-of-run tally and eight of which did not. One title's gen tree carries 122 of
these comments and nothing could say whether any of them was ever reached.

The contract now is one helper, `Lifter._unimplemented`, and this pins its
three properties:

  1. the emitted C calls RECOMP_UNIMPL with the instruction text and its
     guest address, and keeps the TODO comment beside it for grep;
  2. the site is recorded in `lifter.unimplemented`, whichever path emitted it;
  3. instructions the lifter deliberately ignores -- the cache hints -- do
     NOT carry the marker, and neither does anything it translates. The
     ignored ones are decisions, written into the generated C as such, and a
     marker on them would make the runtime report count decisions as
     omissions.

The third is the negative control: without it a change that put RECOMP_UNIMPL
on every unhandled mnemonic including the deliberate ones would pass the first
two and flood the [UNIMPL] log with `prefetch`.
"""
import contextlib
import io
import unittest

from .__main__ import _report_unimplemented
from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, op_str="", operands=(), address=0x00012340):
    lifter = Lifter()
    insn = Instruction(address, 3, mnemonic, op_str, "",
                       operands=list(operands))
    return lifter, " ".join(lifter.lift_instruction(insn))


def _reg(name):
    return Operand(type="reg", reg=name)


class UnimplementedMarkerTest(unittest.TestCase):

    def test_unknown_mnemonic_carries_the_runtime_marker(self):
        # `daa` has no translation and no operands, so nothing about operand
        # formatting can interfere with what is being tested.
        lifter, out = _lift("daa")
        self.assertIn('RECOMP_UNIMPL("daa", 0x00012340u);', out)
        self.assertIn("/* TODO: daa */", out)
        self.assertEqual(lifter.unimplemented, {"daa": [0x00012340]})

    def test_hlt_waits_rather_than_being_ignored(self):
        # hlt was deliberately kept out of the ignored set, because a no-op
        # there turns a wait into a spin. It now has a translation that
        # waits (XBA-111): the runtime's xbox_Halt, named with its address.
        lifter, out = _lift("hlt", address=0x00ABCDEF)
        self.assertIn("xbox_Halt(0x00ABCDEFu);", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("(void)0", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_mmx_path_records_and_marks(self):
        # Before the helper, the MMX paths returned the comment WITHOUT
        # recording the site, so the translator's tally undercounted. A
        # binary MMX op with a destination that is not an mm register takes
        # the "(dst not mm)" exit.
        lifter, out = _lift("paddw", "eax, mm1", [_reg("eax"), _reg("mm1")])
        self.assertIn('RECOMP_UNIMPL("paddw eax, mm1", 0x00012340u);', out)
        self.assertIn("(dst not mm)", out)
        self.assertEqual(lifter.unimplemented, {"paddw": [0x00012340]})

    def test_ignored_hint_has_no_marker(self):
        # Negative control: a cache hint is a decision, not an omission.
        lifter, out = _lift("sfence")
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("TODO", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_translated_instruction_has_no_marker(self):
        # Negative control: an ordinary translated instruction carries neither
        # the marker nor the comment, and records nothing.
        lifter, out = _lift("mov", "eax, ebx", [_reg("eax"), _reg("ebx")])
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("TODO", out)
        self.assertEqual(lifter.unimplemented, {})

    def test_writeback_cache_flush_uses_coherent_host_memory(self):
        lifter, out = _lift("wbinvd")
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertIn("coherent host memory", out)
        self.assertEqual(lifter.unimplemented, {})


class UnimplementedReportTest(unittest.TestCase):

    def test_summary_describes_runtime_calls(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            _report_unimplemented({
                "unimplemented": {
                    "daa": [0x12340, 0x12350],
                    "hlt": [0xABCDEF],
                },
            })

        report = stderr.getvalue()
        self.assertIn(
            "3 instruction(s) unimplemented, emitted as RECOMP_UNIMPL "
            "runtime calls (2 distinct mnemonics):",
            report,
        )
        self.assertNotIn("no-op comments", report)
        self.assertIn("2  daa", report)
        self.assertIn("1  hlt", report)

    def test_empty_summary_is_silent(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            _report_unimplemented({})
        self.assertEqual(stderr.getvalue(), "")


class LiftHealthTest(unittest.TestCase):
    """The classes of code a title cannot reach, counted (XBA-139)."""

    STATS = {
        "seh_lost": {0x11010: [0x110A1, 0x110B0]},
        "unresolved_calls": {0x20000: (0x11234, 0x11200), 0x20010: None},
        "unimplemented": {"daa": [0x12340, 0x12350]},
        "misdecoded": {0x13000: ("outsb dx, byte ptr [esi]", 0x12F00)},
        "cut_bodies": {},
    }

    def _stderr(self, fn, stats):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            fn(stats)
        return stderr.getvalue()

    def test_counts(self):
        from .__main__ import lift_health
        self.assertEqual(lift_health(self.STATS), {
            "seh_lost": 2, "unresolved_calls": 2, "unimplemented": 2,
            "misdecoded": 1, "cut_bodies": 0})
        self.assertEqual(set(lift_health({}).values()), {0})

    def test_the_health_line_is_what_prepare_reads(self):
        import json
        from tools.prepare import __main__ as prepare
        from .__main__ import LIFT_HEALTH_PREFIX, _report_health, lift_health
        self.assertEqual(LIFT_HEALTH_PREFIX, prepare.LIFT_HEALTH_PREFIX)
        self.assertEqual(tuple(lift_health({})), prepare.LIFT_HEALTH_KEYS)
        line = self._stderr(_report_health, self.STATS).strip()
        self.assertTrue(line.startswith(LIFT_HEALTH_PREFIX))
        self.assertEqual(json.loads(line[len(LIFT_HEALTH_PREFIX):]),
                         lift_health(self.STATS))

    def test_unresolved_targets_are_named_with_their_caller(self):
        from .__main__ import _report_unresolved
        out = self._stderr(_report_unresolved, self.STATS)
        self.assertIn("unresolved call targets (stubbed): 2: 0x00020000 (call at "
                      "0x00011234 in sub_00011200), 0x00020010", out)
        self.assertEqual(self._stderr(_report_unresolved, {}), "")

    def test_misdecoded_sites_are_named_with_their_function(self):
        from .__main__ import _report_misdecoded
        out = self._stderr(_report_misdecoded, self.STATS)
        self.assertIn("`outsb dx, byte ptr [esi]` at 0x00013000 in sub_00012F00", out)


if __name__ == "__main__":
    unittest.main()
