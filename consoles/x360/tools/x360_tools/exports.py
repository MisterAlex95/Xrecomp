"""xboxkrnl.exe / xam.xex ordinals by name and back (XBA-270), read from consoles/x360/runtime/x360_exports.inc.

    ordinal("xboxkrnl", "DbgPrint") -> 3;  name("xboxkrnl.exe", 3) -> "DbgPrint"
"""

import os
import re

PATH = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime",
                                     "x360_exports.inc"))
_BY_NAME, _BY_ORD = {}, {}


def _lib(lib):
    return lib.split(".")[0].lower()


def _load():
    if _BY_NAME:
        return
    with open(PATH) as f:
        for m in re.finditer(r"X360_EXPORT\((\w+), (0x[0-9A-F]+), (\w+), (\w+)\)", f.read()):
            lib, o, n, k = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
            _BY_NAME[(lib, n)] = (o, k)
            _BY_ORD[(lib, o)] = n


def all_exports():
    """[(lib, name, "FN" | "VAR")] of both libraries."""
    _load()
    return [(lib, n, v[1]) for (lib, n), v in _BY_NAME.items()]


def ordinal(lib, name):
    _load()
    return _BY_NAME[(_lib(lib), name)][0]


def kind(lib, name):
    _load()
    return _BY_NAME[(_lib(lib), name)][1]


def name(lib, o):
    _load()
    return _BY_ORD.get((_lib(lib), o))
