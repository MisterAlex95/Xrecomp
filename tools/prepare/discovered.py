"""
Seeds from earlier runs (XBA-97, T5; the first step of XBA-65).

A run writes what it met and the analysis missed to <work>/discovered.jsonl
(src/kernel/recomp_discovery.h). Before analysing, prepare takes the new
indirect-call targets, thread routines and StartContext1 values through
tools.seed_from_log's gates (an executable section, decodes as a function
body: never seed data) and keeps the ones that pass in
<work>/discovered-seeds.json, handed to the disassembler with the profile's
seed files. The addresses that fail are remembered in
<work>/discovered-rejected.json so they are not tried again.

A new seed changes the prepare's stamp, so the next prepare analyses and
lifts again; nothing changes when the runs found nothing new.
"""

import json
import os

SEEDS = "discovered-seeds.json"
REJECTED = "discovered-rejected.json"
DISCOVERY = "discovered.jsonl"


def _read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


def refresh(work, xbe, functions_json, analysis_json=None, log=print, gate=None):
    """Bring <work>/discovered-seeds.json up to date; return its path when it
    holds seeds, else None. `gate` is tools.seed_from_log's (a parameter for
    the tests)."""
    seeds_path = os.path.join(work, SEEDS)
    seeds = _read_json(seeds_path, [])
    disc = os.path.join(work, DISCOVERY)
    if os.path.isfile(disc) and os.path.isfile(functions_json):
        from tools.seed_from_log.__main__ import scan_discovery
        with open(disc, encoding="utf-8", errors="replace") as f:
            candidates = scan_discovery(f.read())
        rejected = set(_read_json(os.path.join(work, REJECTED), []))
        known = {e["start"].lower() for e in seeds if isinstance(e, dict)} | {r.lower() for r in rejected}
        new = {va: why for va, why in candidates.items() if ("0x%08x" % va) not in known}
        if new:
            if gate is None:
                from tools.seed_from_log.__main__ import gate
            log("      %d new address(es) from earlier runs (discovered.jsonl)" % len(new))
            added = gate(xbe, functions_json, new, seeds, analysis_json, log=lambda m: log("    " + m))
            taken = {e["start"].lower() for e in added}
            rejected |= {"0x%08X" % va for va in new if ("0x%08x" % va) not in taken}
            _write_json(os.path.join(work, REJECTED), sorted(rejected))
            if added:
                seeds = seeds + added
                _write_json(seeds_path, seeds)
                log("      %d seed(s) added: the analysis runs again with them" % len(added))
    return seeds_path if seeds else None


def digest(path):
    """What the stamp keeps of the seeds file ("" without one)."""
    if not path:
        return ""
    import hashlib
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()
