"""
Seeds from earlier runs (XBA-97, T5; the first step of XBA-65).

A run writes what it met and the analysis missed to <work>/discovered.jsonl
(src/kernel/recomp_discovery.h). Before analysing, prepare takes the new
indirect-call targets, thread routines and StartContext1 values through
tools.seed_from_log's gates (an executable section, decodes as a function
body: never seed data) and keeps the ones that pass in
<work>/discovered-seeds.json, handed to the disassembler with the profile's
seed files. The addresses a static gate refuses are listed in
<work>/discovered-rejected.json; the ones only a run that had gone wrong
reported (a wild call site, entries after the first sign of corruption:
seed_from_log.classify_discovery, profile-free WP1) in <work>/discovered-held.json,
until a healthy run reports them too.

A new seed, or a seed dropped, changes the prepare's stamp, so the next
prepare analyses and lifts again; nothing changes when the runs found
nothing new.
"""

import json
import os

SEEDS = "discovered-seeds.json"
REJECTED = "discovered-rejected.json"
HELD = "discovered-held.json"
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


def _addr_set(items):
    out = set()
    for v in items:
        try:
            out.add(int(v, 16))
        except (TypeError, ValueError):
            pass
    return out


def refresh(work, xbe, functions_json, analysis_json=None, log=print, checker=None):
    """Bring <work>/discovered-seeds.json up to date; return its path when it
    holds seeds, else None.

    Every prepare re-gates the whole of discovered.jsonl, the seeds already
    kept included (profile-free WP1): a seed an older toolkit accepted that today's
    gates refuse -- a wild call target a corrupted run recorded -- is dropped,
    said, and moved to discovered-rejected.json (or discovered-held.json), so
    a poisoned work folder heals without anyone deleting files. Only changes
    are printed. `checker` is a tools.seed_from_log SeedChecker (a parameter
    for the tests)."""
    seeds_path = os.path.join(work, SEEDS)
    seeds = [e for e in _read_json(seeds_path, []) if isinstance(e, dict) and "start" in e]
    disc = os.path.join(work, DISCOVERY)
    if not os.path.isfile(disc):
        return seeds_path if seeds else None
    have_functions = os.path.isfile(functions_json)
    old = _addr_set(e["start"] for e in seeds)
    if not have_functions and not old:
        return None             # no analysis to gate against yet: the next prepare takes them
    from tools.seed_from_log.__main__ import gate_discovery, scan_discovery, seed_entry
    with open(disc, encoding="utf-8", errors="replace") as f:
        text = f.read()
    rejected_path, held_path = os.path.join(work, REJECTED), os.path.join(work, HELD)
    was_rejected = _addr_set(_read_json(rejected_path, []))
    was_held = _addr_set(_read_json(held_path, []))
    candidates = scan_discovery(text)
    known = old | was_rejected | was_held      # once: the union per candidate was quadratic (20 s for 30,000)
    new = [va for va in candidates if va not in known]
    if checker is None:
        from tools.seed_from_log.__main__ import SeedChecker
        checker = SeedChecker(xbe, functions_json if have_functions else None, analysis_json,
                              untrusted=old | set(candidates))
    # Without an analysis (it was removed) nothing new is seeded; the seeds
    # kept are still re-gated with what can be checked.
    accepted, refused, held = gate_discovery(text, checker, only=None if have_functions else old)
    if new and have_functions:
        log("      %d new address(es) from earlier runs (discovered.jsonl)" % len(new))
    for va in sorted(refused):
        if va not in was_rejected and va not in old:
            log("        - %08X  %s" % (va, refused[va]))
    for va in sorted(held):
        if va not in was_held and va not in old:
            log("        ~ %08X  held: %s" % (va, held[va]))
    dropped = [va for va in sorted(old) if va not in accepted]
    for va in dropped:
        why = refused.get(va) or held.get(va) or "no longer in discovered.jsonl"
        log("        - %08X  dropped from %s: %s" % (va, SEEDS, why))
    added = [va for va in sorted(accepted) if va not in old]
    for va in added:
        log("        + %08X  %s  (%s)" % (va, accepted[va][1], accepted[va][0]))
    if have_functions:
        rejected_now, held_now = set(refused), set(held)
    else:
        rejected_now, held_now = was_rejected | set(refused), was_held | set(held)
    # A seed missing from discovered.jsonl (the file was cut) has no
    # evidence left: dropped, but neither refused nor held.
    if rejected_now != was_rejected:
        _write_json(rejected_path, ["0x%08X" % va for va in sorted(rejected_now)])
    if held_now != was_held and (held_now or os.path.isfile(held_path)):
        _write_json(held_path, ["0x%08X" % va for va in sorted(held_now)])
    if dropped or added:
        # The entries kept stay as they were (same order, same notes): the
        # digest in the stamp moves only when the set of seeds does.
        seeds = ([e for e in seeds if int(e["start"], 16) not in dropped]
                 + [seed_entry(va, accepted[va][0]) for va in added])
        if seeds:
            _write_json(seeds_path, seeds)
        elif os.path.isfile(seeds_path):
            os.remove(seeds_path)
        if dropped:
            log("      %d seed(s) dropped: earlier runs reported them, today's gates refuse them" % len(dropped))
        if added:
            log("      %d seed(s) added: the analysis runs again with them" % len(added))
    return seeds_path if seeds else None


def changed(work):
    """True when discovered.jsonl may give refresh() something to change:
    an address neither seeded, refused nor held yet, or a held one a healthy
    stretch of a run has since reported (a v2 runtime writes it again,
    untainted). For tools.prepare --check, which has no XBE at hand: only a
    v2 runtime's own taint is read (an older run's needs the XBE to tell a
    non-code target, and can hold no healthy repeat anyway: its runtime never
    wrote an address twice); the full gates run at the prepare."""
    disc = os.path.join(work, DISCOVERY)
    if not os.path.isfile(disc):
        return False
    from tools.seed_from_log.__main__ import classify_discovery
    with open(disc, encoding="utf-8", errors="replace") as f:
        cls = classify_discovery(f.read())
    seeds = _addr_set(e.get("start") for e in _read_json(os.path.join(work, SEEDS), []) if isinstance(e, dict))
    rejected = _addr_set(_read_json(os.path.join(work, REJECTED), []))
    held = _addr_set(_read_json(os.path.join(work, HELD), []))
    known = seeds | rejected | held
    return any(va not in known or (va in held and c["clean_v2"]) for va, c in cls.items())


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
    stamp made with the default stays valid; otherwise "|sd=<level>:<cap>"
    (and ":strict" with RECOMP_STATIC_DISCOVERY_STRICT), so flipping
    RECOMP_STATIC_DISCOVERY(_CAP, _STRICT) analyses again."""
    from tools.disasm.discovery import FULL, cap_from_env, level_from_env, strict_from_env
    level, cap, strict = level_from_env(env), cap_from_env(env), strict_from_env(env)
    if (level, cap, strict) == (FULL, None, False):
        return ""
    return "|sd=%d:%s%s" % (level, "" if cap is None else "%g" % cap, ":strict" if strict else "")


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
