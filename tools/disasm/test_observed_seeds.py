"""The mid-instruction guard and observed seeds.

The guard rejects a seed that lands inside an instruction of a detected body
unless the seed decodes as a prologue. It was written for RTTI vtable slots,
which are inferences (HL2 had two bad ones in 12,288). A seed from
tools.seed_from_log used to bypass it as "where the CPU actually went"; it no
longer does (WP2): a wild indirect call reports garbage too, and Fable's
0x001C120A, two bytes into a `mov`, cut sub_001C1190 before its `ret 8`. A
seed in bytes no body claims (Halo's XPP init at 0x001CF6AC, `cmp [flag], 0`
right after a pointer table the sweep had walked as code) never reaches the
guard: see test_cut_seed.py.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.__main__ import _load_seed_functions  # noqa: E402
from tools.disasm.disasm import Disassembler  # noqa: E402


class _Engine:
    def __init__(self, prologues):
        self._prologues = set(prologues)

    def probes_as_prologue(self, addr):
        return addr in self._prologues


def _disasm(observed, prologues):
    d = Disassembler.__new__(Disassembler)
    d.observed_seeds = set(observed)
    d.engine = _Engine(prologues)
    return d


class ObservedSeedTest(unittest.TestCase):
    def test_observed_seed_is_not_trusted_without_a_prologue(self):
        # Fable's wild-call seed, two bytes into a mov of sub_001C1190.
        d = _disasm(observed={0x001C120A}, prologues=())
        self.assertFalse(d._trust_mid_instruction_seed(0x001C120A))

    def test_unobserved_non_prologue_seed_is_still_rejected(self):
        # The HL2 RTTI case: six bytes into a mov, decodes as nothing.
        d = _disasm(observed=(), prologues=())
        self.assertFalse(d._trust_mid_instruction_seed(0x00202C2E))

    def test_prologue_seed_is_trusted(self):
        d = _disasm(observed=(), prologues={0x00069538})
        self.assertTrue(d._trust_mid_instruction_seed(0x00069538))

    def test_loader_marks_observed_seeds(self):
        seeds = [
            {"start": "0x001CF6AC", "observed": True, "note": "x"},
            # Written by seed_from_log before the field existed.
            {"start": "0x00015C5A", "note": "PsCreateSystemThreadEx "
             "StartContext1 observed at runtime; decodes as a function body."},
            {"start": "0x00202C2E", "note": "RTTI vtable slot"},
            0x00010000,
        ]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "seeds.json")
            with open(path, "w") as f:
                json.dump(seeds, f)
            observed = set()
            addrs = _load_seed_functions(path, observed)
        self.assertEqual(sorted(addrs),
                         [0x00010000, 0x00015C5A, 0x001CF6AC, 0x00202C2E])
        self.assertEqual(observed, {0x001CF6AC, 0x00015C5A})


if __name__ == "__main__":
    unittest.main()
