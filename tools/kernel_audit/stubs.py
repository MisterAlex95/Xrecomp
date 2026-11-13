#!/usr/bin/env python3
"""
Kernel routes that are stubs, read from the code (XBA-115).

    python3 -m tools.kernel_audit.stubs            list routes by state
    python3 -m tools.kernel_audit.stubs --write    regenerate src/kernel/kernel_stub_table.c
                                                   and the coverage table in docs/pipeline/05-runtime.md
    python3 -m tools.kernel_audit.stubs --check    exit 1 when either is out of date

A route is an ordinal bridge_for_ordinal() hands a bridge_* function. Its
bridge is a *stub* when its body does nothing but discard its arguments and
set g_eax to a constant -- `(void)STACK_ARG(n);`, `g_eax = 0;` -- which is
what "About 87 of 366 routes only set a return value" (XBA-59) counted.

Some of those are complete as they are: doing nothing is what the console
does as far as anything here can observe (IRQL is not modelled, there is no
debugger, the disc tray has no lock). Those are listed in NOOP_OK with the
reason, and are not reported. Every other stub goes into the generated C
table, which kernel_thunk_dispatch consults on each call: the first call of
a stub (then the 10th, 100th ...) is logged and recorded in the discovery
file as {"t":"stub","ord":N,...}, which tools.compat names as a blocker.

The C table is generated rather than hand-kept, and keyed by ordinal, so
implementing a stub means rerunning --write; test_stubs.py fails until then.
"""

import argparse
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRIDGE_C = os.path.join(ROOT, "src", "kernel", "kernel_bridge.c")
TABLE_C = os.path.join(ROOT, "src", "kernel", "kernel_stub_table.c")
RUNTIME_MD = os.path.join(ROOT, "docs", "pipeline", "05-runtime.md")

sys.path.insert(0, ROOT)

# Stubs whose doing nothing is right here, with the reason. Keep the reason
# honest: "nobody calls it" is not one -- that is what the report is for.
NOOP_OK = {
    7: "DbgLoadImageSymbols: no debugger attached",
    10: "DbgPrompt: no debugger attached to answer (a zero-length reply)",
    11: "DbgUnLoadImageSymbols: no debugger attached",
    36: "FscInvalidateIdleBlocks: no file-system cache to trim",
    101: "KeEnterCriticalRegion: kernel APC disabling; APCs here run only at kernel calls",
    122: "KeLeaveCriticalRegion: see KeEnterCriticalRegion",
    103: "KeGetCurrentIrql: IRQL is read from KPCR.Irql by guest code; PASSIVE is the answer outside a raise",
    121: "KeIsExecutingDpc: DPCs run on their own thread, never inside the caller",
    365: "HalEnableSecureTrayEject: no tray lock to arm",
    43: "HalEnableSystemInterrupt: no interrupt controller mask is modelled; device models raise their "
        "lines regardless (HalDisableSystemInterrupt is the same no-op)",
    47: "HalRegisterShutdownNotification: a reboot here ends or replaces the process, and no device model "
        "(the NIC included, XBA-117) has DMA in flight for a notification to stop",
}

CATEGORIES = [
    ("Av", "AV / video mode"),
    ("Dbg", "Debugger"),
    ("Ex", "Executive (pool, locks, interlocked)"),
    ("Fsc", "File-system cache"),
    ("Hal", "HAL (SMBus, interrupts, firmware)"),
    ("Interlocked", "Interlocked / SList"),
    ("Io", "I/O manager"),
    ("Ke", "Kernel (threads, waits, timers, DPCs, APCs)"),
    ("Kf", "Kernel (IRQL fastcalls)"),
    ("Ki", "Kernel internals"),
    ("Mm", "Memory manager"),
    ("Nt", "Native API (files, events, memory)"),
    ("Ob", "Object manager"),
    ("Phy", "Network PHY"),
    ("Ps", "Process / thread"),
    ("Rtl", "Run-time library"),
    ("Xc", "Crypto"),
    ("Xe", "XBE sections"),
    ("", "Other (port I/O, ...)"),
]


def _names():
    from tools.xbe_parser.xbe_parser import KERNEL_EXPORTS
    return KERNEL_EXPORTS


def _strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def routes(src=None):
    """{ordinal: bridge function name} from bridge_for_ordinal."""
    src = src if src is not None else open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    m = re.search(r"static bridge_func_t bridge_for_ordinal\(ULONG ordinal\)\s*\{(.*?)\n\}", src, re.S)
    body = _strip_comments(m.group(1)) if m else ""
    return {int(o): f for o, f in re.findall(r"case\s+(\d+):\s*return\s+(bridge_\w+)\s*;", body)}


_ALLOWED = [
    re.compile(r"\(void\)\s*[^;]+$"),                                  # (void)STACK_ARG(0)
    re.compile(r"g_eax\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uUlL]*$"),            # g_eax = 0
    re.compile(r"g_eax\s*=\s*(\(\w+\)\s*)?STATUS_[A-Z_]+$"),            # g_eax = STATUS_SUCCESS
    re.compile(r"return$"),
]


def is_stub_body(body):
    stmts = [t.strip() for t in _strip_comments(body).replace("\n", " ").split(";")]
    stmts = [t for t in stmts if t]
    return bool(stmts) and all(any(p.match(t) for p in _ALLOWED) for t in stmts)


def bridge_bodies(src=None):
    """{bridge name: body text} for every `static void bridge_X(void) { ... }`."""
    src = src if src is not None else open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    out = {}
    for m in re.finditer(r"^static void (bridge_\w+)\(void\)\s*\n\{\n(.*?)^\}", src, re.S | re.M):
        out[m.group(1)] = m.group(2)
    return out


def classify(src=None):
    """(implemented, stubs, noop_ok) as sorted ordinal lists."""
    src = src if src is not None else open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    bodies = bridge_bodies(src)
    impl, stubs, ok = [], [], []
    for o, fn in sorted(routes(src).items()):
        body = bodies.get(fn)
        if body is None or not is_stub_body(body):
            impl.append(o)
        elif o in NOOP_OK:
            ok.append(o)
        else:
            stubs.append(o)
    return impl, stubs, ok


def render_table_c(stubs, names):
    lines = [
        "/* GENERATED by tools/kernel_audit/stubs.py --write -- do not edit by hand.",
        " *",
        " * Kernel routes whose bridge only discards its arguments and sets a return",
        " * value (XBA-115). kernel_thunk_dispatch reports the first call of each",
        " * (kernel_stub_report.c); tools/kernel_audit/stubs.py has the rules and",
        " * the stubs that are complete as they are. Implementing one: rerun --write. */",
        "#include \"kernel_stubs.h\"",
        "",
        "const KernelStubEntry g_kernel_stub_table[] = {",
    ]
    for o in stubs:
        lines.append("    { %3d, \"%s\" }," % (o, names.get(o, "ordinal_%d" % o)))
    lines += [
        "};",
        "",
        "const unsigned g_kernel_stub_count = sizeof g_kernel_stub_table / sizeof g_kernel_stub_table[0];",
        "",
    ]
    return "\n".join(lines)


def _category(name):
    best = ""
    for prefix, _ in CATEGORIES:
        if prefix and name.startswith(prefix) and len(prefix) > len(best):
            # "Ke" must not swallow "Kf"/"Ki"; longest prefix wins, and a
            # prefix must be followed by an upper-case letter or "_".
            rest = name[len(prefix):]
            if rest[:1].isupper() or rest[:1] == "_" or prefix == "Interlocked":
                best = prefix
    return best


BEGIN = "<!-- kernel-coverage:begin (tools/kernel_audit/stubs.py --write) -->"
END = "<!-- kernel-coverage:end -->"


def render_coverage_md(impl, stubs, ok, names):
    rows = []
    total = {"impl": 0, "stub": 0, "ok": 0}
    for prefix, label in CATEGORIES:
        c = {"impl": [], "stub": [], "ok": []}
        for key, lst in (("impl", impl), ("stub", stubs), ("ok", ok)):
            for o in lst:
                if _category(names.get(o, "")) == prefix:
                    c[key].append(o)
        n = sum(len(v) for v in c.values())
        if not n:
            continue
        for k in total:
            total[k] += len(c[k])
        stub_names = ", ".join("%d %s" % (o, names.get(o, "?")) for o in c["stub"]) or "-"
        rows.append("| %s | %d | %d | %d | %d | %s |" % (label, n, len(c["impl"]), len(c["ok"]),
                                                       len(c["stub"]), stub_names))
    out = [BEGIN,
           "| Category | Routes | Implemented | No-op by design | Stub | Stubs (ordinal, name) |",
           "|---|---:|---:|---:|---:|---|"]
    out += rows
    out.append("| **All** | **%d** | **%d** | **%d** | **%d** | |" % (
        sum(total.values()), total["impl"], total["ok"], total["stub"]))
    out.append(END)
    return "\n".join(out)


def update_md(text, table):
    if BEGIN in text and END in text:
        a = text.index(BEGIN)
        b = text.index(END) + len(END)
        return text[:a] + table + text[b:]
    raise SystemExit("%s: no %s marker" % (RUNTIME_MD, BEGIN))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.kernel_audit.stubs", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)

    names = _names()
    impl, stubs, ok = classify()
    table_c = render_table_c(stubs, names)
    if a.write or a.check:
        md_old = open(RUNTIME_MD, encoding="utf-8").read()
        md_new = update_md(md_old, render_coverage_md(impl, stubs, ok, names))

    if a.write:
        with open(TABLE_C, "w", encoding="utf-8", newline="\n") as f:
            f.write(table_c)
        with open(RUNTIME_MD, "w", encoding="utf-8", newline="\n") as f:
            f.write(md_new)
        print("%d routes: %d implemented, %d no-op by design, %d stubs" % (
            len(impl) + len(stubs) + len(ok), len(impl), len(ok), len(stubs)))
        return 0
    if a.check:
        bad = []
        cur = open(TABLE_C, encoding="utf-8").read() if os.path.exists(TABLE_C) else ""
        if cur != table_c:
            bad.append(os.path.relpath(TABLE_C, ROOT))
        if md_old != md_new:
            bad.append(os.path.relpath(RUNTIME_MD, ROOT))
        if bad:
            print("out of date: %s -- run python3 -m tools.kernel_audit.stubs --write" % ", ".join(bad))
            return 1
        return 0
    for label, lst in (("stub", stubs), ("no-op by design", ok)):
        for o in lst:
            print("%-16s %3d  %s" % (label, o, names.get(o, "?")))
    print("%d routes: %d implemented, %d no-op by design, %d stubs" % (
        len(impl) + len(stubs) + len(ok), len(impl), len(ok), len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
