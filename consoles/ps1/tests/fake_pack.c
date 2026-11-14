/* A PS1 title pack's library for test_pack.c, built the way tools/prepare builds one: a shared library exporting
 * ps1_recomp_title_table, its core calls left undefined (resolved against the program that loads it, which exports the
 * core: ps1_core_exported). The test sets the table's source_sha256 (FAKE_PACK_SRC_SHA) and abi (FAKE_PACK_ABI). */
#include "ps1_recomp_rt.h"

#include <stdlib.h>

static void f_main(ps1_machine *m)
{
    (void)ps1_rc_gte_read(m, 0); /* a core symbol, from the loading program */
}

static const ps1_recomp_entry entries[] = {
    {0x80020000u, 4u, 0x0123456789ABCDEFull, f_main},
};

__attribute__((visibility("default"))) const ps1_recomp_table *ps1_recomp_title_table(void)
{
    static ps1_recomp_table t;
    const char *abi = getenv("FAKE_PACK_ABI");
    t.abi = abi && *abi ? (uint32_t)atoi(abi) : PS1_RECOMP_ABI;
    t.count = 1;
    t.entries = entries;
    t.source_sha256 = getenv("FAKE_PACK_SRC_SHA");
    t.boot_pc = 0x80010000u;
    return &t;
}
