/* dc_bios.c -- the Dreamcast BIOS in C (XBA-227): no image, no BIOS code is run or needed.
 *
 * A disc's program starts with the CPU as the BIOS leaves it (after IP.BIN): privileged, register bank 1, blocked,
 * all interrupts masked, the stack below 0x8C00F400, and the syscall vectors at 0x8C0000B0-0x8C0000E0 that
 * programs call through (KallistiOS: kernel/arch/dreamcast/hardware/syscalls.c, syscall_font.s). Here each vector
 * points at a stub address in low RAM; when PC reaches one the dispatcher serves the call in C (r4-r7 arguments,
 * r7 the function, r6 the super function of the GD-ROM vector, r1 for the font vector), sets r0 and returns to PR.
 * What a KOS program uses at start: sysinfo (init, console ID), the ROM font address, flash reads (no flash: an
 * error), the GD-ROM vector (no drive: "no disc"), the system vector (reset / menu: the run ends). */
#include <stdio.h>
#include <string.h>

#include "dc_internal.h"

static void w32(dc_machine *m, uint32_t a, uint32_t v)
{
    memcpy(m->ram + (a & (DC_RAM_SIZE - 1)), &v, 4);
}

static uint32_t stub_addr(int k)
{
    return DC_HLE_BASE + 0x10u * (uint32_t)k;
}

void dc_bios_setup(dc_machine *m, uint32_t entry)
{
    sh4_cpu *c = &m->cpu;
    static const struct {
        uint32_t vec;
        int stub;
    } vecs[] = {{0x8C0000B0u, DC_HLE_SYSINFO}, {0x8C0000B4u, DC_HLE_ROMFONT}, {0x8C0000B8u, DC_HLE_FLASHROM},
                {0x8C0000BCu, DC_HLE_GDROM},   {0x8C0000C0u, DC_HLE_GDROM2},  {0x8C0000E0u, DC_HLE_SYSTEM}};
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++)
        w32(m, vecs[i].vec, stub_addr(vecs[i].stub));
    for (int k = 0; k < DC_HLE_COUNT; k++)
        w32(m, stub_addr(k), 0x0009000Bu); /* rts; nop: never run (the dispatcher serves the stub) */
    memcpy(m->ram + (DC_HLE_ID_ADDR & (DC_RAM_SIZE - 1)), "XRECOMP\0", 8);

    memset(c->r, 0, sizeof c->r);
    memset(c->rbank, 0, sizeof c->rbank);
    c->sr = 0;
    sh4_set_sr(c, 0x700000F0u);
    c->vbr = 0x8C00F400u;
    c->gbr = 0x8C000000u;
    c->r[15] = 0x8C00F400u;
    c->pr = stub_addr(DC_HLE_EXIT);
    c->pc = entry;
    c->fpscr = 0;
    sh4_set_fpscr(c, 0x00040001u);
}

bool dc_bios_is_stub(uint32_t pc)
{
    uint32_t p1 = (pc & 0x1FFFFFFFu) | 0x80000000u;
    return (pc >> 29) != 7 && p1 >= DC_HLE_BASE && p1 < stub_addr(DC_HLE_COUNT) && !(p1 & 0xF);
}

bool dc_bios_call(dc_machine *m, uint32_t pc)
{
    uint32_t p1 = (pc & 0x1FFFFFFFu) | 0x80000000u;
    if (!dc_bios_is_stub(pc))
        return false;
    sh4_cpu *c = &m->cpu;
    int k = (int)((p1 - DC_HLE_BASE) >> 4);
    int32_t r6 = (int32_t)c->r[6], r7 = (int32_t)c->r[7];
    uint32_t r0 = 0;
    m->stats.hle++;
    switch (k) {
    case DC_HLE_SYSINFO:
        if (r7 == 3)
            r0 = DC_HLE_ID_ADDR; /* console ID */
        else if (r7 == 2)
            r0 = (uint32_t)-1; /* icon: none */
        break;
    case DC_HLE_ROMFONT:
        if (c->r[1] == 0)
            r0 = 0xA0100020u; /* where the BIOS keeps its font: no BIOS here, it reads as zeros */
        break;
    case DC_HLE_FLASHROM:
        r0 = (uint32_t)-1; /* no flash contents */
        break;
    case DC_HLE_GDROM:
    case DC_HLE_GDROM2:
        if (r6 == -1) {
            r0 = 0; /* MISC init / set vector */
        } else if (r7 == 4 && c->r[4]) {
            w32(m, c->r[4], 7); /* drive status: no disc */
            w32(m, c->r[4] + 4, 0);
            r0 = 0;
        } else if (r7 == 1) {
            r0 = (uint32_t)-1; /* command status: failed */
        } else {
            r0 = 0;
        }
        break;
    case DC_HLE_SYSTEM:
        if (m->verbose)
            fprintf(stderr, "dc: BIOS system call %d (reset / menu): run ends\n", r7);
        m->exited = true;
        m->exit_code = 0;
        return true;
    case DC_HLE_EXIT:
        m->exited = true;
        m->exit_code = (int)c->r[8]; /* KOS's arch_real_exit keeps the code in r8 */
        if (m->verbose)
            fprintf(stderr, "dc: program returned to the BIOS (code %d)\n", m->exit_code);
        return true;
    default:
        break;
    }
    if (m->verbose >= 2)
        fprintf(stderr, "dc: BIOS call %d r4=%08x r5=%08x r6=%08x r7=%08x -> %08x\n", k, c->r[4], c->r[5], c->r[6],
                c->r[7], r0);
    c->r[0] = r0;
    c->pc = c->pr;
    c->cycles += 20;
    return true;
}
