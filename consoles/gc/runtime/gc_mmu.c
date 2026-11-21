/* gc_mmu.c -- the Gekko's address translation and locked cache (XBA-260).
 *
 * Translation as the 750 does it, for the accesses that leave the inline MEM1 path of gc_recomp_rt.h:
 * - real mode (MSR[DR] / [IR] clear): the physical address is EA & 0x3FFFFFFF (the console decodes no more);
 * - the four DBATs / IBATs (SPR 536-543 / 528-535): BEPI / BL / Vs / Vp, BRPN / PP, checked first;
 * - else the segment registers and the hashed page table at SDR1: primary then secondary PTEG, PTE match on
 *   V / VSID / H / API, PP checked against Ks / Kp, R (and C on a store) set in the PTE in memory. There is no
 *   TLB: every slow access walks the table (software that relies on a stale TLB entry after changing a PTE without
 *   tlbie would differ; tlbie / tlbsync are no-ops).
 * A failed data access raises the DSI (DSISR: 0x40000000 no translation, 0x08000000 protection, | 0x02000000 on a
 * store; DAR = EA) and longjmps back to the run loop, so the faulting instruction has no effect; a failed fetch
 * raises the ISI (SRR1: 0x40000000, 0x08000000, 0x10000000 no-execute segment). Direct-store segments (T = 1) are
 * not translated (DSI / ISI).
 *
 * The fast path: when the BATs map 0x80000000 and 0xC0000000 onto MEM1 read / write for the current MSR[PR] (the
 * IPL's and the SDK's setup: 256 MB blocks at physical 0, or any block at least as large as MEM1) and translation is
 * on, an EA >= 0x80000000 below MEM1's end is MEM1[EA & 0x3FFFFFFF]; in real mode every EA is. gc_mmu_bats
 * re-examines the BATs after a BAT write, gc_mmu_update picks the mode after every MSR change (gc_rc_head's
 * dfast_mask / dfast_val, the machine's ifast_*), so lifted code keeps one AND / XOR / OR and a compare per access
 * whatever the program does with the other BATs (the SDK maps the locked cache with DBAT3) or the page table.
 * Instruction fetches also take it with 0x80000000 mapped alone (libogc maps no code at 0xC0000000, XBA-279): EAs
 * 0x80000000 .. 0xBFFFFFFF then, when the first IBAT touching that MEM1 view maps it (xlate takes the first match).
 *
 * Locked cache (HID2[LCE]): the upper 16 KB of the data cache become memory at EA 0xE0000000 (where the SDK maps
 * it); dcbz_l zeroes one of its lines, the locked-cache DMA (DMAU / DMAL, SPR 922 / 923) copies between it and
 * main memory at once (the trigger bit reads back clear, the queue length in HID2 stays 0). */
#include <string.h>

#include "gc_internal.h"

enum { XL_OK = 0, XL_MISS, XL_PROT, XL_NOEXEC };
enum { ACC_READ = 0, ACC_WRITE = 1, ACC_FETCH = 2 };

#define SPR_SDR1 25
#define SPR_IBAT0U 528
#define SPR_DBAT0U 536
#define SPR_DMAU 922
#define SPR_DMAL 923

/* The BATs valid for pr map [base, base + MEM1) to [0, MEM1) read / write, as xlate sees them: for every 128 KB block
 * (the BATs' granularity) the first matching BAT translates it to block - base. One BAT of 32 MB or 256 MB (the IPL,
 * older SDKs) or several (later SDKs split MEM1: 16 MB at 0x80000000 and 8 MB at 0x81000000, Prince of Persia: The
 * Two Thrones) alike. */
static bool bats_map_mem1(const uint32_t *bat, unsigned pr, uint32_t base)
{
    for (uint32_t off = 0; off < GC_RAM_SIZE; off += 0x20000u) {
        uint32_t ea = base + off;
        int i;
        for (i = 0; i < 4; i++) {
            uint32_t u = bat[2 * i], l = bat[2 * i + 1];
            if (!(u & (pr ? 1u : 2u)))
                continue;
            uint32_t mask = (((u >> 2) & 0x7FFu) << 17) | 0x1FFFFu;
            if ((ea & ~mask) != (u & 0xFFFE0000u & ~mask))
                continue;
            if ((l & 3u) != 2 || ((l & 0xFFFE0000u & ~mask) | (ea & mask)) != off)
                return false;
            break;
        }
        if (i == 4)
            return false;
    }
    return true;
}

static bool bats_default(const uint32_t *bat, unsigned pr)
{
    return bats_map_mem1(bat, pr, 0x80000000u) && bats_map_mem1(bat, pr, 0xC0000000u);
}

void gc_mmu_bats(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    for (unsigned pr = 0; pr < 2; pr++) {
        m->dbats_default[pr] = bats_default(&c->spr[SPR_DBAT0U], pr);
        m->ibats_default[pr] = bats_default(&c->spr[SPR_IBAT0U], pr);
        m->ibats_lo[pr] = bats_map_mem1(&c->spr[SPR_IBAT0U], pr, 0x80000000u);
    }
    gc_mmu_update(m);
}

void gc_mmu_update(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    unsigned pr = (c->msr & PPC_MSR_PR) ? 1 : 0;
    if (!(c->msr & PPC_MSR_DR))
        m->h.dfast_mask = m->h.dfast_val = 0;
    else if (m->dbats_default[pr])
        m->h.dfast_mask = m->h.dfast_val = 0x80000000u;
    else
        m->h.dfast_mask = 0, m->h.dfast_val = 0xFFFFFFFFu;
    if (!(c->msr & PPC_MSR_IR))
        m->ifast_mask = m->ifast_val = 0;
    else if (m->ibats_default[pr])
        m->ifast_mask = m->ifast_val = 0x80000000u;
    else if (m->ibats_lo[pr]) /* the SDK maps no code at 0xC0000000: 0x80000000 .. 0xBFFFFFFF alone (XBA-279) */
        m->ifast_mask = 0xC0000000u, m->ifast_val = 0x80000000u;
    else
        m->ifast_mask = 0, m->ifast_val = 0xFFFFFFFFu;
    for (int a = 0; a < 3; a++)
        m->xl_memo_blk[a] = 0; /* the BATs or MSR changed */
}

/* The BATs and the page table: xlate's cases after real mode and the remembered block. Out of line so that xlate's
 * common cases inline into gc_load / gc_store (hardware registers, the write-gather pipe: XBA-295). */
static __attribute__((noinline)) int xlate_walk(gc_machine *m, uint32_t ea, int acc, uint32_t *pa)
{
    gc_cpu *c = &m->h.cpu;
    unsigned pr = (c->msr & PPC_MSR_PR) ? 1 : 0;
    const uint32_t *bat = &c->spr[acc == ACC_FETCH ? SPR_IBAT0U : SPR_DBAT0U];
    for (int i = 0; i < 4; i++) {
        uint32_t u = bat[2 * i], l = bat[2 * i + 1];
        if (!(u & (pr ? 1u : 2u)))
            continue;
        uint32_t mask = (((u >> 2) & 0x7FFu) << 17) | 0x1FFFFu;
        if ((ea & ~mask) != (u & 0xFFFE0000u & ~mask))
            continue;
        unsigned pp = l & 3u;
        if (pp == 0 || (acc == ACC_WRITE && pp != 2))
            return XL_PROT;
        *pa = (l & 0xFFFE0000u & ~mask) | (ea & mask);
        m->xl_memo_blk[acc] = (ea >> 17) + 1u;
        m->xl_memo_pa[acc] = *pa & ~0x1FFFFu;
        return XL_OK;
    }
    uint32_t sr = c->sr[ea >> 28];
    if (sr & 0x80000000u) /* direct-store segment */
        return acc == ACC_FETCH ? XL_NOEXEC : XL_MISS;
    if (acc == ACC_FETCH && (sr & 0x10000000u))
        return XL_NOEXEC;
    uint32_t vsid = sr & 0xFFFFFFu, page = (ea >> 12) & 0xFFFFu, hash = (vsid & 0x7FFFFu) ^ page;
    unsigned key = pr ? (sr >> 29) & 1u : (sr >> 30) & 1u;
    uint32_t sdr1 = c->spr[SPR_SDR1], org = sdr1 & 0xFFFF0000u, hmask = sdr1 & 0x1FFu;
    for (uint32_t h = 0; h < 2; h++) {
        uint32_t hh = h ? (~hash & 0x7FFFFu) : hash;
        uint32_t pteg = (org & 0xFE000000u) | ((((org >> 16) & 0x1FFu) | ((hh >> 10) & hmask)) << 16) |
                        ((hh & 0x3FFu) << 6);
        uint32_t want = 0x80000000u | vsid << 7 | h << 6 | (page >> 10);
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t a = (pteg + 8 * k) & 0x3FFFFFFFu;
            if (a > GC_RAM_SIZE - 8)
                return XL_MISS;
            if (rc_be32(m->h.ram + a) != want)
                continue;
            uint32_t w1 = rc_be32(m->h.ram + a + 4), pp = w1 & 3u;
            bool write = acc == ACC_WRITE;
            bool ok = key == 0 ? (!write || pp != 3) : (pp != 0 && (!write || pp == 2));
            if (!ok)
                return XL_PROT;
            uint32_t nw = w1 | 0x100u | (write ? 0x80u : 0); /* R, C */
            if (nw != w1)
                rc_put32(m->h.ram + a + 4, nw);
            *pa = (w1 & 0xFFFFF000u) | (ea & 0xFFFu);
            return XL_OK;
        }
    }
    return XL_MISS;
}

static inline int xlate(gc_machine *m, uint32_t ea, int acc, uint32_t *pa)
{
    if (!(m->h.cpu.msr & (acc == ACC_FETCH ? PPC_MSR_IR : PPC_MSR_DR))) {
        *pa = ea & 0x3FFFFFFFu;
        return XL_OK;
    }
    /* XBA-279: the 128 KB block the last BAT translation of this kind fell in (BATs map whole 128 KB blocks, so the
     * first match is the same BAT for every EA of a block); forgotten whenever the BATs or MSR change */
    if (m->xl_memo_blk[acc] == (ea >> 17) + 1u) {
        *pa = m->xl_memo_pa[acc] | (ea & 0x1FFFFu);
        return XL_OK;
    }
    return xlate_walk(m, ea, acc, pa);
}

static void dsi(gc_machine *m, uint32_t ea, int res, bool write, uint32_t pc)
{
    gc_cpu *c = &m->h.cpu;
    c->spr[PPC_SPR_DSISR] = (res == XL_PROT ? 0x08000000u : 0x40000000u) | (write ? 0x02000000u : 0);
    c->spr[PPC_SPR_DAR] = ea;
    c->pc = pc;
    if (m->verbose >= 2)
        fprintf(stderr, "gc: DSI at %08X, EA %08X\n", pc, ea);
    gc_exception(m, 0x300, 0);
    if (m->fault_armed)
        longjmp(m->fault_jb, 1);
}

static bool in_lc(gc_machine *m, uint32_t ea, unsigned size)
{
    return (m->h.cpu.spr[PPC_SPR_HID2] & GC_HID2_LCE) && ea >= 0xE0000000u && ea + size <= 0xE0000000u + GC_LC_SIZE;
}

uint32_t gc_load(gc_machine *m, uint32_t ea, unsigned size, uint32_t pc)
{
    if (in_lc(m, ea, size)) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v = v << 8 | m->lc[ea - 0xE0000000u + i];
        return v;
    }
    uint32_t pa;
    int r = xlate(m, ea, ACC_READ, &pa);
    if (r) {
        dsi(m, ea, r, false, pc);
        return 0;
    }
    return gc_phys_read(m, pa, size);
}

static __attribute__((noinline)) void store_walk(gc_machine *m, uint32_t ea, uint32_t v, unsigned size, uint32_t pc)
{
    if (in_lc(m, ea, size)) {
        for (unsigned i = 0; i < size; i++)
            m->lc[ea - 0xE0000000u + i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        return;
    }
    uint32_t pa;
    int r = xlate(m, ea, ACC_WRITE, &pa);
    if (r) {
        dsi(m, ea, r, true, pc);
        return;
    }
    if ((pa & 0xFFFF8000u) == 0x0C008000u) { /* the write-gather pipe, where gc_phys_write sends it (XBA-279) */
        gx_fifo_write(m->gx, v, size);
        return;
    }
    gc_phys_write(m, pa, v, size);
}

/* An instruction's store. The common case first, with nothing kept across a call: not the locked cache, the data
 * translation real mode or the remembered BAT block (xlate's inline cases), then the write-gather pipe or the bus
 * (lifted code's stores to hardware registers and the pipe come here: XBA-295). store_walk does the rest as before. */
void gc_store(gc_machine *m, uint32_t ea, uint32_t v, unsigned size, uint32_t pc)
{
    if (!in_lc(m, ea, size)) {
        uint32_t pa;
        if (!(m->h.cpu.msr & PPC_MSR_DR))
            pa = ea & 0x3FFFFFFFu;
        else if (m->xl_memo_blk[ACC_WRITE] == (ea >> 17) + 1u)
            pa = m->xl_memo_pa[ACC_WRITE] | (ea & 0x1FFFFu);
        else
            goto walk;
        if ((pa & 0xFFFF8000u) == 0x0C008000u)
            gx_fifo_write(m->gx, v, size);
        else
            gc_phys_write(m, pa, v, size);
        return;
    }
walk:
    store_walk(m, ea, v, size, pc);
}

/* Not an instruction's access: no DSI, an untranslatable address reads 0 and drops writes. */
static uint32_t quiet_read(gc_machine *m, uint32_t ea, unsigned size)
{
    uint32_t pa;
    if (in_lc(m, ea, size))
        return gc_load(m, ea, size, m->h.cpu.pc);
    return xlate(m, ea, ACC_READ, &pa) ? 0 : gc_phys_read(m, pa, size);
}

static void quiet_write(gc_machine *m, uint32_t ea, uint32_t v, unsigned size)
{
    uint32_t pa;
    if (in_lc(m, ea, size))
        gc_store(m, ea, v, size, m->h.cpu.pc);
    else if (!xlate(m, ea, ACC_WRITE, &pa))
        gc_phys_write(m, pa, v, size);
}

uint8_t gc_read8(gc_machine *m, uint32_t ea) { return (uint8_t)quiet_read(m, ea, 1); }
uint16_t gc_read16(gc_machine *m, uint32_t ea) { return (uint16_t)quiet_read(m, ea, 2); }
uint32_t gc_read32(gc_machine *m, uint32_t ea) { return quiet_read(m, ea, 4); }
void gc_write8(gc_machine *m, uint32_t ea, uint8_t v) { quiet_write(m, ea, v, 1); }
void gc_write16(gc_machine *m, uint32_t ea, uint16_t v) { quiet_write(m, ea, v, 2); }
void gc_write32(gc_machine *m, uint32_t ea, uint32_t v) { quiet_write(m, ea, v, 4); }

bool gc_ifetch(gc_machine *m, uint32_t pc, uint32_t *w)
{
    uint32_t pa = (pc & 0x3FFFFFFFu) | ((pc & m->ifast_mask) ^ m->ifast_val);
    if (pa > GC_RAM_SIZE - 4) {
        int r = xlate(m, pc, ACC_FETCH, &pa);
        if (r) {
            m->h.cpu.pc = pc;
            if (m->verbose >= 2)
                fprintf(stderr, "gc: ISI at %08X\n", pc);
            gc_exception(m, 0x400, r == XL_PROT ? 0x08000000u : r == XL_NOEXEC ? 0x10000000u : 0x40000000u);
            return false;
        }
        if (pa > GC_RAM_SIZE - 4) {
            gc_stop(m, "PC %08X (physical %08X) outside MEM1", pc, pa);
            return false;
        }
    }
    *w = rc_be32(m->h.ram + pa);
    return true;
}

bool gc_icode_direct(gc_machine *m, uint32_t pc)
{
    uint32_t pa = (pc & 0x3FFFFFFFu) | ((pc & m->ifast_mask) ^ m->ifast_val);
    if (pa <= GC_RAM_SIZE - 4)
        return true;
    return xlate(m, pc, ACC_FETCH, &pa) == XL_OK && pa == (pc & 0x3FFFFFFFu);
}

void gc_lc_dma(gc_machine *m)
{
    gc_cpu *c = &m->h.cpu;
    uint32_t u = c->spr[SPR_DMAU], l = c->spr[SPR_DMAL];
    c->spr[SPR_DMAL] = l & ~3u; /* done at once: trigger and flush read back clear */
    if (!(l & 2u) || !(c->spr[PPC_SPR_HID2] & GC_HID2_LCE))
        return;
    uint32_t lines = ((u & 0x1Fu) << 2) | ((l >> 2) & 3u);
    if (!lines)
        lines = 128;
    uint32_t mem = u & 0x3FFFFFE0u, lca = l & 0x3FE0u;
    bool load = l & 0x10u; /* DMA_LD: main memory -> locked cache */
    for (uint32_t i = 0; i < lines * 32; i++) {
        uint32_t pa = mem + i, lo = (lca + i) & (GC_LC_SIZE - 1);
        if (pa >= GC_RAM_SIZE)
            break;
        if (load)
            m->lc[lo] = m->h.ram[pa];
        else
            m->h.ram[pa] = m->lc[lo];
    }
    if (m->verbose >= 2)
        fprintf(stderr, "gc: locked-cache DMA %s %u lines, memory %08X, cache +%04X\n", load ? "load" : "store",
                lines, mem, lca);
}
