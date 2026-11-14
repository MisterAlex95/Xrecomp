/* GBA core: machine creation and the public accessors (gba_core.h). The frame loop is in gba_io.c, the CPU in
 * gba_cpu.c, the memory map in gba_bus.c, BIOS support in gba_bios.c. */
#include "gba_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

gba_machine *gba_create(const uint8_t *rom, size_t rom_size, const uint8_t *bios, size_t bios_size)
{
    if (!rom || rom_size == 0 || rom_size > GBA_ROM_MAX)
        return NULL;
    if (bios && (bios_size == 0 || bios_size > GBA_BIOS_SIZE))
        return NULL;
    gba_machine *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    /* Padded to a word so aligned reads at the end stay in bounds. */
    m->rom = calloc(1, (rom_size + 3) & ~(size_t)3);
    if (!m->rom) {
        free(m);
        return NULL;
    }
    memcpy(m->rom, rom, rom_size);
    m->rom_size = (uint32_t)rom_size;
    gba_save_init(m); /* save kind from the ROM, erased chip (0xFF) */

    if (bios) {
        memcpy(m->bios, bios, bios_size);
        m->builtin_bios = false;
    } else {
        gba_bios_install_builtin(m);
    }
    for (int i = 0; i < 256; i++)
        m->sine[i] = (int16_t)lrint(sin(i * (2.0 * 3.14159265358979323846 / 256.0)) * 16384.0);

    gba_io_reset(m);
    gba_bios_direct_boot(m);
    return m;
}

void gba_boot_bios(gba_machine *m)
{
    gba_io_reset(m);
    if (m->builtin_bios) {
        gba_bios_direct_boot(m);
        return;
    }
    gba_cpu_reset(m, 0, MODE_SVC | CPSR_I | CPSR_F);
}

void gba_destroy(gba_machine *m)
{
    if (!m)
        return;
    free(m->rom);
    free(m);
}

uint32_t gba_reg(gba_machine *m, int r)
{
    if (r == 15)
        return gba_cpu_pc(m);
    return (r >= 0 && r < 15) ? m->cpu.r[r] : 0;
}

uint32_t gba_cpsr(gba_machine *m) { return gba_get_cpsr(m); }
uint64_t gba_cycles(gba_machine *m) { return m->now; }
uint64_t gba_frames(gba_machine *m) { return m->frames; }
