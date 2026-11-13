"""pytest for the PlayStation pipeline placeholder (XBA-146): runs with the tools suite (pytest tools/ consoles/)."""

from ps1_tools import CONSOLE
from ps1_tools.pipeline import describe


def test_describe():
    d = describe()
    assert CONSOLE == "ps1"
    assert d["console"] == "ps1"
    assert d["stages"][0] == "parse"
