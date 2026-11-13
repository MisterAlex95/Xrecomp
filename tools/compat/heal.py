"""
The self-healing loop on a desktop (XBA-129): run a title, let the next
prepare seed what the run found, run it again, until a round brings nothing.

    python -m tools.compat heal SOURCE [--max-idle 1] [--max-rounds 12] [run's options]

The same rules as the app (android/app/src/main/java/.../SelfHeal.java,
docs/android/prepare.md « Self-healing »); the app has no log of a normal
play run, so it decides from discovered.jsonl and the system's word for the
exit, where this has the compat runner's blocker:

  * A round is one prepare + one boot. The prepare reads discovered.jsonl
    (tools/prepare/discovered.py) and takes the new code addresses through
    seed_from_log's gates into discovered-seeds.json.
  * Another round when the run met code the analysis missed (icall, thread,
    thread_ctx entries past what the file held before the run), whether the
    run crashed or not (a skipped indirect call does not crash: the title
    goes on, maybe wrong).
  * No cap on rounds as such: the cap is on rounds without progress, a round
    whose prepare accepted no new seed, or whose run stopped on the same
    blocker as the round before. --max-idle of them in a row stop the loop
    (default 1: the first one). --max-rounds is only a safety net.
  * Never round again on what a prepare cannot fix: a run that stopped with
    a guest exception nothing handled, a stack switch the runtime could not
    carry or an untranslated instruction reached is « needs a fix », with
    that named. Calls to stub kernel routes are named too when nothing else
    is to blame.
"""

import json
import os
import re

from tools.compat import blockers

SEED_KINDS = ("icall", "thread", "thread_ctx")
# A prepare cannot fix these: the lifter, the kernel or the runtime must.
FATAL_KINDS = ("exception", "stack_switch", "unimpl", "smc")

DISCOVERY = "discovered.jsonl"
SEEDS = "discovered-seeds.json"


def entries_from(path, offset=0):
    """The whole JSON lines of `path` past byte `offset` (a torn last line is
    left for later); a file shorter than offset was replaced: all of it."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if offset > len(data):
        offset = 0
    data = data[offset:]
    end = data.rfind(b"\n")
    out = []
    for line in data[:end + 1].decode("utf-8", "replace").splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if isinstance(e, dict):
            out.append(e)
    return out


def finds(entries):
    """{"seeds": [va...], "exception": [entry...], "stack_switch": [...], "unimpl": [...], "smc": [...], "stub": [...]}."""
    f = {"seeds": [], "exception": [], "stack_switch": [], "unimpl": [], "smc": [], "stub": []}
    for e in entries:
        t = e.get("t")
        if t in SEED_KINDS and e.get("va"):
            if e["va"] not in f["seeds"]:
                f["seeds"].append(e["va"])
        elif t in f:
            f[t].append(e)
    return f


def named_blocker(f):
    """What a prepare cannot fix, in words (classify() and the app say the same:
    tools/compat/blockers.py), or None. Code written or run at run time
    (XBA-118) is one: the lifted C cannot run it, another Prepare neither."""
    if f["exception"]:
        return blockers.exception_words(f["exception"])
    if f["stack_switch"]:
        return blockers.stack_switch_words(f["stack_switch"])
    if f["unimpl"]:
        return blockers.unimpl_words(f["unimpl"])
    if f["smc"]:
        return blockers.smc_words(f["smc"])
    return None


def stub_words(f, limit=4):
    return blockers.stub_words(f["stub"], limit)


def _same(a, b):
    """Two blockers name the same stop (the run's seconds aside)."""
    norm = lambda s: re.sub(r"after \d+ s", "after N s", s or "")
    return bool(a) and norm(a) == norm(b)


def decide(f, blocker, prev_blocker, accepted, round_no, idle, max_idle=1, max_rounds=12):
    """After round `round_no` (1-based): ("again" | "stop", why, idle).

    f: finds() of the round's run; blocker: what stopped it (None: it ran its
    time); accepted: seeds the round's prepare took (None for round 1, which
    had nothing new to take); idle: rounds without progress so far."""
    if round_no > 1:
        why_idle = None
        if not accepted:
            why_idle = "the prepare accepted no new seed"
        elif blocker and _same(blocker, prev_blocker):
            why_idle = "the same blocker twice"
        if why_idle:
            idle += 1
            if idle >= max_idle:
                return "stop", "no progress: %s" % why_idle, idle
        else:
            idle = 0
    named = named_blocker(f)
    if blocker and named:
        return "stop", "needs a fix: " + named, idle
    if not f["seeds"]:
        if not blocker:
            return "stop", "nothing new found", idle
        stubs = stub_words(f)
        return "stop", "needs a fix: " + blocker + ("; " + stubs if stubs and stubs not in blocker else ""), idle
    if round_no >= max_rounds:
        return "stop", "safety cap: %d rounds" % max_rounds, idle
    return "again", "%d new code address(es)" % len(f["seeds"]), idle


def seed_count(work):
    try:
        with open(os.path.join(work, SEEDS), encoding="utf-8") as fh:
            return len(json.load(fh))
    except (OSError, ValueError, TypeError):
        return 0


def heal(source, a, run_one, out=None):
    """Rounds of run_one (tools.compat's prepare + boot) under decide().
    Returns the last round's result with "heal": {rounds, stop, history}."""
    name = os.path.basename(os.path.normpath(source))
    work = os.path.join(os.path.abspath(a.work_root), re.sub(r"[^A-Za-z0-9._-]", "_", name))
    disc = os.path.join(work, DISCOVERY)
    history, idle, prev_blocker, res = [], 0, None, None
    for round_no in range(1, a.max_rounds + 1):
        seeds_before = seed_count(work)
        marks = {}
        res = run_one(source, a, marks)
        # The boot's entries: what the file gained after its prepare read it.
        f = finds(entries_from(disc, marks.get("disc_offset", 0)))
        accepted = None if round_no == 1 else seed_count(work) - seeds_before
        blocker = res.get("blocker")
        action, why, idle = decide(f, blocker, prev_blocker, accepted, round_no, idle, a.max_idle, a.max_rounds)
        history.append({"round": round_no, "stage": res.get("stage"), "blocker": blocker,
                        "new_seeds": len(f["seeds"]), "accepted": accepted, "then": why,
                        "run_seeds": (res.get("runs") or [{}])[0].get("seeds")})
        if out:
            out("round %d: %s, %s -> %s" % (round_no, res.get("stage"), blocker or "no blocker", why))
        prev_blocker = blocker
        if action == "stop" or res.get("stage") == "none":
            break
    res["heal"] = {"rounds": len(history), "stop": history[-1]["then"], "history": history}
    return res
