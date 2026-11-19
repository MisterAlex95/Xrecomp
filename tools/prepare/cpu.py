"""
The CPU features a title is compiled for (XBA-313, docs/android/prepare.md
« CPU features »).

An Xbox pack is prepared on the device that runs it, so Prepare can compile
for that CPU instead of every ARMv8.0 core: today one feature, "lse", the
ARMv8.1 atomics (LDADD, CAS, SWP, ...). Lock-prefixed instructions then
compile to one atomic instruction instead of an exclusive load/store loop.
Every core of a phone made since about 2017 has them, but not every Android
device does (Cortex-A53/A57/A72 lack them), so the pack records what it was
compiled for (pack.json "cpu_features", a comma-separated string) and every
reader refuses it on a CPU without one of them: tools/prepare/pack.py check,
the native loader (src/platform/recomp_pack.c) and the app's Pack.check. A
pack moved to such a device is prepared again.

All of them read the same bit, the kernel's AT_HWCAP HWCAP_ATOMICS (what
getauxval(AT_HWCAP) returns), here from /proc/self/auxv: no ctypes, which the
device's Python may lack. Detection only means something where Prepare runs
on the CPU that will run the pack (an aarch64 Linux or Android); anywhere
else (a cross build on an x86-64 host, CI) the answer is no feature, the
ARMv8.0 baseline every pack had before.
"""

import os
import platform
import struct

# Linux arm64 uapi (asm/hwcap.h): AT_HWCAP bits.
AT_HWCAP = 16
HWCAP_ATOMICS = 1 << 8

# feature name -> the clang flags that compile for it. -march=armv8-a+lse only
# adds the atomics to the target's ARMv8.0 baseline (armv8.1-a would bring
# more than the pack records). Prepare keeps -mno-outline-atomics, so every
# atomic is inline either way.
FLAGS = {"lse": ["-march=armv8-a+lse"]}
KNOWN = tuple(sorted(FLAGS))
CHOICES = ("auto", "none") + KNOWN


def hwcap(auxv="/proc/self/auxv"):
    """AT_HWCAP of this process (64-bit little-endian auxv), or None."""
    try:
        with open(auxv, "rb") as f:
            data = f.read()
    except OSError:
        return None
    for off in range(0, len(data) - 15, 16):
        key, val = struct.unpack_from("<QQ", data, off)
        if key == 0:
            break
        if key == AT_HWCAP:
            return val
    return None


def is_arm64_linux(machine=None, system=None):
    machine = (machine or platform.machine()).lower()
    system = system or platform.system()
    return machine in ("aarch64", "arm64") and system in ("Linux", "Android")


def cpuinfo_atomics(cpuinfo="/proc/cpuinfo"):
    """Whether /proc/cpuinfo's Features line names "atomics" (the same HWCAP
    bit by name), or None when there is no such line."""
    try:
        with open(cpuinfo, errors="replace") as f:
            for line in f:
                if line.startswith("Features"):
                    return "atomics" in line.partition(":")[2].split()
    except OSError:
        pass
    return None


def detect(machine=None, system=None, auxv="/proc/self/auxv", cpuinfo="/proc/cpuinfo"):
    """The features of the CPU Prepare runs on, as a sorted tuple: () off an
    aarch64 Linux / Android, or when neither AT_HWCAP nor /proc/cpuinfo can
    be read."""
    if not is_arm64_linux(machine, system):
        return ()
    hw = hwcap(auxv)
    if hw is not None:
        return ("lse",) if hw & HWCAP_ATOMICS else ()
    return ("lse",) if cpuinfo_atomics(cpuinfo) else ()


def resolve(arg="auto", **kw):
    """--cpu-features: "auto" (detect()), "none", or a comma-separated list of
    KNOWN features. ValueError for an unknown one."""
    arg = (arg or "auto").strip().lower()
    if arg == "auto":
        return detect(**kw)
    if arg == "none":
        return ()
    return parse(arg, strict=True)


def parse(text, strict=False):
    """"lse" / "lse,..." -> sorted tuple. An unknown name raises ValueError
    with strict, else is kept (a reader then refuses the pack: it needs
    something this toolkit does not know)."""
    out = set()
    for part in (text or "").split(","):
        part = part.strip().lower()
        if not part:
            continue
        if strict and part not in FLAGS:
            raise ValueError("unknown CPU feature %r (known: %s)" % (part, ", ".join(KNOWN)))
        out.add(part)
    return tuple(sorted(out))


def text(features):
    """pack.json "cpu_features": the sorted names joined by commas ("" for none)."""
    return ",".join(sorted(set(features or ())))


def flags(features):
    out = []
    for f in sorted(set(features or ())):
        out += FLAGS.get(f, [])
    return out


def missing(needed, have=None):
    """The features of `needed` (a pack.json string or a tuple) that `have`
    (default: this CPU, detect()) lacks, sorted."""
    if isinstance(needed, str):
        needed = parse(needed)
    have = set(detect() if have is None else have)
    return tuple(sorted(f for f in set(needed) if f not in have))


def describe(features):
    return "LSE atomics inline" if "lse" in (features or ()) else "ARMv8.0 baseline"
