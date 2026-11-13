"""A seed cannot cut a detected body (WP2, profile-free report section 2).

Fable's discovery file held 0x001C120A, a garbage target of a wild indirect
call that lands two bytes into `mov eax, [esp+0x10]` of a loop. Trusted as
"observed", it made sub_001C1190 end at 0x001C120C with no terminator: the
lift returned without its pops and `ret 8`, and the guest stack was 20 bytes
off after every call. The same shape here, on a synthetic XBE of our own:

  - the seed two bytes into the `mov` is refused, observed or not, and the
    function's bounds are those of the analysis without seeds;
  - the 14 junk values of that discovery file change nothing either;
  - a seed in bytes no body claims (a data byte the sweep decoded across the
    entry, the XID / XPP init shape) is still accepted;
  - a seed on an instruction boundary inside a body is an alias entry, not
    a start that splits the body;
  - a call target inside an instruction (no seed involved) is demoted to an
    alias, so the body before it keeps its tail;
  - no two output bodies overlap, on every synthetic title (an analysis
    error otherwise, OverlappingBodies).
"""

import contextlib
import io
import json
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm.functions import (FunctionDetector, Function,     # noqa: E402
                                    OverlappingBodies)
from tools.disasm.synthetic import (Title, discovery_corpus,        # noqa: E402
                                    discovery_e2e_title, packed_table_title,
                                    CORPUS_IMPORTS, E2E_IMPORTS)

# The seeds tools.seed_from_log let through from Fable's wild call site
# (report section 2.1); plain numbers, no game bytes.
FABLE_JUNK = (0x00031886, 0x00046600, 0x00080400, 0x000C8D00, 0x00100A03,
              0x00118EAC, 0x001C120A, 0x002140C6, 0x00267B00, 0x00280C80,
              0x003C003D, 0x00525000, 0x006A5057, 0x006AFFDD)


def loop_title():
    """entry calls `build`, a list-building loop shaped like Fable's
    sub_001C1190; `mov_cut` labels the `mov eax, [esp+0x14]` whose third
    byte a wild seed names. After it, a byte of data (`68`, push imm32 to the
    sweep) then `xinit`, reached by nothing static: the sweep decodes across
    its entry, but no body claims those bytes."""
    t = Title()
    x = t.section(".text")
    x.label("entry").hex("6a02 6a00").call("build").call("xinit").hex("ebfe")
    x.align(16)
    x.label("build").hex("55 57 53")              # push ebp / push edi / push ebx
    x.hex("8b6c2410 33db")                        # mov ebp, [esp+0x10] / xor ebx, ebx
    x.label("loop").hex("8b7d08")                 # mov edi, [ebp+8]
    x.hex("8b4508 894704")                        # mov eax, [ebp+8] / mov [edi+4], eax
    x.label("mov_cut").hex("8b442414")            # mov eax, [esp+0x14]  <- seed at +2
    x.hex("43 3bd8 897d08")                       # inc ebx / cmp ebx, eax / mov [ebp+8], edi
    x.hex("7c").rel8("loop")                      # jl loop
    x.hex("5b 5f b001 5d c20800")                 # pop ebx / pop edi / mov al, 1 / pop ebp / ret 8
    x.label("blob").hex("68")                     # a data byte: push imm32 to the sweep
    x.label("xinit").hex("803d").abs("flag").hex("00")   # cmp byte [flag], 0
    x.hex("7502 b001 c3")                         # jne +2 / mov al, 1 / ret
    x.align(16)
    x.label("flag").hex("00000000")
    return t


def analyse(t, tmp_path, entry="entry", imports=(), seeds=(), observed=(), name="default.xbe"):
    """(functions.json by start, disassembler) of title `t`."""
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    from tools.disasm.disasm import Disassembler
    os.makedirs(tmp_path, exist_ok=True)
    xbe = t.write(os.path.join(tmp_path, name), entry, imports=imports)
    xjson = xbe[:-4] + "_analysis.json"
    export_json(XBEParser(xbe).parse(), xjson)
    out = os.path.join(tmp_path, "out-%d" % len(seeds))
    d = Disassembler(xbe, analysis_json=xjson, output_dir=out, force=True,
                     seed_functions=sorted(set(seeds)), observed_seeds=set(observed))
    log = io.StringIO()
    with contextlib.redirect_stdout(log):
        assert d.run()
    funcs = json.load(open(os.path.join(out, "functions.json")))
    return {int(f["start"], 16): f for f in funcs}, log.getvalue()


def bounds(funcs):
    return {a: (f["start"], f["end"]) for a, f in funcs.items()}


def test_observed_seed_inside_a_mov_is_refused(tmp_path):
    t = loop_title()
    clean, _ = analyse(t, tmp_path / "clean")
    build = t.va("build")
    cut = t.va("mov_cut") + 2
    assert build in clean and int(clean[build]["end"], 16) == t.va("blob")
    poisoned, log = analyse(t, tmp_path / "seeded", seeds=[cut], observed=[cut])
    assert cut not in poisoned
    assert bounds(poisoned) == bounds(clean)
    assert "Rejected 1 seed(s)" in log and "0x%08X" % cut in log and "sub_%08X" % build in log


def test_fable_junk_values_change_no_bounds(tmp_path):
    t = loop_title()
    clean, _ = analyse(t, tmp_path / "clean")
    junk = list(FABLE_JUNK) + [t.va("mov_cut") + 2, t.va("mov_cut") + 1, t.va("loop") + 1]
    poisoned, _ = analyse(t, tmp_path / "junk", seeds=junk, observed=junk)
    assert bounds(poisoned) == bounds(clean)


def test_seed_in_unclaimed_bytes_is_still_accepted(tmp_path):
    t = loop_title()
    xinit = t.va("xinit")
    clean, _ = analyse(t, tmp_path / "clean")
    funcs, _ = analyse(t, tmp_path / "seeded", seeds=[xinit], observed=[xinit])
    assert xinit in funcs
    assert int(funcs[t.va("build")]["end"], 16) == int(clean[t.va("build")]["end"], 16)


@pytest.mark.parametrize("make", ["corpus", "e2e", "packed", "loop"])
def test_no_two_bodies_overlap_on_any_synthetic_title(tmp_path, make):
    if make == "corpus":
        t, imports = discovery_corpus()[0], CORPUS_IMPORTS
    elif make == "e2e":
        t, imports = discovery_e2e_title(), E2E_IMPORTS
    elif make == "packed":
        t, imports = packed_table_title()[0], CORPUS_IMPORTS
    else:
        t, imports = loop_title(), ()
    funcs, _ = analyse(t, tmp_path, imports=imports)
    bodies = sorted((int(f["start"], 16), int(f["end"], 16)) for f in funcs.values()
                    if f["detection_method"] != "tail_jump_alias")
    for (s0, e0), (s1, e1) in zip(bodies, bodies[1:]):
        assert e0 <= s1, "0x%08X-0x%08X overlaps 0x%08X-0x%08X" % (s0, e0, s1, e1)


def test_overlap_is_an_analysis_error():
    d = FunctionDetector.__new__(FunctionDetector)
    d.functions = {
        0x1C1190: Function(0x1C1190, 0x1C120C, "sub_001C1190", detection_method="call_target"),
        0x1C120A: Function(0x1C120A, 0x1C1220, "sub_001C120A", detection_method="seed_vtable_thunk"),
        # An alias shares its parent's tail on purpose.
        0x1C1218: Function(0x1C1218, 0x1C1220, "sub_001C1218", detection_method="tail_jump_alias"),
    }
    with pytest.raises(OverlappingBodies) as e:
        d.check_no_overlap()
    assert "sub_001C1190 0x001C1190-0x001C120C" in str(e.value)
    assert "sub_001C120A 0x001C120A-0x001C1220" in str(e.value)
    del d.functions[0x1C120A]
    d.check_no_overlap()


def test_call_target_inside_an_instruction_becomes_an_alias(tmp_path):
    """Not only seeds: a direct call two bytes into the loop's `mov` (Fable's
    sub_006372E8, cut at 0x0063736C by a call target in a `mov ax, [esi+4]`,
    in the analysis without any seed). The body keeps its tail, the target
    stays callable as an alias running to the body's end."""
    t = Title()
    x = t.section(".text")
    x.label("entry").hex("6a02 6a00").call("build").call("mid").hex("ebfe")
    x.align(16)
    x.label("build").hex("55 57 53 8b6c2410 33db")
    x.label("loop").hex("8b7d08 8b4508 894704")
    x.hex("8b44").label("mid").hex("2414")        # mov eax, [esp+0x14], called at +2
    x.hex("43 3bd8 897d08").hex("7c").rel8("loop")
    x.hex("5b 5f b001 5d c20800")
    x.label("after").align(16)
    funcs, log = analyse(t, tmp_path)
    build, mid = t.va("build"), t.va("mid")
    assert int(funcs[build]["end"], 16) == t.va("after")
    assert funcs[mid]["detection_method"] == "tail_jump_alias"
    assert int(funcs[mid]["end"], 16) == t.va("after")
    assert "made aliases: 0x%08X" % mid in log


def test_seed_on_a_boundary_inside_a_body_is_an_alias(tmp_path):
    """On an instruction boundary inside a detected body, a seed is an alias
    entry running to the body's end, not a start that splits the body."""
    t = loop_title()
    clean, _ = analyse(t, tmp_path / "clean")
    seed = t.va("mov_cut")
    funcs, log = analyse(t, tmp_path / "seeded", seeds=[seed], observed=[seed])
    build = t.va("build")
    assert funcs[build]["end"] == clean[build]["end"]
    assert funcs[seed]["detection_method"] == "tail_jump_alias"
    assert funcs[seed]["end"] == clean[build]["end"]
    assert "1 seed(s) inside a detected body kept as alias entries" in log
