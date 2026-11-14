/* An N64 title pack's library for test_pack.c, built the way tools/prepare builds one: a shared library exporting
 * n64_recomp_title_table, its core calls left undefined (resolved against the program that loads it, which exports the
 * core: n64_core_exported). The test sets the table's source_sha256 (FAKE_PACK_SRC_SHA) and abi (FAKE_PACK_ABI). */
#include "n64_recomp_rt.h"

#include <stdlib.h>

static void f_main(n64_machine *m)
{
    uint64_t v = 0;
    (void)n64_rc_read(m, 0xFFFFFFFF80000000ull, 4, &v); /* a core symbol, from the loading program */
}

static const n64_recomp_entry entries[] = {
    {0x80100000u, 4u, 0x0123456789ABCDEFull, f_main},
};

__attribute__((visibility("default"))) const n64_recomp_table *n64_recomp_title_table(void)
{
    static n64_recomp_table t;
    const char *abi = getenv("FAKE_PACK_ABI");
    t.abi = abi && *abi ? (uint32_t)atoi(abi) : N64_RECOMP_ABI;
    t.count = 1;
    t.entries = entries;
    t.source_sha256 = getenv("FAKE_PACK_SRC_SHA");
    t.boot_pc = 0x80000400u;
    return &t;
}
