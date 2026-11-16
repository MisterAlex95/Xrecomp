/* dc_core.c -- the Dreamcast spike's machine: loading, the dispatcher, lifted code (XBA-227, epic XBA-221).
 *
 * The dispatcher runs until the frame's last cycle: catch the hardware up when an event is due (scanlines, TMU),
 * take an interrupt when one is pending and unmasked, serve a BIOS stub, else run the lifted function for PC, else
 * interpret. Lifted functions come back here on returns and jumps through registers they cannot resolve, on
 * exceptions and SR writes, and when an event is due (they check cycles against next_event at their entries,
 * return sites and loop heads); calls between lifted functions nest on the host stack (sh4_ops.h, dc_recomp.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_internal.h"

dc_machine *dc_create(void)
{
    dc_machine *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->ram = calloc(1, DC_RAM_SIZE);
    m->vram = calloc(1, DC_VRAM_SIZE);
    m->aram = calloc(1, DC_ARAM_SIZE);
    if (!m->ram || !m->vram || !m->aram) {
        dc_destroy(m);
        return NULL;
    }
    m->cpu.ram = m->ram;
    sh4_decode_init();
    dc_bus_reset(m);
    dc_hw_reset(m);
    return m;
}

void dc_destroy(dc_machine *m)
{
    if (!m)
        return;
    free(m->ram);
    free(m->vram);
    free(m->aram);
    free(m->serial);
    free(m->missed);
    free(m);
}

sh4_cpu *dc_cpu(dc_machine *m)
{
    return &m->cpu;
}

void dc_set_verbose(dc_machine *m, int level)
{
    m->verbose = level;
}

uint32_t dc_peek32(dc_machine *m, uint32_t addr)
{
    return sh4_r32(&m->cpu, addr);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static bool in_ram(uint32_t a, uint32_t size)
{
    return sh4_is_ram(a) && (a & (DC_RAM_SIZE - 1)) + (uint64_t)size <= DC_RAM_SIZE;
}

int dc_load(dc_machine *m, const uint8_t *data, size_t size, char *err, size_t errlen)
{
    uint32_t entry;
    m->img_lo = UINT32_MAX;
    m->img_hi = 0;
    if (size >= 52 && memcmp(data, "\177ELF", 4) == 0) {
        if (data[4] != 1 || data[5] != 1 || rd16(data + 18) != 42) {
            snprintf(err, errlen, "not a 32-bit little-endian SH ELF");
            return -1;
        }
        entry = rd32(data + 24);
        uint32_t phoff = rd32(data + 28), phentsize = rd16(data + 42), phnum = rd16(data + 44);
        if ((uint64_t)phoff + (uint64_t)phentsize * phnum > size || phentsize < 32) {
            snprintf(err, errlen, "truncated program headers");
            return -1;
        }
        for (uint32_t i = 0; i < phnum; i++) {
            const uint8_t *ph = data + phoff + i * phentsize;
            if (rd32(ph) != 1) /* PT_LOAD */
                continue;
            uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20),
                     flags = rd32(ph + 24);
            if (memsz == 0)
                continue;
            if (filesz > memsz || (uint64_t)off + filesz > size || !in_ram(vaddr, memsz)) {
                snprintf(err, errlen, "segment %u at %08x does not fit main RAM", i, vaddr);
                return -1;
            }
            uint8_t *dst = m->ram + (vaddr & (DC_RAM_SIZE - 1));
            memcpy(dst, data + off, filesz);
            memset(dst + filesz, 0, memsz - filesz);
            if (flags & 1) { /* PF_X */
                uint32_t lo = (vaddr & 0x1FFFFFFFu) | 0x80000000u;
                if (lo < m->img_lo)
                    m->img_lo = lo;
                if (lo + filesz > m->img_hi)
                    m->img_hi = lo + filesz;
            }
        }
    } else {
        entry = 0x8C010000u;
        if (!in_ram(entry, (uint32_t)size) || size > DC_RAM_SIZE - 0x10000u) {
            snprintf(err, errlen, "binary too large");
            return -1;
        }
        memcpy(m->ram + 0x10000u, data, size);
        m->img_lo = entry;
        m->img_hi = entry + (uint32_t)size;
    }
    dc_bios_setup(m, entry);
    dc_hw_events(m);
    return 0;
}

bool dc_exited(const dc_machine *m, int *code)
{
    if (code)
        *code = m->exit_code;
    return m->exited;
}

const char *dc_serial(const dc_machine *m, size_t *len)
{
    if (len)
        *len = m->serial_len;
    return m->serial ? m->serial : "";
}

/* ---- lifted code ---- */

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

int dc_attach_recomp(dc_machine *m, const dc_recomp_table *t)
{
    if (t && t->abi != DC_RECOMP_ABI) {
        fprintf(stderr, "dc: lifted code ABI %u, runtime %u\n", t->abi, DC_RECOMP_ABI);
        return -1;
    }
    if (t) {
        for (uint32_t i = 0; i < t->nspans; i++) {
            const dc_recomp_span *s = &t->spans[i];
            if (!in_ram(s->lo, s->hi - s->lo) || fnv1a(m->ram + (s->lo & (DC_RAM_SIZE - 1)), s->hi - s->lo) != s->fnv1a) {
                fprintf(stderr, "dc: lifted code was made from other bytes (%08x-%08x): not attached\n", s->lo, s->hi);
                return -1;
            }
        }
    }
    m->rc = t;
    return 0;
}

#define RC_CACHE 4096
static struct {
    uint32_t addr;
    dc_recomp_fn fn;
} rc_cache[RC_CACHE];
static const dc_recomp_table *rc_cache_owner;

dc_recomp_fn dc_rc_lookup(dc_machine *m, uint32_t addr)
{
    const dc_recomp_table *t = m->rc;
    if (!t || (addr >> 28) != 0x8)
        return NULL;
    if (rc_cache_owner != t) {
        memset(rc_cache, 0, sizeof rc_cache);
        rc_cache_owner = t;
    }
    unsigned h = (addr >> 1) & (RC_CACHE - 1);
    if (rc_cache[h].addr == addr && rc_cache[h].fn)
        return rc_cache[h].fn;
    uint32_t lo = 0, hi = t->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (t->entries[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < t->count && t->entries[lo].addr == addr) {
        rc_cache[h].addr = addr;
        rc_cache[h].fn = t->entries[lo].fn;
        return t->entries[lo].fn;
    }
    return NULL;
}

dc_recomp_fn dc_rc_find(sh4_cpu *c, uint32_t addr)
{
    return dc_rc_lookup((dc_machine *)c, addr);
}

size_t dc_recomp_missed(dc_machine *m, uint32_t *out, size_t max)
{
    for (size_t i = 0; i < m->nmissed && i < max; i++)
        out[i] = m->missed[i];
    return m->nmissed;
}

void dc_get_stats(dc_machine *m, dc_stats *out)
{
    *out = m->stats;
    out->native = m->cpu.rc_native;
}

/* ---- the dispatcher ---- */

int dc_run_frames(dc_machine *m, int n)
{
    sh4_cpu *c = &m->cpu;
    int done = 0;
    for (; done < n && !m->exited; done++) {
        m->run_end = c->cycles + DC_FRAME_CYCLES;
        dc_hw_events(m);
        while (c->cycles < m->run_end && !m->exited) {
            if (c->cycles >= c->next_event)
                dc_hw_events(m);
            dc_hw_take_interrupt(m);
            if (c->sleeping) {
                c->cycles = c->next_event;
                continue;
            }
            uint32_t pc = c->pc;
            if (dc_bios_call(m, pc))
                continue;
            dc_recomp_fn fn = dc_rc_lookup(m, pc);
            if (fn) {
                uint64_t before = c->cycles;
                c->depth = 0;
                fn(c);
                m->stats.calls++;
                if (c->pc != pc || c->cycles != before)
                    continue;
            }
            m->stats.interp += sh4_interp_run(m, 4096);
        }
    }
    m->run_end = 0;
    return done;
}
