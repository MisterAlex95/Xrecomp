"""The PSP static recompiler, spike (XBA-224): ELF / PRX / EBOOT.PBP loading (relocations, module info, NID import
stubs), the PS1's MIPS decoder, discovery and lifter extended to the Allegrex. CLI:
`python3 -m psp_tools.recomp MODULE OUTDIR [--discovered FILE]`.

The decoder, the discovery and the lifter are ps1_tools' (consoles/ps1/tools), subclassed: this package puts that
directory on sys.path, so both run from a checkout without installing anything. ps1_tools itself is unchanged.
"""

import os
import sys

CONSOLE = "psp"

_PS1_TOOLS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "ps1",
                                           "tools"))
if _PS1_TOOLS not in sys.path:
    sys.path.insert(0, _PS1_TOOLS)
