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


def mode_suffix(env=None):
    """What the stamp keeps of the static discovery switches (XBA-137,
    tools/disasm/discovery.py): "" for the default (level 2, no cap), so a
    stamp made with the default stays valid; otherwise "|sd=<level>:<cap>",
    so flipping RECOMP_STATIC_DISCOVERY(_CAP) analyses again."""
    from tools.disasm.discovery import FULL, cap_from_env, level_from_env
    level, cap = level_from_env(env), cap_from_env(env)
    return "" if (level, cap) == (FULL, None) else "|sd=%d:%s" % (level, "" if cap is None else "%g" % cap)


def analysis_counts(an):
    """The analysis' function count and what static discovery added to it,
    per pass, from <an>/disasm/summary.json (XBA-137): pack.json's
    "analysis", for the owner's library report. None without a summary."""
    s = _read_json(os.path.join(an, "disasm", "summary.json"), None)
    if not isinstance(s, dict):
        return None
    out = {"functions": s.get("total_functions"), "by_method": s.get("functions_by_method", {})}
    d = s.get("discovery")
    if isinstance(d, dict):
        out["static_discovery"] = {"level": d.get("level"), "cap": d.get("cap"), "capped": d.get("capped"),
                                   "added": d.get("added", {})}
    return out


def counts_line(counts):
    """« 1234 functions, static discovery +12 (callback_arg 3, gap_body 9) »."""
    if not counts:
        return ""
    line = "%s functions" % counts.get("functions")
    sd = counts.get("static_discovery")
    if sd:
        added = {k: v for k, v in (sd.get("added") or {}).items() if v}
        if sd.get("level") == 0:
            line += ", static discovery off"
        else:
            line += ", static discovery +%d" % sum(added.values())
            if added:
                line += " (%s)" % ", ".join("%s %d" % kv for kv in sorted(added.items()))
            if sd.get("capped"):
                line += ", capped at %g %%" % sd["cap"]
    return line
