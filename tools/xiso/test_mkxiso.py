"""
mkxiso.py's images read back by the independent reader (xdvdfs.py walks tables linearly, the C library walks the
tree), both layouts. Run: python3 tools/xiso/test_mkxiso.py, or pytest.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.xiso import mkxiso  # noqa: E402
from tools.xiso.xdvdfs import Xiso  # noqa: E402


def _tree(top):
    files = {"default.xbe": b"XBEH" + bytes(300), "empty.bin": b"", "a/b/c.txt": b"abc" * 1000}
    files.update({"many/f%03d_%s" % (i, "y" * (i % 50)): bytes([i]) * i for i in range(200)})
    for rel, data in files.items():
        p = os.path.join(top, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
    os.makedirs(os.path.join(top, "void"))
    return files


def _check(base):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src")
        files = _tree(src)
        img = mkxiso.build(src, os.path.join(tmp, "t.iso"), base=base)
        with Xiso(img) as iso:
            assert iso.base == base, hex(iso.base)
            got = {(d + "/" + e.name).lstrip("/"): iso.read(e) for d, e in iso.walk()}
        assert got == files


def test_bare_image():
    _check(0)


def test_redump_base():
    _check(0x18300000)      # sparse: the hole before the partition costs no disk space


def test_xgd3_base():
    _check(0x02080000)      # an Xbox 360 XGD3 disc's game partition


if __name__ == "__main__":
    test_bare_image()
    test_redump_base()
    test_xgd3_base()
    print("3 checks passed")
