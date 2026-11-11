"""The guest CPU block (RecompCpu, g_cpu) is defined twice: in
templates/runtime/recomp_types.h, which the generated code includes, and in
src/kernel/xbox_memory_layout.h, which the runtime includes. Both sides index
the same thread-local struct, so the two definitions must be identical; a
drifted copy compiles and links and then reads the wrong register.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[2]
_TYPES = _ROOT / "templates" / "runtime" / "recomp_types.h"
_LAYOUT = _ROOT / "src" / "kernel" / "xbox_memory_layout.h"


def _block(path, start, end):
    text = path.read_text(encoding="utf-8")
    m = re.search(re.escape(start) + r".*?" + re.escape(end), text, re.S)
    assert m, "%s: %s ... %s not found" % (path, start, end)
    return m.group(0)


class CpuStructTest(unittest.TestCase):
    def test_both_headers_define_the_same_block(self):
        for start, end in (("#ifndef RECOMP_CPU_DEFINED", "#endif /* RECOMP_CPU_DEFINED */"),
                           ("#ifndef RECOMP_XMM_DEFINED", "} RecompXmm;"),
                           ("#ifndef RECOMP_MMX_DEFINED", "} RecompMmx;")):
            self.assertEqual(_block(_TYPES, start, end), _block(_LAYOUT, start, end))

    def test_no_register_is_its_own_thread_local(self):
        # The point of the block: one TLS resolution, not one per register.
        regs = r"g_(e[a-d]x|e[sd]i|esp|ebp|seh_ebp|fs_base|df|eflags|mxcsr|fp_\w+|x?mm[0-7])\b"
        for path in (_TYPES, _LAYOUT, _ROOT / "src" / "kernel" / "xbox_memory_layout.c"):
            for line in path.read_text(encoding="utf-8").splitlines():
                if line.lstrip().startswith("*"):
                    continue
                self.assertIsNone(re.search(r"RECOMP_TLS\s+\w+\s+" + regs, line),
                                  "%s: %s" % (path.name, line))

    def test_generated_aliases_reach_the_block(self):
        # `ebx` is an alias for g_ebx in generated code; g_ebx must not come
        # back as g_cpu.<macro>. Compile a body that uses every alias.
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if not cc or sys.platform == "win32":
            self.skipTest("no POSIX C compiler")
        src = r'''
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u };
int main(void) {
    eax = 1; ecx = 2; edx = 3; esp = 4; ebx = 5; esi = 6; edi = 7;
    g_ebp = 8; g_seh_ebp = 9; g_fs_base = 10; g_df = 1; g_fp_top = 7;
    xmm0.u[0] = 11; xmm7.u[3] = 12; mm0.q = 13; mm7.q = 14;
    g_fp_stack[3] = 1.5; g_fp_control_word = 0x27F;
    if (g_cpu.r_eax != 1 || g_cpu.r_ebx != 5 || g_cpu.r_edi != 7 || g_cpu.r_esp != 4)
        return 1;
    if (g_cpu.r_xmm7.u[3] != 12 || g_cpu.r_mm7.q != 14 || g_cpu.r_fp_stack[3] != 1.5)
        return 2;
    if (&g_esp != &g_cpu.r_esp || recomp_pushfd() != (0x202u | 0x400u))
        return 3;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            exe = os.path.join(tmp, "t")
            with open(c, "w") as f:
                f.write(src)
            r = subprocess.run([cc, "-std=c11", "-O1", "-I", str(_TYPES.parent),
                                "-Wl,-undefined,dynamic_lookup" if sys.platform == "darwin"
                                else "-Wl,--unresolved-symbols=ignore-all",
                                c, "-o", exe, "-lm"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(subprocess.run([exe]).returncode, 0)


if __name__ == "__main__":
    unittest.main()
