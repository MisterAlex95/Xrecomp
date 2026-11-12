"""recomp_unimpl (templates/new-game/src/recomp_manual.c) prints per address.

KOTOR's USB interrupt handler reaches an untranslated `cli`/`sti` pair on
every interrupt. With one global budget of 50 lines those two sites used it up
within a minute and no later site was ever printed. Each address now prints at
hits 1, 10, 100, 1000 ..., so a site hit in a loop is still visible as one and
a site reached later is still printed.

The function is compiled out of the template as it is, with the discovery
call stubbed. Skips itself if no C compiler is on PATH.
"""
import os
import re
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.c_fixture import command, compiler  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
MANUAL = os.path.join(ROOT, "templates", "new-game", "src", "recomp_manual.c")


def _function():
    text = open(MANUAL, encoding="utf-8").read()
    m = re.search(r"^void recomp_unimpl\(const char \*text, uint32_t va\)\n\{.*?^\}\n",
                  text, re.S | re.M)
    assert m, "recomp_unimpl not found in recomp_manual.c"
    return m.group(0)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int s_discovered;
static void recomp_discovery_unimpl(uint32_t va, const char *insn)
{ (void)va; (void)insn; s_discovered++; }
%s
int main(void)
{
    int i;
    for (i = 0; i < 1000; i++) {
        recomp_unimpl("cli", 0x003E3E89u);
        recomp_unimpl("sti", 0x003E3E9Au);
    }
    recomp_unimpl("bound eax, [ecx]", 0x00012345u);
    for (i = 0; i < 70; i++)
        recomp_unimpl("hlt", 0x00020000u + (uint32_t)i);
    printf("discovered %%d\n", s_discovered);
    return 0;
}
"""


def test_unimpl_prints_per_address_at_powers_of_ten():
    cc = compiler()
    if not cc:
        pytest.skip("no C compiler")
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        exe = os.path.join(d, "t.exe")
        with open(src, "w", encoding="utf-8") as f:
            f.write(HARNESS % _function())
        subprocess.run(command(cc, [src], exe), check=True, cwd=d)
        r = subprocess.run([exe], capture_output=True, text=True,
                           env={k: v for k, v in os.environ.items()
                                if k != "RECOMP_UNIMPL_TRAP"})
    assert r.returncode == 0, r.stderr
    lines = r.stderr.splitlines()
    cli = [l for l in lines if "0x003E3E89" in l]
    sti = [l for l in lines if "0x003E3E9A" in l]
    # 1, 10, 100, 1000 hits: four lines each, not 25 each and then silence.
    assert len(cli) == 4 and len(sti) == 4, lines
    assert cli[0].startswith("[UNIMPL] untranslated instruction REACHED: `cli` at 0x003E3E89 (a no-op")
    assert cli[-1].endswith("1000 times")
    # A site reached after 2000 hits of the others is still printed.
    assert any("`bound eax, [ecx]` at 0x00012345" in l for l in lines)
    # The table holds 64 addresses: 2 + 1 + 61 printed, then one note.
    hlt = [l for l in lines if "`hlt`" in l]
    assert len(hlt) == 61, len(hlt)
    assert sum("further ones are not printed" in l for l in lines) == 1
    # Every distinct site still reaches the discovery record once.
    assert "discovered 73" in r.stdout
    # tools/compat reads the first line of a site.
    from tools.compat.__main__ import UNIMPL_RE
    assert UNIMPL_RE.search(cli[0]).group(2) == "0x003E3E89"
