"""bringup (XBA-270): the one-command path on the encrypted, LZX-compressed test title. Needs an x360_run built from
this tree, named by $X360_RUN (a glob for one could pick up a stale binary): skipped without it."""

import os
import shutil
import sys

import pytest

from . import CONSOLE  # noqa: F401
from .bringup import main

X360_RUN = os.environ.get("X360_RUN", "")

TESTS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests"))


@pytest.mark.skipif(not os.path.isfile(X360_RUN) or not shutil.which("cc") or not sys.platform.startswith("linux"),
                    reason="$X360_RUN not set to a built x360_run / no C compiler")
def test_bringup_on_the_test_title(tmp_path, capsys):
    sys.path.insert(0, TESTS)
    import make_hello
    key = b"xba-270-made-up!"
    (tmp_path / "k.key").write_bytes(key)
    (tmp_path / "t.xex").write_bytes(make_hello.build("normal", key))
    disc = tmp_path / "disc"
    (disc / "data").mkdir(parents=True)
    (disc / "data" / "hello.txt").write_text("Hello from the disc!")
    (disc / "readme.txt").write_text("readme\n")
    work = tmp_path / "work"
    assert main([str(tmp_path / "t.xex"), str(work), "--key-file", str(tmp_path / "k.key"), "--disc", str(disc),
                 "--x360-run", X360_RUN]) == 0
    out = capsys.readouterr().out
    assert "compression normal (LZX)" in out and "missing: 0" in out
    assert "the title ended (exit code 0)" in out
    summary = (work / "summary.txt").read_text()
    assert "Hello from the disc" not in summary  # the title's own text stays out of the summary
    assert (work / "dbg.txt").read_text().startswith("Xbox 360 title recompiled")


def test_shader_report(tmp_path):
    import struct

    from .bringup import shader_report
    from .xenos_asm import alu_words, export, program
    good = program([(2, [("alu", alu_words(**export(0, 1)))], {})])
    loop = program([(2, [("alu", alu_words(vop=2, vdest=40, vmask=0xF, export=1))], {})])   # no such register
    mx = program([(2, [("alu", alu_words(vop=2, vdest=32, vmask=0xF, export=1))], {})])     # memory export (XBA-282)
    for name, words in (("vs_00000001.bin", good), ("ps_00000002.bin", good), ("vs_00000003.bin", loop),
                        ("vs_00000004.bin", mx)):
        (tmp_path / name).write_bytes(struct.pack(">%dI" % len(words), *words))
    rep = shader_report(str(tmp_path))
    assert rep == {"total": 4, "translated": 3, "unsupported": {"export to register 40": 1}}
    assert (tmp_path / "vs_00000004.vert").read_text().startswith("#version 310 es")   # GLSL ES 3.10: memexport
    assert (tmp_path / "vs_00000001.vert").exists() and (tmp_path / "ps_00000002.frag").exists()
