"""
Main disassembly orchestrator.

Coordinates all analysis passes and produces the final output.
"""

import bisect
import time
from pathlib import Path
from typing import List, Optional

from . import config
from .loader import load_image, BinaryImage, SectionInfo, _find_analysis_json
from .engine import DisasmEngine
from .functions import FunctionDetector
from .xrefs import build_xrefs, XRefTracker
from .labels import (
    LabelManager, populate_kernel_labels, populate_entry_point,
    extract_strings, populate_string_labels,
)
from .output import OutputWriter, print_stats
from .cache import AnalysisCache


class Disassembler:
    """
    Top-level disassembly orchestrator.

    Usage:
        d = Disassembler("path/to/default.xbe")
        d.run()
    """

    def __init__(self, xbe_path: str,
                 analysis_json: Optional[str] = None,
                 output_dir: Optional[str] = None,
                 text_only: bool = False,
                 stats_only: bool = False,
                 verbose: bool = False,
                 force: bool = False,
                 extra_sections: Optional[list] = None,
                 seed_functions: Optional[list] = None,
                 observed_seeds: Optional[set] = None,
                 skip_library_immediates: bool = False,
                 static_discovery: Optional[int] = None,
                 discovery_cap: Optional[float] = None,
                 lean: bool = False):
        self.xbe_path = xbe_path
        self.lean = lean
        self.analysis_json = analysis_json
        self.output_dir = output_dir or config.DEFAULT_OUTPUT_DIR
        self.text_only = text_only
        self.stats_only = stats_only
        self.verbose = verbose
        self.force = force
        self.extra_sections = extra_sections or []
        self.seed_functions = seed_functions or []
        self.skip_library_immediates = skip_library_immediates
        # Static discovery (XBA-137, discovery.py): level and cap, from the
        # caller or RECOMP_STATIC_DISCOVERY / RECOMP_STATIC_DISCOVERY_CAP.
        from .discovery import cap_from_env, level_from_env
        self.static_discovery = (level_from_env() if static_discovery is None
                                 else static_discovery)
        self.discovery_cap = cap_from_env() if discovery_cap is None else discovery_cap
        # Seeds a run reported (see _load_seed_functions). Kept for the
        # report; they get no extra trust against a detected body (WP2): a
        # wild indirect call reports garbage that "decodes as a body" too.
        self.observed_seeds = observed_seeds or set()

        # Components (initialized during run)
        self.image: Optional[BinaryImage] = None
        self.engine: Optional[DisasmEngine] = None
        self.labels: Optional[LabelManager] = None
        self.xrefs: Optional[XRefTracker] = None
        self.func_detector: Optional[FunctionDetector] = None
        self.strings: List[dict] = []

    def _trust_mid_instruction_seed(self, addr: int) -> bool:
        """A seed inside an instruction of a detected body: keep it anyway?

        Only when it decodes as a prologue, which means the sweep is the one
        out of phase (default.xbe's push ebp at 0x00069538). An RTTI guess six
        bytes into a mov (HL2's 0x00202C2E) is not, and neither is an
        "observed" seed alone: Fable's discovery file held 0x001C120A, a
        garbage target of a wild indirect call two bytes into a `mov`, and
        trusting it cut sub_001C1190 before its pops and `ret 8`, so every
        call left the guest stack 20 bytes off (WP2). A seed in bytes no
        body claims (Halo's XPP init at 0x001CF6AC after a pointer table)
        does not come here: unclaimed bytes yield to any seed.
        """
        return self.engine.probes_as_prologue(addr)

    def run(self) -> bool:
        """
        Execute the full disassembly pipeline.

        Returns True on success.
        """
        t_start = time.time()

        # Check cache
        cache = AnalysisCache(self.output_dir)
        if not self.force:
            json_path = self._find_analysis_json()
            if json_path and cache.is_valid(self.xbe_path, json_path,
                                             self.text_only,
                                             self.extra_sections,
                                             self.seed_functions,
                                             self.skip_library_immediates,
                                             self._discovery_key()):
                last_time = cache.get_last_run_time()
                print(f"Cache hit - results unchanged (last run: "
                      f"{last_time:.1f}s)")
                if self.stats_only:
                    self._load_and_print_cached_stats()
                return True

        # Phase 1: Load
        if self.verbose:
            print("Phase 1: Loading binary image...")
        self.image = load_image(self.xbe_path, self.analysis_json)
        if self.verbose:
            print(f"  Loaded: {self.image.filepath}")
            print(f"  Base: 0x{self.image.base_address:08X}  "
                  f"Entry: 0x{self.image.entry_point:08X}")
            print(f"  Sections: {len(self.image.sections)}  "
                  f"Kernel imports: {len(self.image.kernel_imports)}")

        # Determine sections to analyze
        sections = self._get_target_sections()
        if self.verbose:
            print(f"  Target sections: {', '.join(s.name for s in sections)}")

        # Phase 2: Labels (pre-populate known symbols)
        if self.verbose:
            print("\nPhase 2: Populating labels...")
        self.labels = LabelManager()
        populate_entry_point(self.labels, self.image)
        ki_count = populate_kernel_labels(self.labels, self.image)
        if self.verbose:
            print(f"  Kernel import labels: {ki_count}")

        # Extract strings from .rdata
        self.strings = extract_strings(self.image)
        str_count = populate_string_labels(self.labels, self.strings)
        if self.verbose:
            print(f"  String labels: {str_count}")
            print(f"  Total labels: {self.labels.count()}")

        # Phase 3: Disassembly (linear sweep)
        if self.verbose:
            print("\nPhase 3: Linear sweep disassembly...")
        self.engine = DisasmEngine(self.image)

        total_insns = 0
        for sec in sections:
            if self.verbose:
                print(f"  {sec.name}: 0x{sec.virtual_addr:08X} "
                      f"({sec.virtual_size / 1024:.1f} KB)...", end="", flush=True)

            def progress(done, total):
                if self.verbose:
                    pct = done * 100 // total
                    print(f"\r  {sec.name}: 0x{sec.virtual_addr:08X} "
                          f"({sec.virtual_size / 1024:.1f} KB)... "
                          f"{pct}%", end="", flush=True)

            n = self.engine.linear_sweep(sec, progress_callback=progress)
            total_insns += n
            if self.verbose:
                print(f"\r  {sec.name}: {n:,d} instructions")

        if self.verbose:
            print(f"  Total: {total_insns:,d} instructions")

        # Realign after embedded switch tables before anything reads the
        # instruction stream. See DisasmEngine.resync_jump_tables().
        if self.static_discovery:
            n_reg = self.engine.find_register_dispatch()
            if n_reg:
                print(f"  {n_reg} switch dispatch(es) through a register")
        n_tables = self.engine.resync_jump_tables()
        if self.verbose:
            print(f"  Resynced past {n_tables:,d} embedded jump table(s)")
        if self.static_discovery:
            n_ext = self.engine.measure_external_tables(
                [(s.virtual_addr, s.virtual_addr + s.virtual_size) for s in sections])
            if n_ext:
                print(f"  {n_ext} switch table(s) outside the code they dispatch to")
            self.engine.decode_table_arms()

        # Phase 4: Cross-references
        if self.verbose:
            print("\nPhase 4: Building cross-references...")
        self.xrefs = build_xrefs(self.engine, self.image)
        if self.verbose:
            counts = self.xrefs.count_by_type()
            print(f"  Total xrefs: {self.xrefs.count():,d}")
            for xtype, count in sorted(counts.items()):
                print(f"    {xtype}: {count:,d}")

        # Phase 5: Function detection
        if self.verbose:
            print("\nPhase 5: Detecting functions...")
        # The seed check below measures bodies as before static discovery.
        self.func_detector = FunctionDetector(
            self.engine, self.image, self.xrefs, self.labels,
            skip_library_immediates=self.skip_library_immediates)
        if not self.seed_functions:
            self.func_detector = self._detector()

        # Measure bodies before accepting a seed inside a sweep instruction.
        if self.seed_functions:
            self.func_detector.detect_all(sections)
            realigned = 0
            mid_instruction = 0
            accepted = []
            refused = []
            inside = []
            # The body containing an address, by bisection (seed files run to
            # thousands of entries; get_function_at scans every function).
            pre = sorted(self.func_detector.functions.values(),
                         key=lambda f: f.start)
            pre_starts = [f.start for f in pre]
            pre_reach = []
            for f in pre:               # the furthest end so far: bodies may nest
                pre_reach.append(max(f.end, pre_reach[-1] if pre_reach else 0))

            def body_at(addr):
                j = bisect.bisect_right(pre_starts, addr) - 1
                while j >= 0 and pre_reach[j] > addr:
                    if addr < pre[j].end:
                        return pre[j]
                    j -= 1
                return None

            for addr in self.seed_functions:
                covering = self.engine.instruction_covering(addr)
                if covering is not None:
                    # Unclaimed sweep bytes may be replaced by the seed's
                    # decode; a detected body only yields to a prologue.
                    body = body_at(addr)
                    unclaimed = body is None
                    if not (unclaimed or self._trust_mid_instruction_seed(addr)):
                        mid_instruction += 1
                        refused.append((addr, covering, body))
                        continue
                    if not self.engine.decode_at(addr, replace_overlaps=unclaimed):
                        continue
                    realigned += 1
                elif addr not in self.engine.instructions:
                    if self.engine.decode_at(addr):
                        realigned += 1
                else:
                    # On an instruction boundary inside a detected body: an
                    # alias entry, never a start that splits the body (WP2).
                    body = body_at(addr)
                    if body is not None and body.start != addr:
                        inside.append(addr)
                        continue
                accepted.append(addr)

            # Rebuild from the corrected stream, without candidates or xrefs
            # left over from the discarded sweep instructions.
            self.xrefs = build_xrefs(self.engine, self.image)
            self.func_detector = self._detector()
            for addr in accepted:
                self.func_detector._add_candidate(addr, 0.95, "seed_vtable_thunk")
            self.func_detector.seed_aliases = set(inside)
            if inside:
                print(f"  {len(inside)} seed(s) inside a detected body kept as "
                      f"alias entries")
            if realigned:
                print(f"  Realigned {realigned} seeded address(es) the sweep "
                      f"stepped over")
            if mid_instruction:
                print(f"  Rejected {mid_instruction} seed(s) landing inside an "
                      f"instruction of a detected body:")
                for addr, covering, body in refused[:8]:
                    print(f"    0x{addr:08X}: inside `{covering.mnemonic} "
                          f"{covering.op_str}` at 0x{covering.address:08X} "
                          f"of {body.name}")
                if len(refused) > 8:
                    print(f"    ... and {len(refused) - 8} more")
            if self.verbose:
                print(f"  Seeded {len(self.seed_functions)} function addresses")

        num_funcs = self.func_detector.detect_all(sections)
        # Detection can realign the stream (tail targets); export its xrefs.
        self.xrefs = build_xrefs(self.engine, self.image)
        if self.verbose:
            summary = self.func_detector.summary()
            print(f"  Total functions: {num_funcs:,d}")
            for method, count in sorted(
                    summary["by_detection_method"].items()):
                print(f"    {method}: {count:,d}")

        # Phase 6: Recursive descent validation
        if self.verbose:
            print("\nPhase 6: Recursive descent validation...")
        start_addrs = [self.image.entry_point]
        start_addrs.extend(self.func_detector.functions.keys())
        section_bounds = [
            (s.virtual_addr, s.virtual_addr + s.virtual_size)
            for s in sections
        ]
        reachable = self.engine.recursive_descent(start_addrs, section_bounds)
        if self.verbose:
            coverage = len(reachable) / total_insns * 100 if total_insns else 0
            print(f"  Reachable instructions: {len(reachable):,d} "
                  f"({coverage:.1f}%)")

        elapsed = time.time() - t_start

        # Print stats
        if self.stats_only or self.verbose:
            print_stats(self.engine, self.func_detector, self.xrefs,
                        self.labels, self.strings, self.image)
            print(f"\n  Elapsed: {elapsed:.2f}s")

        # Phase 7: Output
        if not self.stats_only:
            if self.verbose:
                print(f"\nPhase 7: Writing output to {self.output_dir}/...")
            writer = OutputWriter(
                self.output_dir, self.engine, self.func_detector,
                self.xrefs, self.labels, self.image, self.strings,
                lean=self.lean)
            writer.write_all(sections_to_disasm=sections, verbose=self.verbose)

            # Save cache
            json_path = self._find_analysis_json()
            if json_path:
                cache.save(self.xbe_path, json_path, self.text_only, elapsed,
                           self.extra_sections, self.seed_functions,
                           self.skip_library_immediates, self._discovery_key())

            if self.verbose:
                print(f"\n  Output written to {self.output_dir}/")

        print(f"Done in {elapsed:.2f}s")
        return True

    def _discovery_key(self) -> str:
        from .discovery import strict_from_env
        return f"{self.static_discovery}:{self.discovery_cap}" + (":strict" if strict_from_env() else "")

    def _detector(self) -> FunctionDetector:
        """The function detector, with static discovery as configured."""
        return FunctionDetector(
            self.engine, self.image, self.xrefs, self.labels,
            skip_library_immediates=self.skip_library_immediates,
            static_discovery=self.static_discovery,
            discovery_cap=self.discovery_cap,
            rtti_methods=self._rtti_methods())

    def _rtti_methods(self) -> list:
        """Virtual methods MSVC RTTI names (tools.rtti), for discovery
        level 1 and up; none when the title has no RTTI."""
        if not self.static_discovery:
            return []
        cached = self.__dict__.get("_rtti_cache")
        if cached is None:
            try:
                from tools.rtti.rtti import recover, seeds
                cached = seeds(recover(self.xbe_path))
            except Exception as e:      # an XBE rtti cannot read: no RTTI evidence
                print(f"  RTTI not read: {e}")
                cached = []
            if cached:
                print(f"  RTTI: {len(cached)} virtual method(s)")
            self._rtti_cache = cached
        return cached

    def _get_target_sections(self) -> List[SectionInfo]:
        """Determine which sections to disassemble."""
        if self.text_only:
            sections = []
            text = self.image.get_section(".text")
            if text is None:
                raise ValueError("No .text section found")
            sections.append(text)
        else:
            # All executable sections with code
            sections = list(self.image.get_code_sections())

        # Add extra sections (for sections marked non-executable but containing code)
        if self.extra_sections:
            existing_names = {s.name for s in sections}
            for name in self.extra_sections:
                if name in existing_names:
                    continue
                sec = self.image.get_section(name)
                if sec is None:
                    print(f"  Warning: extra section '{name}' not found in XBE")
                elif sec.raw_size == 0:
                    print(f"  Warning: extra section '{name}' has no raw data")
                else:
                    print(f"  Adding extra section '{name}' (VA=0x{sec.virtual_addr:08X}, "
                          f"size={sec.raw_size} bytes, executable={sec.executable})")
                    sections.append(sec)

        return sections

    def _find_analysis_json(self) -> Optional[str]:
        """Find the analysis JSON file path."""
        if self.analysis_json:
            return self.analysis_json

        found = _find_analysis_json(Path(self.xbe_path))
        return str(found) if found else None

    def _load_and_print_cached_stats(self) -> None:
        """Load and print stats from cached summary.json."""
        summary_path = Path(self.output_dir) / "summary.json"
        if not summary_path.exists():
            print("  (no cached summary available)")
            return

        import json
        with open(summary_path) as f:
            summary = json.load(f)

        print(f"\n{'=' * 60}")
        print(f"  Cached Disassembly Summary")
        print(f"{'=' * 60}")
        print(f"  Binary: {summary.get('binary', 'N/A')}")
        print(f"  Instructions: {summary.get('total_instructions', 0):,d}")
        print(f"  Functions: {summary.get('total_functions', 0):,d}")
        print(f"  Cross-references: {summary.get('total_xrefs', 0):,d}")
        print(f"  Labels: {summary.get('total_labels', 0):,d}")
        print(f"  Strings: {summary.get('total_strings', 0):,d}")
        print(f"{'=' * 60}")
