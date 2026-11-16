"""The Nintendo DS static recompiler, spike (XBA-223): .nds header, the ARM9 binary as it is once loaded, the GBA
ARM / Thumb lifter extended to ARMv5TE. CLI: `python3 -m nds_tools.recomp ROM.nds OUTDIR [--discovered FILE]`.

The lifter and the discovery are gba_tools' (consoles/gba/tools), subclassed: this package puts that directory on
sys.path, so both run from a checkout without installing anything.
"""

import os
import sys

CONSOLE = "nds"

_GBA_TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                          "..", "gba", "tools")
_GBA_TOOLS = os.path.normpath(_GBA_TOOLS)
if _GBA_TOOLS not in sys.path:
    sys.path.insert(0, _GBA_TOOLS)
