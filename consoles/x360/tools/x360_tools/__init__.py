"""The Xbox 360 static recompiler, spike (XBA-230): the Xenon layer (64-bit integer, VMX, VMX128) on the shared
PowerPC frontend tools.ppc, fed by the XEX2 parser tools.xex. CLI:
`python3 -m x360_tools.recomp FILE.xex OUTDIR [--key-file KEY] [--compile] [--sweep]`.

This package puts the repository root on sys.path, so `tools.ppc` and `tools.xex` import from a checkout without
installing anything.
"""

import os
import sys

CONSOLE = "x360"

_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)
