/* nds_arm7.c -- the stub ARM7 of the DS spike (XBA-223): the ARM7 binary is not run; this answers the IPC the
 * ARM9's start-up asks for, as calico's ARM7 (ds7_maine.elf) would:
 * - IPCSYNC: the ARM7's output nibble echoes the ARM9's (calico's boot stub waits for it to change);
 * - the boot stub's FIFO word: answered with 0 (crt0 keeps it as the heap's upper bound; 0 = up to the stack);
 * - calico's PXI channels: the ARM7 announces the ones it serves in its mask in main RAM (0x02FFFF64, read by
 *   pxiWaitRemote) and pings the ARM9 (IPCSYNC IRQ). Only the power channel (1), which pmInit waits for: its
 *   requests get no answer, which is what the hello world needs. Packets on other channels are counted
 *   (arm7_unknown) and logged with -v. */
#include <stdio.h>

#include "nds_internal.h"

#define PXI_REMOTE_MASK 0x02FFFF64u
#define PXI_CHANNEL_POWER 1u

void nds_arm7_reset(nds_machine *m)
{
    m->ipcsync_in9 = 0;
    m->arm7_phase = 0;
    m->arm7_unknown = 0;
    m->n79 = m->h79 = 0;
}

static void ping_arm9(nds_machine *m)
{
    if (m->ipcsync_out9 & 0x4000)
        nds_irq_raise(m, 1u << 16);
}

void nds_arm7_sync_written(nds_machine *m)
{
    m->ipcsync_in9 = (uint8_t)((m->ipcsync_out9 >> 8) & 15);
}

void nds_arm7_fifo_recv(nds_machine *m, uint32_t word)
{
    if (m->arm7_phase == 0) {
        nds_fifo_push79(m, 0);
        nds_write32(m, PXI_REMOTE_MASK, 1u << PXI_CHANNEL_POWER);
        m->arm7_phase = 1;
        if (m->verbose)
            fprintf(stderr, "nds: arm7 stub: boot handshake (ARM9 sent 0x%08X)\n", word);
        ping_arm9(m);
        return;
    }
    m->arm7_unknown++;
    if (m->verbose)
        fprintf(stderr, "nds: arm7 stub: PXI packet 0x%08X (channel %u, %s) not answered\n", word, word & 31,
                (word >> 5) & 1 ? "reply" : "request");
}
