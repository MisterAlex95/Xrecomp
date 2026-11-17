"""
Static discovery (XBA-137): find at the first prepare the code that only a run
would otherwise reveal.

Every function the analysis misses costs the player a crash or a wrong
behaviour, then a re-prepare of minutes on a phone (the self-healing loop,
XBA-64/65/97/129). The passes in functions.py already take a function whose
address is an immediate or a table word when it sits in a gap and closes its
own CFG there. This module adds the evidence they do not use, in two levels:

  1 (strong evidence: the address is code, the question is only where)
    * noreturn kernel calls (HalReturnToFirmware, KeBugCheck(Ex),
      PsTerminateSystemThread) end a body, so the function packed after one is
      no longer swallowed by it; and so does a direct call the compiler padded
      with int3 (`call exit; int3`, `call _CxxThrowException; int3`): MSVC
      emits nothing after a call it knows never returns, so the alignment
      int3 lands right behind it (noreturn_targets);
    * callback arguments: the routine pushed for PsCreateSystemThread(Ex)
      (start routine and StartContext1), KeInitializeDpc, KeInitializeApc,
      NtQueueApcThread, KeInitializeInterrupt -- and for a function that
      forwards an argument of its own there (the library's CreateThread
      hands its lpStartAddress on as StartContext1), through any number of
      such wrappers;
    * C++ vtables named by a constructor's `mov dword ptr [reg(+n)], vtable`,
      and the vtables MSVC RTTI describes (tools.rtti);
    * the handlers and filters of exception frames (tools.recomp.seh_frames):
      a funclet outside its function becomes a function, a resume target
      just past it extends the function;
    * jump tables the sweep did not measure (engine.py): a table in a data
      section bounded by the `cmp; ja` in front of its dispatch, byte-indexed
      two-level tables, and `mov reg, [idx*4 + table]; jmp reg`.
    Such an address becomes a function start with the gates a run's seed gets
    (tools.seed_from_log: executable, reads as a function body) plus an
    instruction boundary or a prologue; inside a measured body only at a clean
    split (the instruction before ends a path, and no branch crosses it).

  2 (default; 1 + gap code, no reference at all)
    * bytes in a gap between known bodies, after padding or a terminator,
      that decode as a closed function body with a balanced stack, no
      instruction data decodes into, and at least three instructions or a
      prologue: code reached only through a computed address.
    * packed pointer tables (XBA-140): a run of records of one size in a
      data section whose same field names code, at any alignment (the
      table-word pass in functions.py reads aligned words only); the MSVC
      CRT's x87 math dispatch packs its handler pointers after names of
      any length, and its handlers are frameless. Each word takes a run's
      seed gates plus a closed compiled body, no prologue asked, where a
      function starts (run_packed_tables). Level 2: a run of data words is
      not the strong evidence of level 1. A table one of whose runs is taken
      is then completed (WP5): its other words that name code next to its
      established entries, on an instruction boundary, become aliases even
      where the routine is entered mid path (the CRT's helpers fall into
      each other), so no seed has to anchor the table.

A decoy (data that looks like a pointer: floats, strings, colours, offsets
into the middle of code) must never become a function: see
test_static_discovery.py, which measures the false-positive rate on a decoy
corpus, and the lift growth.

Switches (docs/runtime/environment-variables.md):
  RECOMP_STATIC_DISCOVERY      0 off, 1 strong evidence only, 2 (default) all
  RECOMP_STATIC_DISCOVERY_CAP  at most this many percent more functions than
                               the passes before found (default: no cap)
  RECOMP_STATIC_DISCOVERY_STRICT  1: also tighten the OLDER passes (wave 5):
                               a start after a ret must be a compiled body,
                               a table word must not name the middle of a
                               path nor random bytes that close, and a data
                               section's switch table holds no function
                               pointers. Off by default: on synthetic titles
                               it drops 6-14 % of the functions the older
                               passes found, and on a real title one of them
                               may be genuine; on once the owner has compared
                               the counts on the reference titles. The new
                               passes above (strong evidence, gaps) only add,
                               and stay on.
tools.disasm --static-discovery / --discovery-cap override them.
"""

import array
import bisect
import os
import re
import sys
from typing import Dict, List, Optional, Set, Tuple

ENV_LEVEL = "RECOMP_STATIC_DISCOVERY"
ENV_CAP = "RECOMP_STATIC_DISCOVERY_CAP"
ENV_STRICT = "RECOMP_STATIC_DISCOVERY_STRICT"
OFF, STRONG, FULL = 0, 1, 2

# Kernel calls that never return: a body ends at them.
NORETURN_IMPORTS = frozenset({
    "HalReturnToFirmware", "KeBugCheck", "KeBugCheckEx", "PsTerminateSystemThread",
    # A Windows PE's (XBA-233, tools/pe_parser): kernel32 and the CRT's exits. No Xbox kernel export has these names.
    "ExitProcess", "ExitThread", "FatalExit",
    "exit", "_exit", "abort", "_amsg_exit",
})

# Kernel calls taking a code pointer: name -> the argument indices (0 = the
# first, the one pushed last) that are routines.
CALLBACK_ARGS = {
    # (ThreadHandle, ExtensionSize, KernelStackSize, TlsDataSize, ThreadId,
    #  StartContext1, StartContext2, CreateSuspended, DebuggerThread,
    #  SystemRoutine): XAPI passes its startup as the routine and the title's
    #  as StartContext1; the runtime records both (thread, thread_ctx).
    "PsCreateSystemThreadEx": (9, 5),
    # (ThreadHandle, ThreadId, StartRoutine, StartContext, DebuggerThread)
    "PsCreateSystemThread": (2,),
    # (Dpc, DeferredRoutine, DeferredContext)
    "KeInitializeDpc": (1,),
    # (Apc, Thread, KernelRoutine, RundownRoutine, NormalRoutine, Mode, Context)
    "KeInitializeApc": (2, 3, 4),
    # (ThreadHandle, ApcRoutine, ApcContext, IoStatusBlock, Reserved)
    "NtQueueApcThread": (1,),
    # (Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, Mode, Share)
    "KeInitializeInterrupt": (1,),
}

# What data decodes into and a compiler does not emit in a function body.
DATA_MNEMONICS = frozenset({
    "arpl", "bound", "les", "lds", "into", "aaa", "aas", "daa", "das", "aam",
    "aad", "salc", "xlatb", "pushal", "popal", "pushad", "popad", "sti", "cli",
    "hlt", "ljmp", "lcall", "retf", "iretd", "iret", "int1", "icebp", "enter",
})

CALLEE_SAVED = ("ebx", "esi", "edi", "ebp")
_SAVED_ALIASES = {"ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
                  "esi": "esi", "si": "esi", "edi": "edi", "di": "edi",
                  "ebp": "ebp", "bp": "ebp"}

# The pass names, as functions.json's detection_method and summary.json's
# "discovery" keys.
METHODS = ("callback_arg", "ctor_vtable", "rtti_vtable", "seh_funclet", "gap_body",
           "packed_table")

# Packed pointer tables (level 2, XBA-140): a run of at least this many
# records, a record at most this many bytes. On the decoy corpora
# (test_static_discovery.py, test_packed_tables.py) the target gate alone
# refuses every decoy, even from one record up; three is the margin: a
# string or a colour table holds a lone code-looking word or two by chance,
# a dispatch table holds more records than that.
PACKED_MIN_RECORDS = 3
PACKED_MAX_STRIDE = 32
# Completing a table (run_packed_tables): a refused word is taken only when
# it names code this close to an established entry of its table. The CRT's
# math routines sit within a few KB of each other; a record header read as a
# word (`01 id id` + a pointer's low byte) names code megabytes away.
PACKED_NEAR = 0x10000
_U32 = "I" if array.array("I").itemsize == 4 else "L"


def level_from_env(env=None) -> int:
    v = (env if env is not None else os.environ).get(ENV_LEVEL, "").strip()
    if v == "":
        return FULL
    try:
        return max(OFF, min(FULL, int(v)))
    except ValueError:
        return OFF if v.lower() in ("off", "no", "false") else FULL


def strict_from_env(env=None) -> bool:
    """RECOMP_STATIC_DISCOVERY_STRICT: 1/on/yes/true tightens the older passes."""
    v = (env if env is not None else os.environ).get(ENV_STRICT, "").strip().lower()
    return v in ("1", "on", "yes", "true")


def cap_from_env(env=None) -> Optional[float]:
    v = (env if env is not None else os.environ).get(ENV_CAP, "").strip()
    try:
        return float(v) if v else None
    except ValueError:
        return None


def _self_test(insn) -> bool:
    """`or r, r` / `and r, r`: a flag test that leaves r as it was. The CRT's
    x87 helpers test a flag the dispatcher left in bl this way (`or bl, bl`),
    which is a read of ebx, not a clobber."""
    ops = insn.operands
    return (insn.mnemonic in ("or", "and") and len(ops) == 2
            and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg)


def stack_balanced(decoded, entry: int, lower: int, upper: int) -> bool:
    """Does every path from `entry` through `decoded` (recomp Instructions of
    a closed CFG in [lower, upper)) reach its ret, or its jump out, with esp
    back where it was on entry?

    Tracks the bytes pushed since entry: push/pop, sub/add esp, an ebp frame
    (mov ebp, esp ... leave / mov esp, ebp / lea esp, [ebp-N]). A call that
    is not followed by `add esp, N` is taken as stdcall and pops the
    arguments pushed since the previous call (register saves of the prologue
    are not arguments). Two paths that meet with different depths, an esp
    write it cannot follow, or a ret with anything left on the stack, and it
    is not a body a compiler made."""
    by = {i.address: i for i in decoded}
    if entry not in by:
        return False
    # address -> (depth, ebp depth or None, argument pushes, in prologue)
    state = {entry: (0, None, 0, True)}
    work = [entry]
    steps = 0
    while work:
        steps += 1
        if steps > 20000:
            return False
        a = work.pop()
        d, ebp, args, prologue = state[a]
        insn = by[a]
        m = insn.mnemonic
        ops = insn.operands
        dst = ops[0] if ops else None
        src = ops[1] if len(ops) > 1 else None
        is_reg = lambda op, r: op is not None and op.type == "reg" and op.reg == r
        still_prologue = False
        if m == "push":
            d += 4
            if prologue and dst is not None and dst.type == "reg" and dst.reg in CALLEE_SAVED:
                still_prologue = True
            else:
                args += 1
        elif m in ("pushfd", "pushf"):
            d += 4
            args += 1
        elif m == "pop" or m in ("popfd", "popf"):
            d -= 4
            args = max(0, args - 1)
            if is_reg(dst, "ebp"):
                ebp = None
        elif m == "sub" and is_reg(dst, "esp") and src is not None and src.type == "imm":
            d += src.imm
            still_prologue = prologue
        elif m == "add" and is_reg(dst, "esp") and src is not None and src.type == "imm":
            d -= src.imm
            args = 0
        elif m == "mov" and is_reg(dst, "ebp") and is_reg(src, "esp"):
            ebp = d
            still_prologue = prologue
        elif m == "mov" and is_reg(dst, "esp"):
            if not is_reg(src, "ebp") or ebp is None:
                return False
            d = ebp
        elif m == "lea" and is_reg(dst, "esp"):
            if src is None or src.type != "mem" or src.mem_index:
                return False
            disp = src.mem_disp if src.mem_disp < 0x80000000 else src.mem_disp - (1 << 32)
            if src.mem_base == "ebp" and ebp is not None:
                d = ebp - disp
            elif src.mem_base == "esp":
                d -= disp
            else:
                return False
        elif m == "leave":
            if ebp is None:
                return False
            d, ebp = ebp - 4, None
        elif m == "call":
            nxt = by.get(insn.end_address)
            cdecl = (nxt is not None and nxt.mnemonic == "add" and nxt.operands
                     and is_reg(nxt.operands[0], "esp"))
            if not cdecl:
                d -= 4 * args
            args = 0
        elif "esp" in (insn.regs_written or ()) and not insn.is_ret and not insn.is_branch:
            return False                # and esp, -16; xchg; an esp write it cannot follow
        if d < 0:
            return False
        if insn.is_ret:
            if d != 0:
                return False
            continue
        succ = []
        if insn.is_branch:
            t = insn.jump_target
            if t is None or not (lower <= t < upper):
                # Out of the gap: a tail jump (direct or through memory).
                if d != 0:
                    return False
            elif t in by:
                succ.append(t)
        if not insn.is_jump:
            if insn.end_address in by:
                succ.append(insn.end_address)
        for s in succ:
            new = (d, ebp, args, still_prologue)
            old = state.get(s)
            if old is None:
                state[s] = new
                work.append(s)
            elif old[0] != d:
                return False
    return True


class StaticDiscovery:
    """The passes above, run by FunctionDetector.detect_all (functions.py)."""

    def __init__(self, detector, level: int = FULL, cap: Optional[float] = None,
                 rtti_methods=(), strict: Optional[bool] = None):
        self.d = detector
        self.engine = detector.engine
        self.image = detector.image
        self.level = level
        # The tightening of the older passes (table words, after-ret starts,
        # data-section switch tables): RECOMP_STATIC_DISCOVERY_STRICT, off
        # by default (see the module docstring).
        self.strict = strict_from_env() if strict is None else bool(strict)
        self.cap = cap
        self.rtti_methods = set(rtti_methods or ())
        self.stats: Dict[str, int] = {m: 0 for m in METHODS}
        self.refused: Dict[str, int] = {}
        self.baseline: Optional[int] = None
        self.capped = False
        self.packed_completed = 0
        self.noreturn_slots: Set[int] = set()
        # Direct call targets the compiler treats as noreturn, and where the
        # sweep's branches land, by target -> their sources (an int3 a branch
        # reaches is a trap, not padding): find_noreturn_targets.
        self.noreturn_targets: Set[int] = set()
        self._branch_sources: Dict[int, List[int]] = {}
        self.callback_slots: Dict[int, Tuple[int, ...]] = {}
        # Functions that forward an argument of their own as a routine
        # (callback_args): start -> the argument indices, for summary.json.
        self.callback_wrappers: Dict[int, Tuple[int, ...]] = {}
        if level >= STRONG:
            for ki in self.image.kernel_imports:
                if ki.name in NORETURN_IMPORTS:
                    self.noreturn_slots.add(ki.thunk_addr)
                if ki.name in CALLBACK_ARGS:
                    self.callback_slots[ki.thunk_addr] = CALLBACK_ARGS[ki.name]

    # -- noreturn calls ---------------------------------------------------

    def find_noreturn_targets(self) -> None:
        """Direct call targets that never return, from the sweep (level 1).

        A title's exit(), _CxxThrowException or ExitThread returns as far as
        its own body tells (it ends in a call to another routine, a raise, a
        kernel call through a pointer), so no CFG proves it. Its callers do:
        MSVC emits nothing after a call it knows never returns, and when that
        call is the last instruction of a function the alignment int3 sits
        right behind it -- `push [esp+4]; call ExitThread; int3` then the next
        function. A call that returned would land on that int3, so the int3
        is the compiler's word that the target is noreturn.

        A call site still ends a body only when it is itself followed by an
        int3 nothing branches to (is_noreturn_call): a call to such a target
        followed by code is not cut, since the code after may be reached by a
        branch the walk has not seen yet (a backward one, or one from a later
        path), and a cut body crashes the title."""
        self.noreturn_targets = set()
        self._branch_sources = {}
        if self.level < STRONG:
            return
        insns = list(self.engine.instructions.values())
        for i in insns:
            if getattr(i, "is_branch", False) and i.jump_target is not None:
                self._branch_sources.setdefault(i.jump_target, []).append(i.address)
        for insn in insns:
            target = getattr(insn, "call_target", None)
            if (target is None or target in self.noreturn_targets
                    or not self._int3_pad_at(insn.end_address)):
                continue
            section = self.image.get_section_at_va(target)
            if section is not None and section.executable:
                self.noreturn_targets.add(target)

    def _int3_pad_at(self, addr: int) -> bool:
        """An int3 at addr that no branch reaches: padding, not a trap."""
        if addr in self._branch_sources:
            return False
        b = self.image.read_bytes_at_va(addr, 1)
        return bool(b) and b[0] == 0xCC

    def is_noreturn_call(self, insn) -> bool:
        """Does the path end at this call? A noreturn kernel call, or a call
        to a noreturn target the compiler padded with int3 right here."""
        if insn is None or not getattr(insn, "is_call", False):
            return False
        if insn.memory_ref is not None and insn.memory_ref in self.noreturn_slots:
            return True
        return (insn.call_target is not None
                and insn.call_target in self.noreturn_targets
                and self._int3_pad_at(insn.end_address))

    def branched_over(self, addr: int, window: int = 0x400) -> bool:
        """Does a branch shortly before addr land on it? Then addr is the
        rest of the body a noreturn call sits in (`jz ok; call throw; int3;
        ok:`), not the next function, even with int3 in front."""
        return any(addr - window <= src < addr
                   for src in self._branch_sources.get(addr, ()))

    # -- bookkeeping ------------------------------------------------------

    def _budget(self) -> bool:
        """May one more function be added?"""
        if self.cap is None:
            return True
        if self.baseline is None:
            self.baseline = len(self.d.functions)
        allowed = int(self.baseline * self.cap / 100.0)
        if sum(self.stats.values()) >= allowed:
            if not self.capped:
                print(f"  static discovery: cap reached ({self.cap:g} % of "
                      f"{self.baseline} functions)")
            self.capped = True
            return False
        return True

    def _add(self, addr: int, method: str) -> bool:
        if not self._budget():
            return False
        from . import config
        self.d._add_candidate(addr, config.CONFIDENCE_IMM_REF, method)
        self.stats[method] += 1
        return True

    def _refuse(self, why: str) -> None:
        self.refused[why] = self.refused.get(why, 0) + 1

    def summary(self) -> dict:
        return {"level": self.level, "strict": self.strict, "cap": self.cap, "capped": self.capped,
                "added": dict(self.stats), "refused": dict(sorted(self.refused.items())),
                # Of added["packed_table"]: the aliases a table's completion made.
                "packed_completed": self.packed_completed,
                # Functions that forward an argument of their own to a
                # callback-taking kernel call (a library's CreateThread).
                "callback_wrappers": len(self.callback_wrappers)}

    # -- shared tests -----------------------------------------------------

    def _code_ranges(self, sections):
        return [(s.virtual_addr, s.virtual_addr + s.virtual_size) for s in sections]

    def _prev_insn(self, addr: int):
        return self.engine._instruction_ending_at(addr)

    def _jump_table_words(self):
        """[(start, end)] of every measured jump table, sorted."""
        return sorted(self.engine.jump_tables.items())

    def _in_ranges(self, ranges, addr: int) -> bool:
        """Is `addr` in one of the sorted, disjoint [lo, hi) `ranges`?"""
        cache = self.__dict__.setdefault("_starts_cache", {})
        hit = cache.get(id(ranges))
        if hit is None or hit[0] is not ranges or len(hit[1]) != len(ranges):
            hit = cache[id(ranges)] = (ranges, [r[0] for r in ranges])   # holds the list: no id reuse
        starts = hit[1]
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and addr < ranges[i][1]

    def _is_padding_byte(self, addr: int) -> bool:
        b = self.image.read_bytes_at_va(addr, 1)
        return not b or b[0] in (0xCC, 0x90, 0x00)

    # -- table words (functions.py's _pass_data_ptr_targets) --------------

    def table_word_inside_ok(self, target: int) -> bool:
        """A data word naming an instruction inside a measured body: an
        alias entry only where the instruction before ends a path (ret, jmp,
        int3, nop), the shape of the merged constructor thunks that pass was
        written for. A word that names the `mov ebp, esp` after a `push ebp`,
        or the ret after a `pop`, is a decoy: on the decoy corpus every
        suffix-entry alias was one. Level 1 and up."""
        if self.level < STRONG or not self.strict:
            return True
        prev = self._prev_insn(target)
        if prev is not None and (prev.is_ret or prev.is_jump
                                 or prev.mnemonic in ("int3", "nop")):
            return True
        self._refuse("table word: inside a body, mid path")
        return False

    def table_word_gap_ok(self, target: int, end: int) -> bool:
        """A data word naming bytes in a gap, without a prologue: the body
        must not hold what data decodes into, and its stack must balance.
        Level 1 and up."""
        if self.level < STRONG or not self.strict or self.engine.probes_as_prologue(target):
            return True
        # Where a compiler starts a function: aligned, after padding, or
        # straight after a path's end. A word into the middle of a blob
        # names none of these three times out of four.
        prev = self._prev_insn(target)
        before = self.image.read_bytes_at_va(target - 3, 3) or b"\0\0\0"
        if not (target % 4 == 0 or before[-1] in (0xCC, 0x90, 0x00, 0xC3)
                or (before[0] == 0xC2 and before[2] == 0)
                or (prev is not None and (prev.is_ret or prev.is_jump))):
            self._refuse("table word: not where a function starts")
            return False
        decoded = self._cfg(target, end)
        if not decoded:
            return True                 # the probes that accepted it read no CFG: theirs to judge
        if not self._plausible_body(decoded, target, end):
            self._refuse("table word: not a compiled body")
            return False
        return True

    @staticmethod
    def _plausible_body(decoded, entry: int, end: int) -> bool:
        """No instruction data decodes into, every `ret N` pops whole
        arguments (a multiple of 4, at most 0x100), and the stack balances."""
        saved, clobbered = set(), set()
        for i in decoded:
            if i.mnemonic.split()[-1] in DATA_MNEMONICS:
                return False
            if i.is_ret and i.operands:
                n = i.operands[0].imm if i.operands[0].type == "imm" else None
                if n is None or n % 4 or n > 0x100:
                    return False
            if i.mnemonic == "push" and i.operands and i.operands[0].type == "reg":
                saved.add(i.operands[0].reg)
            elif _self_test(i):
                pass                    # `or bl, bl`: sets flags, keeps the value
            elif i.mnemonic != "pop":
                clobbered.update(_SAVED_ALIASES.get(r) for r in (i.regs_written or ())
                                 if r in _SAVED_ALIASES)
        # The callee-saved registers (ebx, esi, edi, ebp) a body writes it
        # saves first: the ABI every function reached through a pointer
        # keeps. Random bytes clobber them (`mov ebp, imm32`).
        if clobbered - saved:
            return False
        return stack_balanced(decoded, entry, entry, end)

    def _frame_prologue(self, addr: int) -> bool:
        """A prologue other than a lone register push: a frame (`push ebp;
        mov ebp, esp`, the hot-patch pad), a stack adjustment, the SEH or
        stack-probe helper call. A one-byte `push esi` is a byte data holds
        as often as code does."""
        if not self.engine.probes_as_prologue(addr):
            return False
        b = self.image.read_bytes_at_va(addr, 1) or b""
        return b[:1] not in (b"\x53", b"\x56", b"\x57")

    def weak_start_ok(self, addr: int, upper: int) -> bool:
        """functions.py's _pass_gap_prologues: a start right after a ret
        that opens with a lone register push or is a constant accessor must
        also be a compiled body (_plausible_body). Level 1 and up."""
        if self.level < STRONG or not self.strict or self._frame_prologue(addr):
            return True
        decoded = self._cfg(addr, upper)
        if decoded and self._plausible_body(decoded, addr, upper):
            return True
        self._refuse("gap prologue: not a compiled body")
        return False

    def _cfg(self, addr: int, hi: int):
        decoder = self.engine.__dict__.get("_probe_decoder")
        if decoder is None:
            from ..recomp.disasm import Disassembler
            decoder = self.engine._probe_decoder = Disassembler(cache_cfg_decodes=True)
        raw = self.image.read_bytes_at_va(addr, max(0, hi - addr))
        if not raw:
            return []
        return decoder.disassemble_cfg(raw, addr, hi, {addr},
                                       stop_mnemonics=("int3", "int", "ud2", "hlt"))

    # -- level 1: strong evidence ----------------------------------------

    def _pushed_args(self, call_insn, count: int) -> List:
        """The pushes in front of a call, nearest first, within its block."""
        pushes = []
        addr = call_insn.address
        for _ in range(48):
            prev = self._prev_insn(addr)
            if prev is None or prev.is_call or prev.is_ret or prev.is_branch:
                break
            if prev.mnemonic == "push":
                pushes.append(prev)
                if len(pushes) >= count:
                    break
            addr = prev.address
        return pushes

    def _pushed_value(self, push) -> Optional[int]:
        if push.imm_ref is not None:
            return push.imm_ref
        reg = push.op_str.strip()
        if reg not in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"):
            return None
        addr = push.address
        for _ in range(16):
            prev = self._prev_insn(addr)
            if prev is None or prev.is_call or prev.is_ret or prev.is_branch:
                return None
            ops = prev.op_str.replace(" ", "")
            if ops.startswith(reg + ","):
                if prev.mnemonic in ("mov", "lea") and prev.imm_ref is not None:
                    return prev.imm_ref
                if prev.mnemonic == "lea" and prev.memory_ref is not None:
                    return prev.memory_ref
                return None             # written some other way
            addr = prev.address
        return None

    # A function's own stack argument, as an operand: `dword ptr [ebp + 0x10]`.
    _ARG_OPERAND = re.compile(r"^dword ptr \[(ebp|esp) \+ (0x[0-9a-f]+|\d+)\]$")

    def _incoming_arg(self, insn, operand: str, start: int) -> Optional[int]:
        """The index (0 = the first) of the argument of the function at
        `start` that `operand` of `insn` reads, or None.

        `[ebp + 8 + 4k]` in a function that opens with `push ebp; mov ebp,
        esp`; `[esp + n]` when the stack depth at `insn` is known: only
        pushes, pops and `sub/add esp, imm` in a straight line from the
        start (no call, no branch: what a small forwarding wrapper is)."""
        m = self._ARG_OPERAND.match(operand.strip())
        if m is None:
            return None
        disp = int(m.group(2), 0)
        get = self.engine.instructions.get
        if m.group(1) == "ebp":
            first = get(start)
            second = get(first.end_address) if first is not None else None
            if (first is None or second is None or insn.address < second.end_address
                    or first.mnemonic != "push" or first.op_str.strip() != "ebp"
                    or second.mnemonic != "mov" or second.op_str.replace(" ", "") != "ebp,esp"):
                return None
            off = disp - 8
        else:
            depth, addr = 0, start
            for _ in range(64):
                if addr >= insn.address:
                    break
                cur = get(addr)
                if cur is None or cur.is_call or cur.is_ret or cur.is_branch:
                    return None
                ops = cur.op_str.replace(" ", "")
                if cur.mnemonic == "push":
                    depth += 4
                elif cur.mnemonic == "pop":
                    depth -= 4
                elif cur.mnemonic in ("sub", "add") and ops.startswith("esp,"):
                    try:
                        n = int(ops[4:], 0)
                    except ValueError:
                        return None
                    depth += n if cur.mnemonic == "sub" else -n
                elif ops.startswith("esp,") or cur.mnemonic in ("leave", "pushal", "popal",
                                                                 "pushfd", "popfd", "enter"):
                    return None
                addr = cur.end_address
            if addr != insn.address:
                return None
            off = disp - depth - 4
        return off // 4 if off >= 0 and off % 4 == 0 else None

    def _forwarded_arg(self, push, start: int) -> Optional[int]:
        """The index of the argument of the function at `start` that `push`
        hands on: `push dword ptr [ebp + n]`, or `push reg` after `mov reg,
        dword ptr [ebp + n]` (the esp forms too, _incoming_arg)."""
        operand = push.op_str.strip()
        if operand not in ("eax", "ebx", "ecx", "edx", "esi", "edi"):
            return self._incoming_arg(push, operand, start)
        addr = push.address
        for _ in range(16):
            prev = self._prev_insn(addr)
            if (prev is None or prev.address < start or prev.is_call or prev.is_ret
                    or prev.is_branch):
                return None
            ops = prev.op_str.replace(" ", "")
            if ops.startswith(operand + ","):
                if prev.mnemonic != "mov":
                    return None
                return self._incoming_arg(prev, prev.op_str.split(",", 1)[1], start)
            addr = prev.address
        return None

    def callback_args(self) -> Dict[int, str]:
        """The routines handed to the kernel calls of CALLBACK_ARGS, and to
        the functions that forward an argument of their own to one.

        An XDK title does not call PsCreateSystemThreadEx with its thread
        routine: its library's CreateThread does, with the library's startup
        as the routine and its own lpStartAddress argument as StartContext1
        (the CRT's _beginthreadex in turn calls CreateThread with its
        _threadstartex). So a function that pushes an argument of its own
        where a routine goes takes a routine there too, and what its callers
        push for it is the same evidence, through as many wrappers as there
        are. Without this the start address is an ordinary immediate, which
        the passes before take only in a gap with a closed body: a routine
        packed after a call that never returns, or one that ends in such a
        call (`_endthreadex`), did not exist, and its threads ended at once
        (GitHub issue 1: three workers, « [ICALL] Failed to resolve VA », a
        black screen once the title waited for them)."""
        out = {}
        if not self.callback_slots:
            return out
        bodies = sorted((f.start, f.end) for f in self.d.functions.values())
        starts = [b[0] for b in bodies]

        def owner(addr: int) -> Optional[int]:
            i = bisect.bisect_right(starts, addr) - 1
            return bodies[i][0] if i >= 0 and addr < bodies[i][1] else None

        kernel_calls, direct_calls = [], {}
        for insn in list(self.engine.instructions.values()):
            if not insn.is_call:
                continue
            if insn.memory_ref in self.callback_slots:
                kernel_calls.append(insn)
            elif insn.call_target is not None:
                direct_calls.setdefault(insn.call_target, []).append(insn)

        wrappers: Dict[int, Set[int]] = {}      # function start -> its arguments that are routines
        pending: List[Tuple[int, int]] = []

        def scan(insn, indices) -> None:
            pushes = self._pushed_args(insn, max(indices) + 1)
            start = owner(insn.address)
            for i in indices:
                if i >= len(pushes):
                    continue
                v = self._pushed_value(pushes[i])
                if v is not None:
                    out.setdefault(v, "callback_arg")
                    continue
                if start is None:
                    continue
                k = self._forwarded_arg(pushes[i], start)
                if k is not None and k not in wrappers.setdefault(start, set()):
                    wrappers[start].add(k)
                    pending.append((start, k))

        for insn in kernel_calls:
            scan(insn, self.callback_slots[insn.memory_ref])
        while pending:
            start, k = pending.pop()
            for insn in direct_calls.get(start, ()):
                scan(insn, (k,))
        self.callback_wrappers = {a: tuple(sorted(ks)) for a, ks in wrappers.items() if ks}
        return out

    def _vtable_words(self, vtable: int, code_ranges) -> List[int]:
        out = []
        for k in range(1024):
            w = self.image.read_u32_at_va(vtable + 4 * k)
            if w is None or not self._in_ranges(code_ranges, w):
                break
            out.append(w)
        return out

    def ctor_vtables(self, sections) -> Dict[int, str]:
        """`mov dword ptr [reg(+n)], imm32` with the immediate in a data
        section: a constructor installing a vtable. Its words that point into
        code are methods."""
        from .loader import DATA_SECTION_NAMES
        code_ranges = sorted(self._code_ranges(sections))
        out = {}
        seen = set()
        for insn in list(self.engine.instructions.values()):
            if insn.mnemonic != "mov" or insn.imm_ref is None:
                continue
            ops = insn.op_str.replace(" ", "")
            if not ops.startswith("dwordptr["):
                continue
            inside = ops[len("dwordptr["):ops.index("]")] if "]" in ops else ""
            reg, _, off = inside.partition("+")
            if reg not in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp") or "*" in inside:
                continue
            if off and (not off.startswith("0x") or int(off, 16) > 0x40):
                continue
            vt = insn.imm_ref
            sec = self.image.get_section_at_va(vt)
            if sec is None or (self._in_ranges(code_ranges, vt)
                               and sec.name not in DATA_SECTION_NAMES):
                continue
            if vt in seen:
                continue
            seen.add(vt)
            for m in self._vtable_words(vt, code_ranges):
                out.setdefault(m, "ctor_vtable")
        return out

    def rtti_vtables(self) -> Dict[int, str]:
        return {m: "rtti_vtable" for m in self.rtti_methods}

    def _seh_prologs(self) -> Set[int]:
        """Functions that build an exception frame for their caller: they
        write fs:[0] and are called right after two immediate pushes."""
        writers = set()
        for f in self.d.functions.values():
            for insn in self.engine.get_instructions_in_range(f.start, f.end):
                if insn.mnemonic == "mov" and insn.op_str.replace(" ", "").startswith("dwordptrfs:[0],"):
                    writers.add(f.start)
                    break
        out = set()
        for insn in self.engine.instructions.values():
            if insn.call_target in writers and insn.call_target not in out:
                p1 = self._prev_insn(insn.address)
                p2 = self._prev_insn(p1.address) if p1 is not None else None
                if (p1 is not None and p2 is not None and p1.mnemonic == "push"
                        and p2.mnemonic == "push" and p1.op_str.startswith("0x")
                        and p2.op_str.startswith("0x")):
                    out.add(insn.call_target)
        return out

    def seh_targets(self, sections) -> Tuple[Dict[int, str], Dict[int, Set[int]]]:
        """Funclets outside their function and away from it ({addr:
        "seh_funclet"}), and the handler targets in the gap right after a
        function, which extend it ({function start: {targets}})."""
        from ..recomp.disasm import Disassembler
        from ..recomp.seh_frames import analyze_seh_frame
        code_ranges = sorted(self._code_ranges(sections))
        prologs = self._seh_prologs()
        decoder = None
        funclets, resume = {}, {}
        starts = sorted(f.start for f in self.d.functions.values()
                        if f.start not in self.d._alias_entries)
        read_u32 = self.image.read_u32_at_va
        is_code = lambda va: self._in_ranges(code_ranges, va)
        for f in list(self.d.functions.values()):
            if f.start in prologs:
                continue
            insns = self.engine.get_instructions_in_range(f.start, f.end)
            if not any((i.mnemonic == "mov" and "fs:[0], esp" in i.op_str.replace("  ", " "))
                       or i.call_target in prologs for i in insns):
                continue
            raw = self.image.read_bytes_at_va(f.start, f.end - f.start)
            if not raw:
                continue
            if decoder is None:
                decoder = Disassembler()
            decoded = decoder.disassemble_function(raw, f.start, f.end)
            try:
                frame = analyze_seh_frame(decoded, f.start, f.end, read_u32, prologs, is_code)
            except Exception:
                continue
            if frame is None or not frame.outside:
                continue
            # analyze_seh_frame drops the outside targets from both lists;
            # read the frame again with no upper bound to tell which of them
            # were resume targets (the rest are funclets).
            outside_resume = set()
            probe = analyze_seh_frame(decoded, f.start, 0xFFFFFFFF, read_u32, prologs, is_code)
            if probe is not None:
                outside_resume = {t for t in probe.resume if t in frame.outside}
            # MSVC parks filters, __finally blocks and __except blocks after
            # the function's ret: those in the gap before the next function
            # extend the function (a funclet made a start there would cut
            # it short of the resume target behind). A funclet elsewhere is
            # a function of its own; a resume target elsewhere stays lost.
            k = bisect.bisect_right(starts, f.start)
            nxt = starts[k] if k < len(starts) else f.end
            for t in frame.outside:
                if f.start < t < nxt:
                    resume.setdefault(f.start, set()).add(t)
                elif t not in outside_resume:
                    funclets.setdefault(t, "seh_funclet")
                else:
                    self._refuse("seh: resume target in another function")
        return funclets, resume

    def _clean_split(self, addr: int, owner_start: int, owner_end: int) -> bool:
        """May the body [owner_start, owner_end) be cut at `addr`?

        The instruction before ends a path (ret, jmp, int3, or a call: the
        address has strong evidence, and a call that never returns is how a
        body runs over the next function), and no branch or switch arm of
        either part reaches into the other."""
        if addr not in self.engine.instructions:
            return False
        prev = self._prev_insn(addr)
        if prev is None or prev.address < owner_start:
            return False
        if not (prev.is_ret or prev.is_jump or prev.is_call or prev.mnemonic == "int3"
                or self._is_padding_byte(addr - 1)):
            return False
        for insn in self.engine.get_instructions_in_range(owner_start, owner_end):
            targets = []
            if insn.jump_target is not None:
                targets.append(insn.jump_target)
            if insn.jump_table is not None:
                targets.extend(self.engine.jump_table_entries(insn.jump_table))
            for t in targets:
                if insn.address < addr <= t < owner_end:
                    return False        # the first part reaches past the cut
                if owner_start <= t < addr <= insn.address:
                    return False        # the second part reaches back
        return (self.engine.probes_as_prologue(addr)
                or self.engine.probes_as_callback_body(
                    addr, owner_end, tail_targets=set(self.d.functions)))

    def place(self, evidence: Dict[int, str], sections) -> bool:
        """Make each strongly evidenced address a function start, when the
        gates let it (module docstring)."""
        code_ranges = sorted(self._code_ranges(sections))
        bodies = sorted((f.start, f.end) for f in self.d.functions.values())
        starts = [b[0] for b in bodies]
        tables = self._jump_table_words()
        arms = self.d._jump_table_arms()
        added = False
        for addr, method in sorted(evidence.items()):
            if (addr in self.d.functions or addr in self.d._candidates
                    or addr in self.d._alias_entries):
                continue
            if not self._in_ranges(code_ranges, addr):
                self._refuse("not in a code section")
                continue
            if self._in_ranges(tables, addr) or addr in arms:
                self._refuse("jump table or switch arm")
                continue
            if self.d._is_byte_table(addr) or self._is_padding_byte(addr):
                self._refuse("padding or byte table")
                continue
            i = bisect.bisect_right(starts, addr) - 1
            if i >= 0 and bodies[i][0] < addr < bodies[i][1]:
                if not self._clean_split(addr, bodies[i][0], bodies[i][1]):
                    self._refuse("inside a body, no clean split")
                    continue
            else:
                # In a gap: a run's seed gate (seed_from_log: reads as a
                # function body), on an instruction boundary or a prologue.
                # ...or right after padding, which the sweep swallowed into
                # an instruction that runs into the target (as
                # _realign_table_target allows).
                if (self.engine.instruction_covering(addr) is not None
                        and not self.engine.probes_as_prologue(addr)
                        and not self._is_padding_byte(addr - 1)):
                    self._refuse("inside an instruction")
                    continue
                if not self.engine.probes_as_function_body(addr):
                    self._refuse("does not read as a function body")
                    continue
                if addr not in self.engine.instructions:
                    if not self.engine.decode_at(addr, replace_overlaps=True):
                        self._refuse("does not decode")
                        continue
            if not self._add(addr, method):
                break
            added = True
        return added

    def run_strong(self, sections) -> Tuple[bool, Dict[int, Set[int]]]:
        """Level 1 over the current bodies; returns (added?, SEH resume
        targets by function)."""
        if self.level < STRONG:
            return False, {}
        if self.baseline is None:
            self.baseline = len(self.d.functions)
        evidence = {}
        # Strongest first: setdefault keeps the first kind named.
        for part in (self.callback_args(), self.rtti_vtables(), self.ctor_vtables(sections)):
            for a, k in part.items():
                evidence.setdefault(a, k)
        funclets, resume = self.seh_targets(sections)
        for a, k in funclets.items():
            evidence.setdefault(a, k)
        added = self.place(evidence, sections)
        return added, resume

    # -- level 2: gap code ------------------------------------------------

    def _skip_fill(self, addr: int, hi: int) -> int:
        data = self.image.read_bytes_at_va(addr, max(0, hi - addr)) or b""
        off = 0
        while off < len(data):
            matched = False
            for pad in self.d._PADDING + (b"\x00",):
                if data.startswith(pad, off):
                    off += len(pad)
                    matched = True
                    break
            if not matched:
                break
        return addr + off

    def _next_fill(self, addr: int, hi: int) -> int:
        """The first address past the next run of padding in [addr, hi)."""
        data = self.image.read_bytes_at_va(addr, max(0, hi - addr)) or b""
        for off in range(1, len(data)):
            if data[off] in (0xCC, 0x90) or (data[off] == 0 and data[off:off + 4] == b"\0\0\0\0"):
                return self._skip_fill(addr + off, hi)
        return hi

    def _next_candidate(self, addr: int, hi: int) -> Tuple[int, bool]:
        """Where to look next in a gap after `addr` was refused: past the
        next ret or jmp of its linear decode (code after an unconditional
        jump), or past the next padding. (address, right after a terminator)"""
        data = self.image.read_bytes_at_va(addr, max(0, min(hi - addr, 512))) or b""
        for n, insn in enumerate(self.engine._cs.disasm(data, addr)):
            if n >= 64 or insn.mnemonic in ("int3", "nop"):
                break
            if insn.mnemonic in ("ret", "retn", "jmp"):
                end = insn.address + insn.size
                nxt = self._skip_fill(end, hi)
                return nxt, nxt == end
        # The decode ran into bytes that do not decode (data): the next byte
        # past a `ret`, `ret N` or short `jmp` encoding, or past padding.
        fill = self._next_fill(addr, hi)
        for off in range(1, len(data)):
            p = addr + off
            if p >= fill:
                break
            if (data[off - 1] == 0xC3 or (off >= 3 and data[off - 3] == 0xC2 and data[off - 1] == 0)
                    or (off >= 2 and data[off - 2] == 0xEB)):
                return p, True
        return fill, False

    def gap_body(self, addr: int, hi: int, after_terminator: bool) -> Optional[int]:
        """The end of the function body at `addr` in the gap [addr, hi), or
        None when the gates refuse it (module docstring, level 2)."""
        first = self.image.read_bytes_at_va(addr, 1)
        if not first or first[0] < 0x0F:
            self._refuse("gap: byte table or fill")
            return None
        # After padding a compiler aligns: int3 or nop to 4 at least, zero
        # fill (a section's or a hand-laid one) to 16.
        before = self.image.read_bytes_at_va(addr - 1, 1) or b"\0"
        if not after_terminator and addr % (16 if before == b"\0" else 4):
            self._refuse("gap: not aligned after padding")
            return None
        # Packed straight after a terminator and off a dword boundary: only
        # a prologue says a compiler started a function there.
        if after_terminator and addr % 4 and not self.engine.probes_as_prologue(addr):
            self._refuse("gap: unaligned without a prologue")
            return None
        if not self.engine.probes_as_callback_body(addr, hi, addr,
                                                   tail_targets=set(self.d.functions)):
            self._refuse("gap: no closed body")
            return None
        decoded = self._cfg(addr, hi)
        if not decoded:
            return None
        # Three instructions, or a prologue, or a whole constant accessor
        # (`mov reg, imm ; ret`) on a 16-byte boundary.
        if (len(decoded) < 3 and not self.engine.probes_as_prologue(addr)
                and not (addr % 16 == 0 and self.engine.probes_as_constant_stub(addr))):
            self._refuse("gap: too short without a prologue")
            return None
        if not self._plausible_body(decoded, addr, hi):
            self._refuse("gap: not a compiled body")
            return None
        return max(i.end_address for i in decoded)

    def run_gaps(self, sections) -> bool:
        if self.level < FULL:
            return False
        if self.baseline is None:
            self.baseline = len(self.d.functions)
        # An alias in a gap runs on to the next start by design; what it
        # covers is its measured body (functions.py's _gap_alias_bodies).
        bodies = sorted((f.start, f.end) for f in self.d.functions.values()
                        if f.start not in self.d._alias_entries)
        covered = self.d._merged_ranges(bodies + self.d._gap_alias_bodies(bodies))
        tables = self._jump_table_words()
        arms = self.d._jump_table_arms()
        added = False
        for sec in sections:
            lo_sec = sec.virtual_addr
            hi_sec = sec.virtual_addr + min(sec.virtual_size, sec.raw_size or sec.virtual_size)
            gaps = []
            cur = lo_sec
            for lo, hi in covered:
                if hi <= lo_sec or lo >= hi_sec:
                    continue
                if lo > cur:
                    gaps.append((cur, lo))
                cur = max(cur, hi)
            if cur < hi_sec:
                gaps.append((cur, hi_sec))
            for lo, hi in gaps:
                prev = self._prev_insn(lo)
                ends_path = prev is not None and (
                    prev.is_ret or prev.is_jump or self.is_noreturn_call(prev))
                addr = self._skip_fill(lo, hi)
                after_terminator = ends_path and addr == lo
                for _ in range(64):
                    if addr >= hi:
                        break
                    if (self._in_ranges(tables, addr) or addr in arms
                            or addr in self.d._candidates or addr in self.d._alias_entries):
                        addr = self._next_fill(addr, hi)
                        after_terminator = False
                        continue
                    end = self.gap_body(addr, hi, after_terminator)
                    if end is None:
                        addr, after_terminator = self._next_candidate(addr, hi)
                        continue
                    if addr not in self.engine.instructions:
                        if not self.engine.decode_at(addr, replace_overlaps=True):
                            addr, after_terminator = self._next_candidate(addr, hi)
                            continue
                    if not self._add(addr, "gap_body"):
                        return added
                    added = True
                    nxt = self._skip_fill(end, hi)
                    last = self._prev_insn(end)
                    after_terminator = (nxt == end and last is not None
                                        and (last.is_ret or last.is_jump
                                             or self.is_noreturn_call(last)))
                    addr = nxt
        if added:
            print(f"  static discovery: {self.stats['gap_body']} function(s) in gaps")
        return added

    # -- level 2: packed pointer tables (XBA-140) -------------------------

    def _packed_chains(self, sections, code_ranges) -> List[List[Tuple[int, int]]]:
        """Runs of words in the data sections, at any byte offset, that
        name code: PACKED_MIN_RECORDS or more at one stride (4 to
        PACKED_MAX_STRIDE bytes: a record's size), at least one of them
        off a dword boundary (an aligned run is functions.py's table-word
        pass's). Each run is [(word va, value)], maximal at its stride."""
        from .loader import DATA_SECTION_NAMES
        code_names = {s.name for s in sections}
        chains = []
        if not code_ranges:
            return chains
        for sec in self.image.sections:
            if sec.name in code_names and sec.name not in DATA_SECTION_NAMES:
                continue                # code: its words are operands
            data = self.image.get_section_data(sec)
            if not data:
                continue
            data = bytes(data[:min(len(data), sec.raw_size or len(data),
                                   sec.virtual_size or len(data))])
            hits = {}
            lo_all, hi_all = code_ranges[0][0], code_ranges[-1][1]
            for phase in range(4):
                n = (len(data) - phase) // 4
                if n <= 0:
                    continue
                words = array.array(_U32, data[phase:phase + 4 * n])
                if sys.byteorder != "little":
                    words.byteswap()
                for k, v in enumerate(words):
                    if lo_all <= v < hi_all and self._in_ranges(code_ranges, v):
                        hits[phase + 4 * k] = v
            if len(hits) < PACKED_MIN_RECORDS:
                continue
            for off in sorted(hits):
                for stride in range(4, PACKED_MAX_STRIDE + 1):
                    if off - stride in hits:
                        continue        # not the head of the run at this stride
                    run = []
                    o = off
                    while o in hits:
                        run.append((sec.virtual_addr + o, hits[o]))
                        o += stride
                    if len(run) >= PACKED_MIN_RECORDS and any(va % 4 for va, _ in run):
                        chains.append(run)
        return chains

    def _packed_target(self, addr: int, bodies, starts, code_ranges, tables, arms):
        """How a packed table's word may enter: "known" (already an entry),
        "start", "alias", or None (refused, counted). Read-only.

        The gates of a run's observed seed (tools.seed_from_log: executable,
        reads as a function body) and of gap code: a closed CFG that is a
        compiled body (_plausible_body), with no prologue asked, since the
        run-time library's hand-written routines have none. And where a
        function starts: in a gap, an instruction boundary that is aligned or
        follows padding or a path's end; inside a measured body, an
        instruction after a path's end (a routine packed after its owner's
        ret): a clean split makes it a start, otherwise an alias, which
        keeps the owner whole."""
        d = self.d
        if addr in d.functions or addr in d._candidates or addr in d._alias_entries:
            return "known"
        if not self._in_ranges(code_ranges, addr):
            return self._refuse_packed("not in a code section")
        if self._in_ranges(tables, addr) or addr in arms:
            return self._refuse_packed("jump table or switch arm")
        if d._is_byte_table(addr) or self._is_padding_byte(addr):
            return self._refuse_packed("padding or byte table")
        prev = self._prev_insn(addr)
        ends_path = prev is not None and (prev.is_ret or prev.is_jump
                                          or prev.mnemonic in ("int3", "nop"))
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and bodies[i][0] < addr < bodies[i][1]:
            owner_start, owner_end = bodies[i]
            if addr not in self.engine.instructions:
                return self._refuse_packed("inside an instruction")
            if not ends_path:
                return self._refuse_packed("inside a body, mid path")
            upper, kind = owner_end, "alias"
            if self._clean_split(addr, owner_start, owner_end):
                kind = "start"
        else:
            if (self.engine.instruction_covering(addr) is not None
                    and not self._is_padding_byte(addr - 1)):
                return self._refuse_packed("inside an instruction")
            before = self.image.read_bytes_at_va(addr - 3, 3) or b"\0\0\0"
            if not (addr % 4 == 0 or ends_path or before[-1] in (0xCC, 0x90, 0x00, 0xC3)
                    or (before[0] == 0xC2 and before[2] == 0)):
                return self._refuse_packed("not where a function starts")
            k = bisect.bisect_right(starts, addr)
            sec = self.image.get_section_at_va(addr)
            upper = sec.virtual_addr + sec.virtual_size if sec is not None else addr
            if k < len(starts):
                upper = min(upper, starts[k])
            kind = "start"
        if not self.engine.probes_as_function_body(addr):
            return self._refuse_packed("does not read as a function body")
        if not self.engine.probes_as_callback_body(addr, upper, addr,
                                                   tail_targets=set(d.functions)):
            return self._refuse_packed("no closed body")
        decoded = self._cfg(addr, upper)
        if not decoded or not self._plausible_body(decoded, addr, upper):
            return self._refuse_packed("not a compiled body")
        return kind

    def _refuse_packed(self, why: str) -> None:
        self._refuse("packed table: " + why)
        return None

    @staticmethod
    def _clustered(targets) -> bool:
        """Do these code addresses lie within PACKED_NEAR of each other? A
        dispatch table's handlers do (the CRT's math helpers span a few KB);
        words read across a table of short strings (`"+3\\0\\0"` read one
        byte in) step through code 64 KB at a time."""
        return bool(targets) and max(targets) - min(targets) <= PACKED_NEAR

    @classmethod
    def _trim(cls, run, kinds):
        """The run without the refused words at either end that name code
        far from the run's other routines, when those are clustered (a
        refused word near them stays and counts against the run). A
        run is maximal at its stride, so it picks up the words next to a
        table that happen to read as code addresses: the record header in
        front of the first pointer (a flag byte and a u16 id + the pointer's
        low byte, megabytes away from the handlers). Otherwise the run as it
        is."""
        good = [t for (_, t), k in zip(run, kinds) if k is not None]
        if not cls._clustered(good):
            return run, kinds

        def far(i):
            return kinds[i] is None and all(abs(run[i][1] - g) > PACKED_NEAR for g in good)
        lo, hi = 0, len(run)
        while lo < hi and far(lo):
            lo += 1
        while hi > lo and far(hi - 1):
            hi -= 1
        return run[lo:hi], kinds[lo:hi]

    def _packed_completion(self, addr: int, bodies, starts, code_ranges, tables, arms):
        """The end of the alias entry an anchored table's word may make, or
        None (refused, counted). For a word of a table whose other entries
        are established (run_packed_tables): the hand-written CRT routines
        fall into each other, so an entry may follow an instruction that does
        not end a path (`fstp st0` / `ret`, or `mov [ebp-0x90], 8` /
        `call ...`), branch back into a neighbour's code, or open with a byte
        a compiler never starts with (`or cl, cl`). Asked: an instruction
        boundary, never inside an instruction (WP2's rule); not a jump table,
        a switch arm, padding or a measured byte table; a body that reads as
        a function body and closes, where an exit to code the sweep already
        decoded counts; a compiled body (_plausible_body). Always an alias:
        it keeps the body it sits in (or the gap) whole."""
        d = self.d
        if not self._in_ranges(code_ranges, addr):
            return self._refuse_completion("not in a code section")
        if self._in_ranges(tables, addr) or addr in arms:
            return self._refuse_completion("jump table or switch arm")
        if addr in self.engine.byte_tables or self._is_padding_byte(addr):
            return self._refuse_completion("padding or byte table")
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and bodies[i][0] < addr < bodies[i][1]:
            if addr not in self.engine.instructions:
                return self._refuse_completion("inside an instruction")
            upper = bodies[i][1]
        else:
            if addr not in self.engine.instructions and (
                    self.engine.instruction_covering(addr) is not None):
                return self._refuse_completion("inside an instruction")
            k = bisect.bisect_right(starts, addr)
            sec = self.image.get_section_at_va(addr)
            upper = sec.virtual_addr + sec.virtual_size if sec is not None else addr
            if k < len(starts):
                upper = min(upper, starts[k])
        if not self.engine.probes_as_function_body(addr):
            return self._refuse_completion("does not read as a function body")
        escapes: Set[int] = set()
        if not (self.engine.probes_as_callback_body(addr, upper, addr,
                                                    tail_targets=set(d.functions),
                                                    escapes=escapes)
                and all(e in self.engine.instructions and self._in_ranges(code_ranges, e)
                        for e in escapes)):
            return self._refuse_completion("no closed body")
        decoded = self._cfg(addr, upper)
        if not decoded or not self._plausible_body(decoded, addr, upper):
            return self._refuse_completion("not a compiled body")
        return upper

    def _refuse_completion(self, why: str) -> None:
        self._refuse("packed table completion: " + why)
        return None

    def _complete_tables(self, runs, taken, verdicts, bodies, starts, code_ranges,
                         tables, arms) -> Dict[int, int]:
        """run_packed_tables's completion step: {target: alias end}."""
        if not any(taken):
            return {}
        # One table: runs connected by a shared word (union-find on runs).
        parent = list(range(len(runs)))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        by_word: Dict[int, int] = {}
        for n, run in enumerate(runs):
            for va, _ in run:
                m = by_word.setdefault(va, n)
                if m != n:
                    parent[find(n)] = find(m)
        anchored = {find(n) for n, ok in enumerate(taken) if ok}
        established: Dict[int, Set[int]] = {}
        for n, run in enumerate(runs):
            root = find(n)
            if root in anchored:
                established.setdefault(root, set()).update(
                    t for _, t in run if verdicts.get(t) is not None)
        near_of = {root: sorted(ts) for root, ts in established.items()}
        out: Dict[int, int] = {}
        judged: Dict[int, Optional[int]] = {}
        for n, run in enumerate(runs):
            root = find(n)
            if root not in anchored:
                continue
            named = {t for _, t in run}
            est = {t for t in named if verdicts.get(t) is not None}
            if not est or 2 * len(est) < len(named) or not self._clustered(est):
                continue
            near = near_of[root]
            for t in sorted(named - est):
                j = bisect.bisect_left(near, t)
                if not any(0 <= k < len(near) and abs(near[k] - t) <= PACKED_NEAR
                           for k in (j - 1, j)):
                    continue            # a header byte's word, far from the table's code
                if t not in judged:
                    judged[t] = self._packed_completion(t, bodies, starts, code_ranges,
                                                        tables, arms)
                if judged[t] is not None:
                    out[t] = judged[t]
        return out

    def run_packed_tables(self, sections) -> bool:
        """Level 2: the function pointers of packed tables (XBA-140). A run
        (trimmed of refused words at its ends, _trim) is taken when
        PACKED_MIN_RECORDS of its words pass _packed_target, three quarters
        of them do, and they name two routines or more.

        Then the table is completed (WP5): runs that share a word with a
        taken run make one table (a record's pointers are a run at stride 4,
        each field a run at the record's stride). In a run of such a table
        where at least half the routines named are established (passed
        _packed_target, or were entries already), a refused word becomes an
        alias entry when it passes _packed_completion and names code within
        PACKED_NEAR bytes of an established entry of its table. That takes
        the CRT math table's fall-into entries with no seed to anchor it
        (the MSVC CRT's math table), and never a run no gate established: a run of
        decoys has no established word. Returns: anything added (starts to
        build, or aliases)."""
        if self.level < FULL:
            return False
        if self.baseline is None:
            self.baseline = len(self.d.functions)
        code_ranges = sorted(self._code_ranges(sections))
        bodies = sorted((f.start, f.end) for f in self.d.functions.values()
                        if f.start not in self.d._alias_entries)
        starts = [b[0] for b in bodies]
        tables = self._jump_table_words()
        arms = self.d._jump_table_arms()
        verdicts: Dict[int, Optional[str]] = {}
        take: Dict[int, str] = {}
        runs = self._packed_chains(sections, code_ranges)
        taken: List[bool] = []
        for run in runs:
            kinds = []
            for _, target in run:
                if target not in verdicts:
                    verdicts[target] = self._packed_target(target, bodies, starts,
                                                           code_ranges, tables, arms)
                kinds.append(verdicts[target])
            trun, tkinds = self._trim(run, kinds)
            good = [(t, k) for (_, t), k in zip(trun, tkinds) if k is not None]
            if (len(good) < PACKED_MIN_RECORDS or 4 * len(good) < 3 * len(trun)
                    or len({t for t, _ in good}) < 2):
                self._refuse("packed table: run refused")
                taken.append(False)
                continue
            taken.append(True)
            for t, k in good:
                if k != "known":
                    take.setdefault(t, k)
        completed = self._complete_tables(runs, taken, verdicts, bodies, starts,
                                          code_ranges, tables, arms)
        added = False
        for addr, kind in sorted(take.items()):
            if kind == "alias":
                if not self._budget():
                    break
                i = bisect.bisect_right(starts, addr) - 1
                self.d._alias_entries[addr] = bodies[i][1]
                self.stats["packed_table"] += 1
                added = True
                continue
            if addr not in self.engine.instructions:
                if not self.engine.decode_at(addr, replace_overlaps=True):
                    self._refuse_packed("does not decode")
                    continue
            if not self._add(addr, "packed_table"):
                break
            added = True
        n_completed = 0
        for addr, end in sorted(completed.items()):
            if addr in take or addr in self.d._alias_entries:
                continue
            if not self._budget():
                break
            if addr not in self.engine.instructions:
                if not self.engine.decode_at(addr, replace_overlaps=True):
                    self._refuse_completion("does not decode")
                    continue
            self.d._alias_entries[addr] = end
            self.stats["packed_table"] += 1
            n_completed += 1
            self.packed_completed += 1
            added = True
        if added:
            print(f"  static discovery: {self.stats['packed_table']} function(s) "
                  f"from packed tables ({n_completed} completing a table)")
        return added
