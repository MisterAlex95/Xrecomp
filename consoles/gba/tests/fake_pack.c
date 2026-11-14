/* A GBA title pack's library for test_pack.c, built the way tools/prepare builds one: a shared library exporting
 * gba_recomp_title_table, its core calls left undefined (resolved against the program that loads it, which exports the
 * core: gba_core_exported). The test sets the table's rom_sha256 (FAKE_PACK_ROM_SHA) and abi (FAKE_PACK_ABI). */
#include "gba_recomp.h"

#include <stdlib.h>

static void f_arm(gba_machine *m)
{
    gba_set_keys(m, 0);   /* a core symbol, from the loading program */
}

static void f_thumb(gba_machine *m)
{
    gba_set_keys(m, 0);
}

static const gba_recomp_entry entries[] = {
    {0x08000000u, 0u, f_arm},
    {0x08000100u, GBA_RECOMP_THUMB, f_thumb},
};

__attribute__((visibility("default"))) const gba_recomp_table *gba_recomp_title_table(void)
{
    static gba_recomp_table t;
    const char *abi = getenv("FAKE_PACK_ABI");
    t.abi = abi && *abi ? (uint32_t)atoi(abi) : GBA_RECOMP_ABI;
    t.count = 2;
    t.entries = entries;
    t.rom_sha256 = getenv("FAKE_PACK_ROM_SHA");
    return &t;
}
