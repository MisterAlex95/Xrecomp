"""pytest for the Nintendo 64 pipeline description (XBA-146): runs with the tools suite (pytest tools/ consoles/)."""

from n64_tools import CONSOLE
from n64_tools.pipeline import describe


def test_describe():
    d = describe()
    assert CONSOLE == "n64"
    assert d["console"] == "n64"
    assert d["stages"][0] == "parse"
