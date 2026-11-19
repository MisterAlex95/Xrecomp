"""The x86 interpreter (XBA-303, src/kernel/xbox_interp_core.c) against the
CPU and against lifted code.

  - test_interpreter_matches_the_cpu: 600 random sequences (integer, flags
    through their consumers, string instructions, shifts, rotates, stc / clc
    / cmc, xadd, cmpxchg, bsf / bsr, div, push / pop, forward branches whose
    join the flags cross) and the NAMED regressions, each run from 8 random
    states on a real x86 CPU (tools/recomp/data/interp_golden.json, made by
    `python3 -m tools.recomp.interp_fixture --golden`, which runs them in the
    linux/386 container), then by the interpreter on this host: every
    register and the data area must match. Needs only a GNU C compiler, so it
    runs on arm64 too.
  - test_lifted_code_matches_the_cpu (XBA-317): the same vectors, every
    sequence lifted by the real translator and built at -O2. No sequence is
    left out: the lifter answers every consumer of a defined flag.
  - test_the_lifter_models_every_consumer: no golden sequence lifts to the
    fallback flag, an unimplemented instruction or an undeclared carry.
  - test_interpreter_matches_lifted_code: the same sequences, interpreter
    against lifted code on this host (more states than the goldens hold).
  - test_sse_mmx_x87_match_lifted_code: the generated half, random SSE / MMX /
    x87 sequences against the lifted C.
  - tools/conformance runs its 7,662 vectors (integer, x87, SSE) through the
    interpreter as a fourth pass against the CPU.
  - test_fresh_sequences_on_the_cpu: new random sequences, three ways in the
    linux/386 container (XBOXRECOMP_PYTEST_DOCKER=1, as tools/conformance).
  - test_the_generated_file_is_current: xbox_interp_gen.inc is what
    tools/recomp/gen_interp.py makes from the lifter today.
"""
import json
import os
import shutil
import subprocess
import tempfile

import pytest

from . import interp_fixture as F
from .interp_fixture import gnu_cc


def _need_cc():
    if not gnu_cc() or not shutil.which(gnu_cc()):
        pytest.skip("no GNU C compiler")


def _capstone():
    try:
        import capstone  # noqa: F401
    except ImportError:
        pytest.skip("capstone not installed")


def test_interpreter_matches_the_cpu():
    _need_cc()
    rc, out = F.run_golden()
    assert rc == 0, out[-3000:]
    with open(F.GOLDEN) as f:
        doc = json.load(f)
    want = sum(len(s["vectors"]) for s in doc["sequences"])
    assert f"ran {want}, bad 0" in out, out[-3000:]


def test_lifted_code_matches_the_cpu():
    """XBA-317: lifted code against the CPU's vectors, every sequence. The
    lifter used to disagree on ~3 % of the sequences it claimed to model
    (setle after xadd ignored OF, sbb after cmpxchg read a stale carry) and
    fell back on a quarter of the rest (a second consumer after a join, seto
    / cmovo, unsigned conditions after shld / shrd, the signed ones after a
    string compare, rep lods)."""
    _need_cc()
    _capstone()
    rc, out = F.run_golden(lifted=True)
    with open(F.GOLDEN) as f:
        doc = json.load(f)
    want = sum(len(s["vectors"]) for s in doc["sequences"])
    assert rc == 0 and f"ran {want}, bad 0" in out, out[-3000:]


def test_the_lifter_models_every_consumer():
    _capstone()
    with open(F.GOLDEN) as f:
        doc = json.load(f)
    gaps = [s.get("name") or s["code"] for s in doc["sequences"]
            if F.lifter_falls_back(bytes.fromhex(s["code"]), F.BASE)]
    assert not gaps, gaps[:10]


def test_interpreter_matches_lifted_code():
    _need_cc()
    _capstone()
    with open(F.GOLDEN) as f:
        doc = json.load(f)
    seqs = [bytes.fromhex(s["code"]) for s in doc["sequences"]]
    rc, out = F.build_and_run(seqs, 8)
    assert rc == 0 and "bad 0" in out, out[-3000:]


def test_sse_mmx_x87_match_lifted_code():
    """The generated half: random SSE / MMX / x87 sequences (register and
    memory forms, immediates, compares with a consumer, fcmov, the x87 stack
    0..8 deep), interpreter against the lifted C: xmm, mm, the x87 stack and
    its state, the integer registers and the data area."""
    _need_cc()
    _capstone()
    seqs, _ = F.fp_sequences(200, 16, 5000)
    rc, out = F.build_and_run(seqs, 8, fp=True)
    assert rc == 0 and "bad 0" in out, out[-3000:]


def test_a_wrong_flag_is_caught():
    """The vectors see flags: the interpreter built with CF inverted after
    every add fails them."""
    _need_cc()
    rc, out = F.run_golden(extra=("-DXBOX_INTERP_MUTATE_ADD_CF=1",))
    assert rc != 0 and "bad 0" not in out, out[-2000:]


@pytest.mark.skipif(os.environ.get("XBOXRECOMP_PYTEST_DOCKER") != "1",
                    reason="XBOXRECOMP_PYTEST_DOCKER=1: runs sequences on the CPU in a linux/386 container")
def test_fresh_sequences_on_the_cpu():
    _capstone()
    if subprocess.run(["docker", "image", "inspect", F.I386_IMAGE], capture_output=True).returncode:
        pytest.skip(f"no {F.I386_IMAGE} image (bash tools/macos/setup.sh --test)")
    seqs, _ = F.sequences(200, 20, int.from_bytes(os.urandom(3), "little"), native=True, filtered=False)
    with tempfile.TemporaryDirectory() as d:
        rc, out = F.run_native(seqs, 10, d)
    detail = "\n".join(l for l in out.splitlines() if not l.startswith(("GOLD", "LIFT")))[-3000:]
    assert rc == 0 and ", bad 0," in out, detail
    assert "lifter differs from the CPU in 0 sequences" in out, detail


def test_the_generated_file_is_current():
    _capstone()
    from . import gen_interp
    assert gen_interp.main(["--check"]) == 0, "python3 -m tools.recomp.gen_interp"
