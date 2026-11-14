"""Placeholder for the Nintendo 64 pipeline (the N64 header, byte order and entry point, MIPS III disassembly, then function discovery and the lifter)."""

from . import CONSOLE

VERSION = "0.0.0"


def describe() -> dict:
    """What this console's pipeline is: its name and the stages it will have."""
    return {
        "console": CONSOLE,
        "version": VERSION,
        "stages": ["parse", "disasm", "func_discovery", "lift"],
    }
