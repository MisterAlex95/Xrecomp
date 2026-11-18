"""Remake consoles/x360/runtime/x360_exports.inc (XBA-270): the ordinals and names of xboxkrnl.exe and xam.xex.

    python3 -m x360_tools.gen_exports [XBOXKRNL_TABLE.inc XAM_TABLE.inc]

The ordinals are the console's ABI; the list comes from Xenia's export tables (src/xenia/kernel/xboxkrnl/
xboxkrnl_table.inc and xam/xam_table.inc, BSD licence, Copyright Ben Vanik; downloaded from GitHub when no paths
are given). Only (library, ordinal, name, kind) is kept, as X360_EXPORT lines the runtime and x360_tools.exports
read. Not run by the build: the output is committed."""

import os
import re
import sys
import urllib.request

URLS = ["https://raw.githubusercontent.com/xenia-project/xenia/master/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc",
        "https://raw.githubusercontent.com/xenia-project/xenia/master/src/xenia/kernel/xam/xam_table.inc"]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "runtime", "x360_exports.inc"))
LINE = re.compile(r"XE_EXPORT\((\w+),\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)")


def main(argv):
    texts = []
    for k, url in enumerate(URLS):
        if len(argv) > k:
            with open(argv[k]) as f:
                texts.append(f.read())
        else:
            with urllib.request.urlopen(url, timeout=60) as r:
                texts.append(r.read().decode())
    rows = []
    for t in texts:
        for m in LINE.finditer(t):
            lib, ordinal, name, kind = m.groups()
            rows.append((lib, int(ordinal, 16), name, "VAR" if kind == "kVariable" else "FN"))
    with open(OUT, "w") as f:
        f.write("/* x360_exports.inc -- the ordinals of xboxkrnl.exe and xam.xex (XBA-270), made by\n"
                " * x360_tools/gen_exports.py from Xenia's export tables (xboxkrnl_table.inc, xam_table.inc;\n"
                " * BSD licence, Copyright Ben Vanik): library, ordinal, name, FN or VAR. */\n")
        for lib, ordinal, name, kind in rows:
            f.write("X360_EXPORT(%s, 0x%03X, %s, %s)\n" % (lib, ordinal, name, kind))
    print("%s: %d exports" % (OUT, len(rows)))


if __name__ == "__main__":
    main(sys.argv[1:])
