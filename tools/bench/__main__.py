"""
Benchmark of the lifted code (XBA-37, XBA-38): synthetic x86 workloads of our
own (tools/bench/corpus.s), lifted with the real pipeline and built with the
flags the Android Prepare uses (tools/prepare/compile.py codegen_flags), as
an executable and as the Android layout (libtitle.so dlopened next to a
dlopened runtime library that owns the thread-local guest registers).

    python -m tools.bench lift OUT [--copies N]
    python -m tools.bench build OUT VARIANT [--cc CC] [--target T] [--sysroot S]
    python -m tools.bench run OUT VARIANT [--reps R] [--scale X] [--layout exe|so]
    python -m tools.bench sweep OUT VARIANT... [--cc CC] [--reps R]  -> Markdown table
    python -m tools.bench insns OUT VARIANT [--cc CC --target aarch64-linux-android30 --sysroot S]
    python -m tools.bench icount OUT VARIANT --target T --runner 'qemu-aarch64 -L ...' [--scale X]

`lift` needs capstone and binutils that assemble i386 (`as --32`, `ld -m
elf_i386`): xba-deps:latest has both. `--copies N` adds N renamed copies of
the corpus (never run) so that compile time and code size are measured on a
lift of title-like size; the benchmark always runs copy 0.

Variants (VARIANTS below) are extra flags on top of the Prepare's, or lifter
switches applied at lift time (-D...). A variant whose checksums differ from
the baseline's is reported as such: it changed what the code computes.

`insns` disassembles the hot functions of a build (objdump of libtitle.so)
and counts instructions, loads, stores and calls (TLS descriptor calls
apart): the arm64 numbers, since qemu timings say nothing about a phone.

`icount` counts the instructions each workload executes (XBA-238), under
the qemu of --runner with one instruction per translation block, less a
run of the same binary at scale 0 (start-up, image load). Dynamic counts
are what static counts and qemu timings are not: comparable across
builds of the hot loops. Slow (qemu logs every instruction): use
--scale 0.1. An arm64 build qemu-user can run is a glibc one, e.g. NDK
clang with --target aarch64-linux-gnu --sysroot <aarch64 glibc root>
(the build links with lld, as the Android one does).

Nothing here comes from a game: the corpus is hand-written.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import compile as pcc     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x10000
HDR = 0x1000                        # tools/xbe_ident/test_xbe_ident.py make_xbe: text at BASE + HDR
TEXT_VA = BASE + HDR
TITLE_ID = 0x58420001

# Workloads: (symbol in corpus.s, iterations at scale 1). Sized for ~10-50 ms
# each natively at -O2.
WORKLOADS = (
    ("w_int", 2000000),
    ("w_mem", 2000),
    ("w_rep", 4000),
    ("w_bytes", 400),
    ("w_x87", 600),
    ("w_sse", 1500),
    ("w_icall", 2000000),
    ("w_fib", 25),
    ("w_vcall", 2000000),
)
DATA_SYMS = ("matrix", "src_buf", "byte_buf", "vec_in")

# name -> extra compile flags (after the Prepare's own). `base` is the Prepare
# as it is. Lifter-side switches are macros the generated C or recomp_types.h
# reads (documented where they are defined).
VARIANTS = {
    "base": [],
    "O1": ["-O1"],
    "O3": ["-O3"],
    "Os": ["-Os"],
    "hidden": ["-fvisibility=hidden"],
    "nosi": ["-fno-semantic-interposition"],
    "noplt": ["-fno-plt"],
    "fp": ["-fno-omit-frame-pointer"],
    "nofp": ["-fomit-frame-pointer"],
    "sections": ["-ffunction-sections", "-fdata-sections"],
    "nomatherrno": ["-fno-math-errno"],
    "tlsdesc": ["-mtls-dialect=desc"],
    "tlstrad": ["-mtls-dialect=trad"],
    "ie": ["-ftls-model=initial-exec"],
    "ld": ["-ftls-model=local-dynamic"],
    "thinlto": ["-flto=thin"],
    "a715": ["-mcpu=cortex-a715"],          # the reference device's big cores (arm64 builds only)
    "cpuslot": ["-DRECOMP_CPU_SLOT=1"],
    "cpuslot0": ["-DRECOMP_CPU_SLOT=0"],
    "memalias": ["-DRECOMP_GUEST_MEM_NOALIAS=1"],
    "memalias0": ["-DRECOMP_GUEST_MEM_NOALIAS=0"],
    # The frame shadow stack compiled out (XBA-247, recomp_types.h): its cost, on one lift.
    "noframes": ["-DRECOMP_FRAME_SHADOW=0"],
}


def sh(args, cwd=None, env=None, check=True):
    r = subprocess.run(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if check and r.returncode != 0:
        raise SystemExit("$ %s\n%s" % (" ".join(args), r.stdout[-4000:]))
    return r


def variant_flags(name):
    """Extra flags of a variant name; '+' joins several (`O3+hidden`)."""
    out = []
    for part in name.split("+"):
        if part not in VARIANTS:
            raise SystemExit("unknown variant %r (known: %s)" % (part, ", ".join(sorted(VARIANTS))))
        out += VARIANTS[part]
    return out


# ── lift ────────────────────────────────────────────────────────

def corpus_text(copies):
    """corpus.s, plus `copies` renamed copies of its code (data once)."""
    src = open(os.path.join(HERE, "corpus.s")).read()
    head, data = src.split("# @data", 1)
    # Numeric local labels (1:, 2b, 3f) stay as they are: they are local already.
    labels = sorted({l for l in re.findall(r"^(\w+):", head, re.M) if not l.isdigit()} - {"_start"},
                    key=len, reverse=True)
    extra, calls = [], []
    for k in range(1, copies + 1):
        body = head.split(".text", 1)[1]
        body = re.sub(r"\b(%s)\b" % "|".join(map(re.escape, labels + ["_start"])),
                      lambda m: "%s_c%d" % (m.group(1), k), body)
        body = body.replace(".globl _start_c%d" % k, "")
        extra.append(body)
        calls.append("    call _start_c%d" % k)
    head = head.replace("# @copies: each copy's _start_<k> is called here",
                        "\n".join(calls))
    return head + "\n".join(extra) + "\n# @data" + data


def lift(out, copies=0, python=sys.executable, recomp_args=()):
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    asm = os.path.join(out, "corpus.s")
    with open(asm, "w") as f:
        f.write(corpus_text(copies))
    obj, elf, img = (os.path.join(out, n) for n in ("corpus.o", "corpus.elf", "image.bin"))
    sh(["as", "--32", "-o", obj, asm])
    sh(["ld", "-m", "elf_i386", "-Ttext=0x%x" % TEXT_VA, "-e", "_start", "-o", elf, obj])
    sh(["objcopy", "-O", "binary", "-j", ".text", elf, img])
    syms = {}
    for line in sh(["nm", elf]).stdout.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    from tools.xbe_ident.test_xbe_ident import make_xbe
    import pathlib
    xdir = os.path.join(out, "xbe")
    os.makedirs(xdir, exist_ok=True)
    xbe = make_xbe(pathlib.Path(xdir) / "default.xbe", libs=(), text=open(img, "rb").read(),
                   title_id=TITLE_ID, title="Bench")
    an = os.path.join(out, "analysis")
    gen = os.path.join(out, "gen")
    shutil.rmtree(an, ignore_errors=True)
    shutil.rmtree(gen, ignore_errors=True)
    os.makedirs(gen)
    d, fid = os.path.join(an, "disasm"), os.path.join(an, "func_id")
    log = os.path.join(out, "lift.log")
    t0 = time.time()
    steps = (
        ["tools.xbe_parser", xbe, "--json", os.path.join(xdir, "default_analysis.json")],
        ["tools.disasm", xbe, "-o", d, "--force"],
        ["tools.func_id", xbe, "--functions", os.path.join(d, "functions.json"),
         "--strings", os.path.join(d, "strings.json"), "--xrefs", os.path.join(d, "xrefs.json"), "-o", fid],
        ["tools.abi_analysis", xbe, "--disasm-dir", d, "--func-id-dir", fid, "--output-dir", os.path.join(an, "abi")],
        # tools/prepare/android.py Run.lift's arguments.
        ["tools.recomp", xbe, "--all", "--split-bytes", str(pcc.LIFT_CHUNK_BYTES), "--game-name", "Bench",
         "--disasm-dir", d, "--func-id-dir", fid, "--abi-dir", os.path.join(an, "abi"),
         "--gen-dir", gen, "-o", os.path.join(an, "recomp")] + list(recomp_args),
    )
    with open(log, "w") as lf:
        for args in steps:
            lf.write("$ %s\n" % " ".join(args))
            lf.flush()
            r = subprocess.run([python, "-m"] + args, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                raise SystemExit("%s failed, see %s" % (args[0], log))
    lift_s = time.time() - t0
    with open(os.path.join(out, "bench_syms.h"), "w") as f:
        f.write("/* generated by tools/bench lift */\n#define BENCH_IMAGE_VA 0x%xu\n" % TEXT_VA)
        for s in DATA_SYMS:
            f.write("#define BENCH_SYM_%s 0x%xu\n" % (s, syms[s]))
        f.write("#define BENCH_WORKLOADS \\\n")
        f.write(" \\\n".join('    { "%s", 0x%xu, %du },' % (n, syms[n], it) for n, it in WORKLOADS))
        f.write("\n")
    meta = {"copies": copies, "lift_s": round(lift_s, 2), "syms": {n: syms[n] for n, _ in WORKLOADS},
            "c_files": len([f for f in os.listdir(gen) if f.endswith(".c")]),
            "c_lines": sum(sum(1 for _ in open(os.path.join(gen, f))) for f in os.listdir(gen) if f.endswith(".c"))}
    with open(os.path.join(out, "bench.json"), "w") as f:
        json.dump(meta, f, indent=1)
    return meta


# ── build ───────────────────────────────────────────────────────

def title_sources(out):
    gen = os.path.join(out, "gen")
    return sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c")) + \
        [os.path.join(HERE, "bench_manual.c")]


def base_flags(opt="2"):
    """The Prepare's code generation flags (no target, sysroot or includes)."""
    flags = pcc.compile_flags(opt, ROOT, "/nonexistent", "/nonexistent")
    return [f for f in flags if not f.startswith(("--target=", "--sysroot=", "-resource-dir=", "-I"))]


def build(out, variant, cc="clang", target=None, sysroot=None, jobs=2):
    """Objects of the lifted C with the variant's flags, then the executable
    (exe/bench) and the shared layout (so/bench, so/libbenchrt.so,
    so/libtitle.so). Returns {compile_s, objects_bytes, text_bytes, ...}."""
    out = os.path.abspath(out)
    bdir = os.path.join(out, "build", variant + ("" if not target else "-" + target))
    shutil.rmtree(bdir, ignore_errors=True)
    os.makedirs(os.path.join(bdir, "obj"))
    os.makedirs(os.path.join(bdir, "exe"))
    os.makedirs(os.path.join(bdir, "so"))
    tflags = []
    if target:
        tflags.append("--target=" + target)
    if sysroot:
        tflags.append("--sysroot=" + sysroot)
    extra = variant_flags(variant)
    flags = tflags + base_flags() + extra + ["-I" + os.path.join(out, "gen"), "-I" + out]
    lto = any(f.startswith("-flto") for f in extra)
    srcs = title_sources(out)
    objs = []
    t0 = time.time()
    peak = 0
    from concurrent.futures import ThreadPoolExecutor
    procs = pcc.Procs()

    def one(src):
        o = os.path.join(bdir, "obj", os.path.basename(src)[:-2] + ".o")
        pk = []
        rc, text = procs.run([cc] + flags + ["-c", src, "-o", o], peak=pk)
        if rc != 0:
            raise SystemExit("compile %s failed:\n%s" % (src, text[-3000:]))
        return o, (pk[0] if pk else 0)
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for o, pk in ex.map(one, srcs):
            objs.append(o)
            peak = max(peak, pk)
    compile_s = time.time() - t0
    obj_bytes = sum(os.path.getsize(o) for o in objs)
    rt_flags = tflags + base_flags() + ["-I" + os.path.join(out, "gen"), "-I" + out]
    ldx = []
    # ThinLTO needs lld (its --thinlto-jobs); so does a cross build (no GNU ld
    # for the target: NDK clang with --target=aarch64-linux-gnu, see above).
    if (target and ("android" in target or target.startswith("aarch64"))) or lto:
        ldx = ["-fuse-ld=lld"]
    lto_link = (["-flto=thin", "-Wl,--thinlto-jobs=%d" % jobs] if lto else []) + \
        [f for f in extra if f.startswith("-O")]
    rt = os.path.join(HERE, "bench_rt.c")
    main = os.path.join(HERE, "bench_main.c")
    so = os.path.join(bdir, "so")
    t1 = time.time()
    sh([cc] + rt_flags + ldx + ["-shared", "-o", os.path.join(so, "libbenchrt.so"), rt, "-lm"])
    sh([cc] + tflags + ldx + lto_link + ["-shared", "-Wl,-Bsymbolic", "-Wl,--no-undefined",
                                         "-o", os.path.join(so, "libtitle.so")] + objs +
       ["-L" + so, "-lbenchrt", "-lm", "-Wl,-rpath,$ORIGIN"])
    link_s = time.time() - t1
    sh([cc] + tflags + ldx + ["-O2", "-DBENCH_SHARED", "-o", os.path.join(so, "bench"), main, "-ldl"])
    sh([cc] + rt_flags + ldx + lto_link + ["-o", os.path.join(bdir, "exe", "bench"), main, rt] + objs + ["-lm"])
    try:
        size = sh(["size", "-A", os.path.join(so, "libtitle.so")], check=False).stdout
    except OSError:                 # no binutils `size` (the arm64 cross image)
        size = ""
    m = re.search(r"^\.text\s+(\d+)", size, re.M)
    info = {"variant": variant, "flags": " ".join(extra), "compile_s": round(compile_s, 2),
            "link_s": round(link_s, 2), "peak_rss_mb": round(peak / 1024.0, 1), "objects_bytes": obj_bytes,
            "lib_bytes": os.path.getsize(os.path.join(so, "libtitle.so")),
            "text_bytes": int(m.group(1)) if m else 0}
    with open(os.path.join(bdir, "build.json"), "w") as f:
        json.dump(info, f, indent=1)
    return info


# ── run ─────────────────────────────────────────────────────────

def run(out, variant, layout="so", reps=5, scale=1.0, runner=(), target=None, only=None):
    out = os.path.abspath(out)
    bdir = os.path.join(out, "build", variant + ("" if not target else "-" + target))
    exe = os.path.join(bdir, layout, "bench")
    args = list(runner) + [exe, os.path.join(out, "image.bin"), str(reps), str(scale)] + ([only] if only else [])
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=1800)
    if r.returncode != 0:
        raise SystemExit("%s failed (%d): %s" % (exe, r.returncode, r.stderr[-2000:]))
    res = {}
    for line in r.stdout.splitlines():
        name, va, n, csum, ns = line.split()
        res[name] = {"va": va, "n": int(n), "checksum": csum, "ns": int(ns)}
    return res


# ── insns: static counts on the hot functions ───────────────────

def objdump_functions(lib, objdump="llvm-objdump"):
    """{symbol: [instruction text]} of every function in lib."""
    text = sh([objdump, "-d", "--no-show-raw-insn", lib]).stdout
    funcs, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        m = re.match(r"^\s+[0-9a-f]+:\s+(.*)$", line)
        if m and cur:
            funcs[cur].append(m.group(1).split("//")[0].strip())
    return funcs


def classify(insns, arch):
    c = {"insns": len(insns), "loads": 0, "stores": 0, "calls": 0, "tls_calls": 0, "branches": 0}
    for i, ins in enumerate(insns):
        op = ins.split()[0] if ins.split() else ""
        if arch == "aarch64":
            if op.startswith("ld") and not op.startswith("ldaxr"):
                c["loads"] += 1
            elif op.startswith("st"):
                c["stores"] += 1
            if op in ("bl", "blr"):
                prev = " ".join(insns[max(0, i - 4):i])
                if op == "blr" and ("tlsdesc" in prev or ":tlsdesc" in ins):
                    c["tls_calls"] += 1
                else:
                    c["calls"] += 1
            if op.startswith("b.") or op in ("cbz", "cbnz", "tbz", "tbnz", "b"):
                c["branches"] += 1
        else:
            if op.startswith("call"):
                if "__tls_get_addr" in ins or "tlsdesc" in ins:
                    c["tls_calls"] += 1
                else:
                    c["calls"] += 1
            if op.startswith("j"):
                c["branches"] += 1
            if "(" in ins:
                ops = ins.split(None, 1)[1] if len(ins.split(None, 1)) > 1 else ""
                last = ops.rsplit(",", 1)[-1]
                if "(" in last and op.startswith("mov"):
                    c["stores"] += 1
                elif "(" in ops and not op.startswith("lea"):
                    c["loads"] += 1
    return c


def icount(out, variant, runner, layout="exe", scale=0.1, target=None, only=None):
    """{workload: instructions executed} (see the module docstring)."""
    out = os.path.abspath(out)
    bdir = os.path.join(out, "build", variant + ("" if not target else "-" + target))
    exe = os.path.join(bdir, layout, "bench")
    trace = ["-one-insn-per-tb", "-d", "nochain,exec", "-D", "/dev/stdout"]

    def count(w, sc):
        args = list(runner[:1]) + trace + list(runner[1:]) + \
            [exe, os.path.join(out, "image.bin"), "1", str(sc), w]
        p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        n = sum(1 for line in p.stdout if line.startswith(b"Trace"))
        if p.wait() != 0:
            raise SystemExit("%s failed (%d)" % (" ".join(args), p.returncode))
        return n
    res = {}
    for w, _ in WORKLOADS:
        if only and w != only:
            continue
        res[w] = count(w, scale) - count(w, 0)
    return res


def insns(out, variant, target=None, objdump="llvm-objdump"):
    out = os.path.abspath(out)
    bdir = os.path.join(out, "build", variant + ("" if not target else "-" + target))
    lib = os.path.join(bdir, "so", "libtitle.so")
    arch = "aarch64" if target and "aarch64" in target else "x86_64"
    meta = json.load(open(os.path.join(out, "bench.json")))
    funcs = objdump_functions(lib, objdump)
    res = {}
    for name, va in meta["syms"].items():
        sym = "sub_%08X" % va
        if sym in funcs:
            res[name] = classify(funcs[sym], arch)
    res["_all"] = classify([i for f in funcs.values() for i in f], arch)
    return res


# ── CLI ─────────────────────────────────────────────────────────

def table(rows, cols):
    lines = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        lines.append("| " + " | ".join(str(r.get(c, "")) for c in cols) + " |")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m tools.bench", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("lift")
    p.add_argument("out")
    p.add_argument("--copies", type=int, default=0)
    p.add_argument("--recomp-arg", action="append", default=[])
    for name in ("build", "run", "sweep", "insns", "icount"):
        p = sub.add_parser(name)
        p.add_argument("out")
        p.add_argument("variants", nargs="+")
        p.add_argument("--cc", default=os.environ.get("CC", "clang"))
        p.add_argument("--target")
        p.add_argument("--sysroot")
        p.add_argument("--jobs", type=int, default=2)
        p.add_argument("--reps", type=int, default=5)
        p.add_argument("--scale", type=float, default=1.0)
        p.add_argument("--layout", default="so", choices=("so", "exe"))
        p.add_argument("--runner", default="", help="e.g. 'qemu-aarch64 -L /usr/aarch64-linux-gnu'")
        p.add_argument("--objdump", default="llvm-objdump")
        p.add_argument("--only")
        p.add_argument("--json", help="also write the results here")
    a = ap.parse_args(argv)
    if a.cmd == "lift":
        print(json.dumps(lift(a.out, a.copies, recomp_args=a.recomp_arg)))
        return 0
    results = []
    for v in a.variants:
        row = {"variant": v}
        if a.cmd in ("build", "sweep"):
            row.update(build(a.out, v, a.cc, a.target, a.sysroot, a.jobs))
        if a.cmd in ("run", "sweep"):
            for layout in (("so", "exe") if a.cmd == "sweep" else (a.layout,)):
                r = run(a.out, v, layout, a.reps, a.scale, a.runner.split(), a.target, a.only)
                row[layout] = r
        if a.cmd == "insns":
            row["insns"] = insns(a.out, v, a.target, a.objdump)
        if a.cmd == "icount":
            if not a.runner:
                raise SystemExit("icount needs --runner (a qemu-user)")
            row["icount"] = icount(a.out, v, a.runner.split(), a.layout,
                                   a.scale if a.scale != 1.0 else 0.1, a.target, a.only)
        results.append(row)
        print(json.dumps(row), flush=True)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    if a.cmd == "sweep":
        base = results[0]
        rows = []
        for r in results:
            row = {"variant": r["variant"], "compile s": r["compile_s"], ".text": r["text_bytes"]}
            for layout in ("so", "exe"):
                for w, _ in WORKLOADS:
                    t, b = r[layout][w], base[layout][w]
                    mark = "" if t["checksum"] == b["checksum"] else " (checksum differs)"
                    row["%s %s" % (layout, w)] = "%.2f%s" % (t["ns"] / 1e6, mark) if r is base else \
                        "%+.0f%%%s" % (100.0 * (t["ns"] - b["ns"]) / b["ns"], mark)
            rows.append(row)
        cols = ["variant", "compile s", ".text"] + ["so " + w for w, _ in WORKLOADS] + ["exe " + w for w, _ in WORKLOADS]
        print(table(rows, cols))
    return 0


if __name__ == "__main__":
    sys.exit(main())
