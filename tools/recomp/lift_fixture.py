"""Real bytes through the translator, the emitted C through gcc at -O2.

A test helper. Generated titles are built at -O2, so a lift that is only
right at -O0 (signed overflow, a shift by the width, a value re-read after it
changed) is wrong where it matters. These helpers take the shortest honest
route to that build: a hex string is installed as a one-section image,
translated by FunctionTranslator exactly as a title's function would be, and
compiled against the real templates/runtime/recomp_types.h with a test's own
main().

Guest memory is a host buffer: main() points g_xbox_mem_offset at `g_ram`
(64 KiB), so guest address 0x1000 is g_ram[0x1000]. esp starts inside it.
"""
import os
import subprocess
import tempfile

from . import config
from .c_fixture import compiler, is_msvc
from .translator import FunctionTranslator

BASE = 0x10000
RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "templates", "runtime")

PRELUDE = r"""
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u };
static uint8_t g_ram[0x10000] __attribute__((aligned(16)));
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; }
static void guest_reset(void)
{
    memset(g_ram, 0, sizeof g_ram);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_ram;
    esp = 0x8000u;
}
"""


def translate(hexcode, name="lifted", base=BASE):
    """The C of one function made of `hexcode`, renamed to `name`."""
    image = bytes.fromhex(hexcode.replace(" ", ""))
    config._install(
        [config.Section(".text", base, len(image), 0, len(image), True)],
        entry_point=base, kernel_thunk_addr=base, origin="lift-fixture")
    db = {base: {"start": f"0x{base:08X}", "end": base + len(image),
                 "_addr": base, "size": len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    return code.replace(f"sub_{base:08X}", name)


def gnu_cc():
    """A GNU-dialect C compiler, or None (MSVC cannot take the prelude)."""
    cc = os.environ.get("CC") or compiler()
    return None if not cc or is_msvc(cc) else cc


def build_and_run(functions, main_body, opt="-O2", extra_flags=()):
    """Compile the functions plus `int main(void) { main_body }` and run it.

    Returns (returncode, stdout + stderr). A failed build raises
    AssertionError with the compiler's output.
    """
    cc = gnu_cc()
    src = (PRELUDE + "\n".join(functions)
           + "\nint main(void)\n{\n" + main_body + "\n}\n")
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        with open(c, "w") as f:
            f.write(src)
        built = subprocess.run(
            [cc, "-std=gnu11", opt, "-w", *extra_flags, "-I", RUNTIME, c,
             "-o", exe, "-lm", "-Wl,--unresolved-symbols=ignore-all"],
            capture_output=True, text=True)
        assert built.returncode == 0, built.stderr[-4000:] + "\n" + src
        run = subprocess.run([exe], capture_output=True, text=True, timeout=60,
                             cwd=d)
        return run.returncode, run.stdout + run.stderr
