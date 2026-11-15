"""
CLI entry point for the disassembly tool.

Usage:
    py -3 -m tools.disasm <path_to_xbe> [options]

Examples:
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only --stats-only -v
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" -o output/
"""

import argparse
import sys

from .disasm import Disassembler
from .functions import OverlappingBodies


def main():
    parser = argparse.ArgumentParser(
        prog="tools.disasm",
        description="Xbox XBE Disassembly Tool - "
                    "Static analysis and function detection for Xbox executables",
    )

    parser.add_argument(
        "xbe_path",
        help="Path to the Xbox XBE executable file",
    )
    parser.add_argument(
        "-o", "--output",
        dest="output_dir",
        default=None,
        help="Output directory for JSON databases and ASM listings "
             "(default: tools/disasm/output/)",
    )
    parser.add_argument(
        "--analysis-json",
        default=None,
        help="Path to the XBE analysis JSON (auto-detected if not specified)",
    )
    parser.add_argument(
        "--text-only",
        action="store_true",
        help="Only disassemble the .text section (faster, ~2.73 MB)",
    )
    parser.add_argument(
        "--stats-only",
        action="store_true",
        help="Print statistics only, don't write output files",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output with progress information",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-analysis even if cache is valid",
    )
    parser.add_argument(
        "--extra-sections",
        default=None,
        help="Comma-separated list of additional section names to treat as code "
             "(for sections marked non-executable in the XBE but containing code, "
             "e.g., --extra-sections XIPS,DOLBY)",
    )
    parser.add_argument(
        "--seed-functions",
        action="append",
        default=None,
        metavar="JSON",
        help="JSON file with additional function entry points to seed the detector. "
             "Format: array of objects with 'start' field (hex address string). "
             "Use icall_targets.json from tools.recomp.icall_feedback, which holds "
             "indirect-branch targets the title was measured reaching. "
             "NOT identified_functions.json from func_id: that is inference, not "
             "measurement, and it carries addresses that sit at a valid instruction "
             "boundary *inside* an existing function. The mid-instruction guard below "
             "does not catch those, and each one clamps the end of the function it "
             "sits in -- on the Xbox dashboard it cut __heap_init short of its "
             "epilogue, so the CRT heap was never initialised and nothing said so. "
             "Repeatable: pass it once per file. Hand-maintained seed lists and "
             "machine-generated ones stay separate files rather than being merged "
             "into each other.",
    )

    parser.add_argument(
        "--skip-library-immediates",
        action="store_true",
        help="An immediate in a library section (D3D, DSOUND, XPP, ...) never "
             "starts a function in .text. For titles whose XDK code holds "
             "register constants equal to .text addresses (NFSU1). Off by "
             "default.",
    )

    parser.add_argument(
        "--static-discovery",
        type=int, choices=(0, 1, 2), default=None,
        help="Static discovery (XBA-137, tools/disasm/discovery.py): 0 off, "
             "1 strong evidence only (callback arguments of kernel calls, "
             "constructor and RTTI vtables, exception handlers, noreturn "
             "kernel calls, switch tables outside code), 2 also code in gaps "
             "that nothing references. Default: $RECOMP_STATIC_DISCOVERY, "
             "else 2.",
    )
    parser.add_argument(
        "--discovery-cap",
        type=float, default=None, metavar="PCT",
        help="Static discovery adds at most PCT %% more functions than the "
             "passes before it found (each costs Prepare time on a phone). "
             "Default: $RECOMP_STATIC_DISCOVERY_CAP, else no cap.",
    )

    parser.add_argument(
        "--lean",
        action="store_true",
        help="Write only what the pipeline reads: compact JSON, no ASM "
             "listings (tools.prepare --android: faster, and tens of MB less "
             "on a phone; XBA-209)",
    )

    args = parser.parse_args()

    try:
        # Derive the section layout from the XBE being analyzed.
        from . import config
        config.configure_from_xbe(args.xbe_path)

        extra = [s.strip() for s in args.extra_sections.split(",")] if args.extra_sections else []
        seed_funcs = []
        observed = set()
        for _seed_path in (args.seed_functions or []):
            _got = _load_seed_functions(_seed_path, observed)
            seed_funcs.extend(_got)
            if args.verbose:
                print(f"  Seed file {_seed_path}: {len(_got)} addresses")
        seed_funcs = sorted(set(seed_funcs))
        disassembler = Disassembler(
            xbe_path=args.xbe_path,
            analysis_json=args.analysis_json,
            output_dir=args.output_dir,
            text_only=args.text_only,
            stats_only=args.stats_only,
            verbose=args.verbose,
            force=args.force,
            extra_sections=extra,
            seed_functions=seed_funcs,
            observed_seeds=observed,
            skip_library_immediates=args.skip_library_immediates,
            static_discovery=args.static_discovery,
            discovery_cap=args.discovery_cap,
            lean=args.lean,
        )
        success = disassembler.run()
        sys.exit(0 if success else 1)

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except OverlappingBodies as e:
        # An analysis error, not a warning: lifted, the cut body would fall
        # off its end without its epilogue (WP2).
        print(f"Error: analysis invariant: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(2)


def _load_seed_functions(path, observed=None):
    """Load seed function addresses from a JSON file.

    Entries marked "observed" (tools.seed_from_log writes them: a run
    actually called or started a thread there) are also added to `observed`.
    Entries written by seed_from_log before it set the field say so in their
    note, and count too.
    """
    import json
    with open(path) as f:
        data = json.load(f)
    addrs = []
    for entry in data:
        if isinstance(entry, dict) and "start" in entry:
            addr = int(entry["start"], 16)
            addrs.append(addr)
            if observed is not None and (
                    entry.get("observed")
                    or "observed at runtime" in entry.get("note", "")):
                observed.add(addr)
        elif isinstance(entry, int):
            addrs.append(entry)
    return addrs


if __name__ == "__main__":
    main()
