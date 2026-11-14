"""What the Nintendo 64 pipeline is (XBA-146 / XBA-169 / XBA-170): its stages, implemented by rom.py (parse), isa.py
(disasm), discover.py / dma.py (func_discovery) and lift.py / recomp.py (lift)."""

from . import CONSOLE

VERSION = "1.0.0"


def describe() -> dict:
    """This console's pipeline: its name and its stages."""
    return {
        "console": CONSOLE,
        "version": VERSION,
        "stages": ["parse", "disasm", "func_discovery", "lift"],
        "entry": "python3 -m n64_tools.recomp ROM OUTDIR [--discovered FILE]",
    }
