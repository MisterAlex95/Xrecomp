"""tools.seed_from_log reads a run's log and the runtime's discovery file (XBA-64)."""

import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.seed_from_log.__main__ import scan_log      # noqa: E402

DISCOVERY = """{"t":"run","time":"2026-11-11T18:00:00Z"}
{"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}
{"t":"thread","va":"0x00034560","ctx1":"0x00045670"}
{"t":"thread_ctx","va":"0x00045670","routine":"0x00034560"}
{"t":"unimpl","va":"0x00056780","insn":"bsf eax, ecx"}
{"t":"icall","va":"0x0012AB
"""


def test_discovery_file():
    got = scan_log(DISCOVERY)
    assert sorted(got) == [0x00034560, 0x00045670, 0x0012ABCD]     # no unimpl, no torn last line
    assert got[0x00034560].startswith("Thread start routine")


def test_run_log_still_read():
    log = ("[ICALL] Failed to resolve VA 0x00012345 (total calls: 9)\n"
           "  [KERNEL] PsCreateSystemThreadEx: start routine 0x00023456 not found in dispatch!\n")
    assert sorted(scan_log(log)) == [0x00012345, 0x00023456]
