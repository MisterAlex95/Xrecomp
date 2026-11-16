"""The GameCube static recompiler, spike (XBA-225): the DOL format and the Gekko layer (paired singles, quantised
loads / stores) on the shared PowerPC frontend tools.ppc. CLI:
`python3 -m gc_tools.recomp PROG.dol OUTDIR [--discovered FILE]`.

This package puts the repository root on sys.path, so `tools.ppc` imports from a checkout without installing
anything.
"""

import os
import sys

CONSOLE = "gc"

_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)
