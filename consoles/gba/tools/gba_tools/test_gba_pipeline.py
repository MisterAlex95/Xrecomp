"""pytest for the Game Boy Advance pipeline placeholder (XBA-146): runs with the tools suite (pytest tools/ consoles/)."""

from gba_tools import CONSOLE
from gba_tools.pipeline import describe


def test_describe():
    d = describe()
    assert CONSOLE == "gba"
    assert d["console"] == "gba"
    assert d["stages"][0] == "parse"
