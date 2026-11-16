"""The PS2 static recompiler, spike (XBA-226): EE (R5900) ELF loading, the PS1's MIPS discovery and lifter extended
to the R5900 (64-bit GPRs, the 128-bit MMI, LQ / SQ, the non-IEEE FPU). CLI:
`python3 -m ps2_tools.recomp ELF OUTDIR [--discovered FILE]`.

The discovery and the lifter's shape (blocks, event checks, interpreter hand-offs) are ps1_tools'
(consoles/ps1/tools), subclassed: this package puts that directory on sys.path, so both run from a checkout without
installing anything. ps1_tools itself is unchanged.
"""

import os
import sys

CONSOLE = "ps2"

_PS1_TOOLS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "ps1",
                                           "tools"))
if _PS1_TOOLS not in sys.path:
    sys.path.insert(0, _PS1_TOOLS)
