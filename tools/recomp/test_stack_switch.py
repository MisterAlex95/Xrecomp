"""Guest stack switches (XBA-101): which esp loads leave for another stack,
and what the lifter emits for them.

A function that loads esp from memory and then returns continues on the
other stack; lifted, its ret would be a C return to the caller on the old
one. The lifter notes the esp it leaves and lands on, and its ret hands them
to recomp_stack_switch_ret (src/kernel/recomp_stack_switch.c), which switches
host stacks with the guest one. The runtime side has its own test,
tests/stack_switch.
"""
import struct

import pytest

from . import config
from .translator import FunctionTranslator
from .lifter import stack_switch_sites

BASE = 0x10000
SLOT = 0x10200
SAVE4 = bytes.fromhex("53555657")           # push ebx, ebp, esi, edi
LOAD4 = bytes.fromhex("5f5e5d5b")           # pop edi, esi, ebp, ebx
RET = b"\xc3"
XCHG_SLOT = b"\x87\x25" + struct.pack("<I", SLOT)   # xchg [SLOT], esp


@pytest.fixture(autouse=True)
def restore_layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", list(config._SECTIONS))


def _translator(image, funcs):
    config._install([config.Section(".text", BASE, 0x400, 0, 0x400, True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="stack-switch-test")
    db = {va: {"start": f"0x{va:08X}", "end": va + size, "_addr": va, "size": size}
          for va, size in funcs}
    return FunctionTranslator(image.ljust(0x400, b"\xcc"), db), db


def _lift(code):
    t, db = _translator(code, [(BASE, len(code))])
    return t.translate_function(BASE, db[BASE])


def _sites(code):
    t, _ = _translator(code, [(BASE, len(code))])
    instructions, _ = t.decode_function(BASE, BASE + len(code))
    return stack_switch_sites(instructions)


def test_xchg_with_memory_is_carried_at_the_ret():
    c = _lift(SAVE4 + XCHG_SLOT + LOAD4 + RET)
    assert "_ss_entry = MEM32(esp)" in c
    assert "_ss_save = esp; _ss_site = 0x00010004u;" in c
    # The save comes before the exchange, the load right after it.
    assert c.index("_ss_save = esp") < c.index("RECOMP_ATOMIC_XCHG32") < c.index("_ss_load = esp;")
    assert ("if (_ss_save) recomp_stack_switch_ret(_ss_save, _ss_load, _ss_entry, 0u, _ss_site);"
            " esp += 4; return;") in c
    assert "recomp_stack_switch_unhandled" not in c


def test_ret_n_passes_its_count():
    c = _lift(SAVE4 + XCHG_SLOT + LOAD4 + b"\xc2\x04\x00")      # ret 4
    assert "recomp_stack_switch_ret(_ss_save, _ss_load, _ss_entry, 4u, _ss_site); esp += 8;" in c


@pytest.mark.parametrize("code", [
    bytes.fromhex("8b6104"),            # mov esp, [ecx+4]
    bytes.fromhex("8b01" "8be0"),       # mov eax, [ecx]; mov esp, eax
    bytes.fromhex("8b2500020100"),      # mov esp, [0x10200]
    bytes.fromhex("876104"),            # xchg [ecx+4], esp
])
def test_loads_from_another_stack_are_sites(code):
    assert len(_sites(code + RET)) == 1


@pytest.mark.parametrize("code", [
    bytes.fromhex("8be5"),              # mov esp, ebp (epilogue)
    bytes.fromhex("8d65f4"),            # lea esp, [ebp-0xc]
    bytes.fromhex("8b65e8"),            # mov esp, [ebp-0x18] (__except block)
    bytes.fromhex("8d4c2408" "2bc8" "8be1"),    # _chkstk: lea ecx, [esp+8]; sub ecx, eax; mov esp, ecx
    bytes.fromhex("54" "50" "58" "5c"), # push esp; push eax; pop eax; pop esp
    bytes.fromhex("94"),                # xchg esp, eax
    bytes.fromhex("8b01" "eb00" "8be0"),        # a branch between the load and the move
])
def test_esp_from_this_stack_is_not_a_site(code):
    assert _sites(code + RET) == []
    assert "_ss_" not in _lift(code + RET)


def test_a_switch_with_no_ret_is_reported_at_run_time():
    c = _lift(bytes.fromhex("8b21" "ff6104"))       # mov esp, [ecx]; jmp [ecx+4]
    assert "recomp_stack_switch_unhandled(0x00010000u);" in c
    assert "recomp_stack_switch_ret" not in c


def test_the_next_function_does_not_inherit_the_sites():
    switch = SAVE4 + XCHG_SLOT + LOAD4 + RET
    plain = bytes.fromhex("8bc1") + RET           # mov eax, ecx; ret
    image = switch + plain
    t, db = _translator(image, [(BASE, len(switch)), (BASE + len(switch), len(plain))])
    assert "recomp_stack_switch_ret" in t.translate_function(BASE, db[BASE])
    c = t.translate_function(BASE + len(switch), db[BASE + len(switch)])
    assert "_ss_" not in c and "recomp_stack_switch" not in c
