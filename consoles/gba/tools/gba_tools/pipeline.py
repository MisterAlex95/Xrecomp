"""What the Game Boy Advance pipeline is (XBA-146): its stages, each a module of this package."""

from . import CONSOLE

VERSION = "1.0.0"


def describe() -> dict:
    """The console, the version of the pipeline and its stages (rom, isa / discover, lift, recomp)."""
    return {
        "console": CONSOLE,
        "version": VERSION,
        "stages": ["parse", "disasm", "func_discovery", "lift"],
        "cli": "python3 -m gba_tools.recomp ROM OUTDIR [--discovered FILE]",
    }
