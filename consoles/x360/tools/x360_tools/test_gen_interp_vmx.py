"""gen_interp_vmx (XBA-283): the runtime's x360_interp_vmx.inc is what the generator makes from the current lifter
(regenerate with python3 -m x360_tools.gen_interp_vmx after a change to xenon.py's vector instructions)."""

from . import CONSOLE  # noqa: F401

from .gen_interp_vmx import OUT, generate


def test_vmx_interpreter_is_current():
    with open(OUT) as f:
        assert f.read() == generate(), "x360_interp_vmx.inc is stale: python3 -m x360_tools.gen_interp_vmx"


def test_every_vector_mnemonic_has_a_case():
    from .xenon import OP5, OP6, OP6_3, OP6_R, VA4, VC4, VX4, VX128_1, X31_VMX
    text = generate()
    names = set()
    for t in (OP5, OP6, OP6_3, OP6_R, VA4, VC4, VX4, VX128_1, X31_VMX):
        names |= set(t.values())
    names |= {"vsldoi128", "vperm128", "vpermwi128", "vpkd3d128", "vrlimi128"}
    missing = sorted(n for n in names if "/* %s */" % n not in text)
    assert not missing, missing
