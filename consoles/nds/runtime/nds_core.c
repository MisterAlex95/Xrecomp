/* nds_core.c -- the DS spike's machine (XBA-223): loading a .nds as the firmware would (direct boot), the frame
 * loop (lines, IRQs, halt, native code or the interpreter). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds_internal.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int nds_parse_header(const uint8_t *rom, size_t size, nds_info *out, char *why, size_t why_n)
{
    memset(out, 0, sizeof *out);
    if (size < 0x200) {
        snprintf(why, why_n, "file shorter than the 512-byte .nds header");
        return -1;
    }
    memcpy(out->title, rom, 12);
    memcpy(out->game_code, rom + 12, 4);
    out->arm9_rom_offset = le32(rom + 0x20);
    out->arm9_entry = le32(rom + 0x24);
    out->arm9_ram = le32(rom + 0x28);
    out->arm9_size = le32(rom + 0x2C);
    out->arm7_rom_offset = le32(rom + 0x30);
    out->arm7_entry = le32(rom + 0x34);
    out->arm7_ram = le32(rom + 0x38);
    out->arm7_size = le32(rom + 0x3C);
    if ((uint64_t)out->arm9_rom_offset + out->arm9_size > size || out->arm9_size == 0) {
        snprintf(why, why_n, "ARM9 binary outside the file");
        return -1;
    }
    if ((out->arm9_ram >> 24) != 0x02 || ((out->arm9_ram & 0xFFFFFF) + (uint64_t)out->arm9_size) > NDS_MAIN_SIZE) {
        snprintf(why, why_n, "ARM9 binary at 0x%08X+0x%X: not in main RAM", out->arm9_ram, out->arm9_size);
        return -1;
    }
    return 0;
}

nds_machine *nds_create(const uint8_t *rom, size_t size, char *why, size_t why_n)
{
    nds_info info;
    if (nds_parse_header(rom, size, &info, why, why_n))
        return NULL;
    nds_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_n, "out of memory");
        return NULL;
    }
    m->main_ram = calloc(1, NDS_MAIN_SIZE);
    if (!m->main_ram) {
        free(m);
        snprintf(why, why_n, "out of memory");
        return NULL;
    }
    m->h.rc_main = m->main_ram;
    nds_bus_reset(m);
    nds_arm7_reset(m);
    for (int i = 0; i < 1024; i++)
        m->rc_cache_pc[i] = 0xFFFFFFFFu;
    m->swi_wait_first = true;
    m->rc_after_jump = true;
    /* What the firmware does before it jumps to the ARM9's entry: the binaries in main RAM, the cartridge header at
     * 0x027FFE00 (0x02FFFE00 in calico's mirror). */
    memcpy(m->main_ram + (info.arm9_ram & (NDS_MAIN_SIZE - 1)), rom + info.arm9_rom_offset, info.arm9_size);
    if ((info.arm7_ram >> 24) == 0x02 && (uint64_t)info.arm7_rom_offset + info.arm7_size <= size &&
        (info.arm7_ram & 0xFFFFFF) + (uint64_t)info.arm7_size <= NDS_MAIN_SIZE)
        memcpy(m->main_ram + (info.arm7_ram & (NDS_MAIN_SIZE - 1)), rom + info.arm7_rom_offset, info.arm7_size);
    memcpy(m->main_ram + 0x3FFE00, rom, 0x170);
    nds_cpu_reset(m, info.arm9_entry);
    m->h.next_event = m->line_end;
    return m;
}

void nds_destroy(nds_machine *m)
{
    if (!m)
        return;
    free(m->span_state);
    free(m->span_checked);
    free(m->missed);
    free(m->main_ram);
    free(m);
}

void nds_set_verbose(nds_machine *m, int level)
{
    m->verbose = level;
}

int nds_run_frame(nds_machine *m)
{
    uint64_t frame = m->frames;
    while (!m->error[0]) {
        if (m->h.now >= m->line_end) {
            nds_line_event(m);
            if (m->vcount == 0) {
                m->frames++;
                if (m->frames != frame)
                    return 0;
            }
            continue;
        }
        m->h.next_event = m->line_end;
        if (m->h.irq_line && !(m->h.cpu.cpsr_ctl & 0x80u)) {
            nds_cpu_irq(m);
            continue;
        }
        if (m->h.cpu.halted) {
            m->h.now = m->line_end;
            continue;
        }
        if (!nds_recomp_dispatch(m)) {
            nds_cpu_step(m);
            nds_recomp_note_interp(m);
        }
    }
    return -1;
}

void nds_dump_trace(const nds_machine *m, FILE *f)
{
    for (unsigned i = 0; i < 64; i++) {
        uint32_t v = m->trace[(m->trace_at + i) & 63];
        fprintf(f, "  %08X%s\n", v & ~1u, (v & 1) ? " T" : "");
    }
    fprintf(f, "  regs:");
    for (int i = 0; i < 16; i++)
        fprintf(f, " r%d=%08X", i, m->h.cpu.r[i]);
    fprintf(f, " cpsr=%08X\n", nds_cpu_cpsr(m));
}

const char *nds_error(const nds_machine *m)
{
    return m->error;
}

uint64_t nds_frames(const nds_machine *m)
{
    return m->frames;
}

const uint16_t *nds_screen(const nds_machine *m, int top)
{
    bool a_top = (m->io[0x305] & 0x80) != 0;
    return (top ? a_top : !a_top) ? m->fb_a : m->fb_b;
}
