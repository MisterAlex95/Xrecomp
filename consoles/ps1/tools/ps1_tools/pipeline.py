"""The PlayStation pipeline's stages: exe.py (PS-X EXE + ISO 9660), isa.py (MIPS I), discover.py, lift.py; recomp.py
runs them (XBA-160, XBA-161)."""

from . import CONSOLE

VERSION = "1.0.0"


def describe() -> dict:
    """What this console's pipeline is: its name and its stages."""
    return {
        "console": CONSOLE,
        "version": VERSION,
        "stages": ["parse", "disasm", "func_discovery", "lift"],
    }
