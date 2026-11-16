"""The Nintendo 3DS static recompiler, spike (XBA-228): the 3DSX loader, the GBA ARM / Thumb lifter through the DS
spike's ARMv5TE layer, extended to the ARM11's ARMv6K + VFPv2. CLI:
`python3 -m ctr_tools.recomp PROG.3dsx OUTDIR [--discovered FILE]`.

The lifter and the discovery are gba_tools' (consoles/gba/tools) and nds_tools' (consoles/nds/tools), subclassed:
this package puts both directories on sys.path, so everything runs from a checkout without installing anything.
"""

import os
import sys

CONSOLE = "3ds"

_CONSOLES = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
for _d in (os.path.join(_CONSOLES, "gba", "tools"), os.path.join(_CONSOLES, "nds", "tools")):
    if _d not in sys.path:
        sys.path.insert(0, _d)
