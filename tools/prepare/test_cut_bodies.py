"""Prepare reports the bodies the lift ended with a recomp_cut_body trap (WP2)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.prepare import __main__ as prepare            # noqa: E402
from tools.recomp import __main__ as recomp              # noqa: E402


def test_prefix_matches_what_the_lift_prints():
    assert prepare.CUT_BODIES_PREFIX == recomp.CUT_BODIES_PREFIX


def test_reads_the_line_of_this_lift_only(tmp_path):
    log = tmp_path / "prepare.log"
    log.write_text("%s 9: sub_00010000 (last 0x00010003)\n" % prepare.CUT_BODIES_PREFIX)
    at = os.path.getsize(log)
    assert prepare.lift_cut_bodies(str(log), at) is None
    with open(log, "a") as f:
        f.write("\n$ python -m tools.recomp ...\n12/12 functions\n"
                "%s 1: sub_001C1190 (last 0x001C1208)\n" % prepare.CUT_BODIES_PREFIX)
    assert prepare.lift_cut_bodies(str(log), at) == "1: sub_001C1190 (last 0x001C1208)"
    assert prepare.lift_cut_bodies(str(tmp_path / "missing.log"), 0) is None
