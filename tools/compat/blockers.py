"""The words a blocker uses, one place for every reader (wave 3).

tools.compat's classify() (the desktop run and, through tools/compat/device.py,
the device's library check), tools/compat/heal.py (the desktop self-heal loop)
and the app's SelfHeal.java (« needs a fix ») name the same discovery entries
(docs/technical/discovery.md); they say it with these words, so a blocker
reads the same in the matrix, the heal log and on the device. SelfHeal.java
mirrors these functions; SelfHealTest and test_blockers.py pin the same
strings.
"""


def more(n):
    """" (+2)" after the first of three, "" for one."""
    return " (+%d)" % (n - 1) if n > 1 else ""


def exception_words(entries):
    """{"t":"exception"} (XBA-129): a guest exception nothing handled -- a
    divide error that stops the title, a raise no handler took, a host fault
    on a guest address no handler took (XBA-132)."""
    e = entries[0]
    return "guest exception %s at %s: %s%s" % (e.get("code", "?"), e.get("va", "?"), e.get("why", "?"),
                                              more(len(entries)))


def stack_switch_words(entries):
    """{"t":"stack_switch"} (XBA-101, and SEH resumes refused across host stacks)."""
    e = entries[0]
    return "stack switch at %s to %s: %s%s" % (e.get("va", "?"), e.get("to", "?"), e.get("why", "?"),
                                               more(len(entries)))


def unimpl_words(entries):
    """{"t":"unimpl"}: an untranslated instruction reached."""
    e = entries[0]
    return "untranslated instruction: %s at %s%s" % (e.get("insn", "?"), e.get("va", "?"), more(len(entries)))


def smc_site(e):
    if e.get("kind") == "code_write":
        return "code_write %s (%s)" % (e.get("at") or e.get("va"), e.get("section", "?"))
    return "%s %s (%s, from %s)" % (e.get("kind", "smc"), e.get("va"), e.get("where", "?"), e.get("ret", "?"))


def smc_words(entries, limit=3):
    """{"t":"smc"} (XBA-118): code the title wrote or made at run time."""
    sites = [smc_site(e) for e in entries]
    if len(sites) > limit:
        sites = sites[:limit] + ["+%d more" % (len(sites) - limit)]
    return "code written or run at run time x%d: %s" % (len(entries), ", ".join(sites))


def stub_names(entries, limit=4):
    """["ordinal 136 (KeRemoveQueue)", ...]: each stub route once, in the order first called."""
    out = []
    for e in entries:
        if "ord" not in e:
            continue
        s = "ordinal %s" % e["ord"] + (" (%s)" % e["name"] if e.get("name") else "")
        if s not in out:
            out.append(s)
    if len(out) > limit:
        out = out[:limit] + ["+%d more" % (len(out) - limit)]
    return out


def stub_words(entries, limit=4):
    """{"t":"stub"} (XBA-115), or None without one."""
    names = stub_names(entries, limit)
    return "calls stub " + ", ".join(names) if names else None


def d3d_layout_words(entries, limit=3):
    """{"t":"d3d_layout"} (XBA-105): the XDK D3D layer met a D3D build its
    layout table does not know and used another build's value for a rule:
    « D3D layout guessed for D3D8LTCG 5933: fence_event, rpb_nodes », or None."""
    by = []
    for e in entries:
        key = "%s %s" % (e.get("lib", "?"), e.get("build", "?"))
        rule = e.get("rule", "?")
        for k, rules in by:
            if k == key:
                if rule not in rules:
                    rules.append(rule)
                break
        else:
            by.append((key, [rule]))
    if not by:
        return None
    parts = ["%s: %s" % (k, ", ".join(r[:limit] + (["+%d more" % (len(r) - limit)] if len(r) > limit else [])))
             for k, r in by]
    return "D3D layout guessed for " + "; ".join(parts)


def of_kind(discovered, kind):
    return [d for d in discovered or () if isinstance(d, dict) and d.get("t") == kind]
