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
      no longer swallowed by it;
    * callback arguments: the routine pushed for PsCreateSystemThread(Ex)
      (start routine and StartContext1), KeInitializeDpc, KeInitializeApc,
      NtQueueApcThread, KeInitializeInterrupt;
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

A decoy (data that looks like a pointer: floats, strings, colours, offsets
into the middle of code) must never become a function: see
test_static_discovery.py, which measures the false-positive rate on a decoy
corpus, and the lift growth.

Switches (docs/runtime/environment-variables.md):
  RECOMP_STATIC_DISCOVERY      0 off, 1 strong evidence only, 2 (default) all
  RECOMP_STATIC_DISCOVERY_CAP  at most this many percent more functions than
                               the passes before found (default: no cap)
tools.disasm --static-discovery / --discovery-cap override them.
"""

import bisect
import os
from typing import Dict, List, Optional, Set, Tuple

ENV_LEVEL = "RECOMP_STATIC_DISCOVERY"
ENV_CAP = "RECOMP_STATIC_DISCOVERY_CAP"
OFF, STRONG, FULL = 0, 1, 2

# Kernel calls that never return: a body ends at them.
NORETURN_IMPORTS = frozenset({
    "HalReturnToFirmware", "KeBugCheck", "KeBugCheckEx", "PsTerminateSystemThread",
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
METHODS = ("callback_arg", "ctor_vtable", "rtti_vtable", "seh_funclet", "gap_body")


def level_from_env(env=None) -> int:
    v = (env if env is not None else os.environ).get(ENV_LEVEL, "").strip()
    if v == "":
        return FULL
    try:
        return max(OFF, min(FULL, int(v)))
    except ValueError:
        return OFF if v.lower() in ("off", "no", "false") else FULL


def cap_from_env(env=None) -> Optional[float]:
    v = (env if env is not None else os.environ).get(ENV_CAP, "").strip()
    try:
        return float(v) if v else None
    except ValueError:
        return None


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
                 rtti_methods=()):
        self.d = detector
        self.engine = detector.engine
        self.image = detector.image
        self.level = level
        self.cap = cap
        self.rtti_methods = set(rtti_methods or ())
        self.stats: Dict[str, int] = {m: 0 for m in METHODS}
        self.refused: Dict[str, int] = {}
        self.baseline: Optional[int] = None
        self.capped = False
        self.noreturn_slots: Set[int] = set()
        self.callback_slots: Dict[int, Tuple[int, ...]] = {}
        if level >= STRONG:
            for ki in self.image.kernel_imports:
                if ki.name in NORETURN_IMPORTS:
                    self.noreturn_slots.add(ki.thunk_addr)
                if ki.name in CALLBACK_ARGS:
                    self.callback_slots[ki.thunk_addr] = CALLBACK_ARGS[ki.name]

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
        return {"level": self.level, "cap": self.cap, "capped": self.capped,
                "added": dict(self.stats), "refused": dict(sorted(self.refused.items()))}

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
        if self.level < STRONG:
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
        if self.level < STRONG or self.engine.probes_as_prologue(target):
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
        if self.level < STRONG or self._frame_prologue(addr):
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

    def callback_args(self) -> Dict[int, str]:
        out = {}
        if not self.callback_slots:
            return out
        for insn in list(self.engine.instructions.values()):
            if not insn.is_call or insn.memory_ref not in self.callback_slots:
                continue
            indices = self.callback_slots[insn.memory_ref]
            pushes = self._pushed_args(insn, max(indices) + 1)
            for i in indices:
                if i < len(pushes):
                    v = self._pushed_value(pushes[i])
                    if v is not None:
                        out.setdefault(v, "callback_arg")
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
        """Funclets outside their function ({addr: "seh_funclet"}) and resume
        targets past a function's end ({function start: {targets}})."""
        from ..recomp.disasm import Disassembler
        from ..recomp.seh_frames import analyze_seh_frame
        code_ranges = sorted(self._code_ranges(sections))
        prologs = self._seh_prologs()
        decoder = None
        funclets, resume = {}, {}
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
            for t in frame.outside:
                if t in outside_resume:
                    resume.setdefault(f.start, set()).add(t)
                else:
                    funclets.setdefault(t, "seh_funclet")
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
                    prev.is_ret or prev.is_jump
                    or (prev.is_call and prev.memory_ref in self.noreturn_slots))
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
                                        and (last.is_ret or last.is_jump))
                    addr = nxt
        if added:
            print(f"  static discovery: {self.stats['gap_body']} function(s) in gaps")
        return added
