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


def test_lift_health_reads_the_counts_of_this_lift(tmp_path):
    """The counts of each class of unreachable code (XBA-139): the summary
    line, and the stamp a library check ranks titles by."""
    log = tmp_path / "prepare.log"
    log.write_text('%s {"seh_lost": 9}\n' % prepare.LIFT_HEALTH_PREFIX)
    at = os.path.getsize(log)
    assert prepare.lift_health(str(log), at) is None
    with open(log, "a") as f:
        f.write('12/12 functions\n%s {"seh_lost": 2, "unresolved_calls": 4, '
                '"unimplemented": 0, "misdecoded": 1, "cut_bodies": 0}\n'
                % prepare.LIFT_HEALTH_PREFIX)
    health = prepare.lift_health(str(log), at)
    assert health == {"seh_lost": 2, "unresolved_calls": 4, "unimplemented": 0,
                      "misdecoded": 1, "cut_bodies": 0}
    assert prepare.health_line(health) == (
        "2 exception handler targets no function enters, 4 unresolved call targets, "
        "1 misdecoded sites (string port I/O)")
    assert prepare.health_line(dict.fromkeys(prepare.LIFT_HEALTH_KEYS, 0)) is None
    assert prepare.health_line(None) is None


def test_a_garbled_health_line_is_no_health(tmp_path):
    log = tmp_path / "prepare.log"
    log.write_text("%s not json\n" % prepare.LIFT_HEALTH_PREFIX)
    assert prepare.lift_health(str(log), 0) is None
    assert prepare.lift_health(str(tmp_path / "missing.log"), 0) is None
