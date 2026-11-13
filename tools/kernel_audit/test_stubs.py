"""tools.kernel_audit.stubs (XBA-115): the stub table and the coverage table
in docs/pipeline/05-runtime.md are what the code says."""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.kernel_audit import stubs  # noqa: E402


def test_generated_files_are_up_to_date():
    # Fails after a stub is implemented (or a new one is added) until
    # `python3 -m tools.kernel_audit.stubs --write` is rerun.
    assert stubs.main(["--check"]) == 0


def test_stub_body_rules():
    assert stubs.is_stub_body("    (void)STACK_ARG(0);\n    (void)STACK_ARG(1);\n    g_eax = 0;\n")
    assert stubs.is_stub_body("    (void)g_ecx;\n    g_eax = 0xC0000001u; /* not supported */\n")
    assert stubs.is_stub_body("    g_eax = STATUS_SUCCESS;\n")
    assert not stubs.is_stub_body("    g_eax = ks_slist_pop(g_ecx);\n")
    assert not stubs.is_stub_body("    uint32_t p = STACK_ARG(0);\n    if (p) BRIDGE_MEM32(p) = 0;\n    g_eax = 0;\n")
    assert not stubs.is_stub_body("")


def test_known_routes_classified():
    impl, stub, ok = stubs.classify()
    # Implemented by XBA-113, stubs before it.
    for o in (56, 57, 58, 104, 117, 136, 12):
        assert o in impl
    # Still a stub when this was written; a real call of it should be reported.
    assert 206 in stub or 206 in impl          # NtQueueApcThread
    assert set(ok) <= set(stubs.NOOP_OK)
    assert not set(impl) & set(stub)


def test_c_table_matches_classification():
    _, stub, _ = stubs.classify()
    text = open(stubs.TABLE_C, encoding="utf-8").read()
    in_c = [int(o) for o in re.findall(r"\{\s*(\d+),\s*\"", text)]
    assert in_c == stub
