/* gc_dsp.c -- the DSP interface at 0xCC005000 (XBA-263): mailboxes both ways, the control / status register and
 * its three interrupts (DSP, ARAM DMA, audio DMA), the ARAM DMA (16 MB, timed), the audio DMA's registers, and the
 * DSP itself at a high level: the ROM's boot protocol, the init code, and the microcode a game uploads, recognised
 * by the hash of its image (gc_ucode_hash) and emulated by gc_ax.c / gc_asnd.c / gc_zelda.c / gc_aesnd.c (XBA-266:
 * the Zelda / JAudio family and libogc's AESND); an unknown image is logged and
 * played as AX (most GameCube games use the AX family). No DSP ROM dump, no LLE. Facts from the hardware's
 * documentation (Dolphin's notes and ucode hash list among them); our own code.
 *
 * Registers (16-bit, index = offset / 2): 0x00 / 0x01 mail to the DSP (high, low: writing the low half sends it),
 * 0x02 / 0x03 mail from the DSP (bit 15 of the high half: full; reading the low half takes it), 0x05 control,
 * 0x09 AR_INFO, 0x0B AR_MODE, 0x0D AR_REFRESH, 0x10 .. 0x15 ARAM DMA (main memory address, ARAM address, count with
 * the direction in bit 31: writing the count's low half starts it), 0x18 / 0x19 audio DMA start, 0x1B its control
 * (bit 15 enable, 32-byte blocks), 0x1D blocks left (gc_ai.c plays it). */
#include <stdlib.h>
#include <string.h>

#include "gc_dsp_internal.h"

/* ---- interrupts ---- */

static void dsp_pi_update(gc_machine *m)
{
    gc_audio *a = m->audio;
    uint16_t cr = a->csr;
    bool on = (cr >> 1) & cr & (DSPCR_AIDINT | DSPCR_ARINT | DSPCR_DSPINT);
    if (on)
        m->pi_intsr |= GC_INT_DSP;
    else
        m->pi_intsr &= ~GC_INT_DSP;
    gc_check_interrupts(m);
}

void gc_dsp_irq(gc_machine *m, uint16_t bit)
{
    m->audio->csr |= bit;
    dsp_pi_update(m);
}

void gc_dsp_schedule_int(gc_machine *m, uint64_t delay)
{
    gc_audio *a = m->audio;
    uint64_t t = m->h.now + delay;
    if (t < a->dspint_at)
        a->dspint_at = t;
    gc_schedule(m);
}

/* ---- mail from the DSP ---- */

void gc_dsp_push_mail(gc_machine *m, uint32_t mail, bool irq, uint64_t delay)
{
    gc_audio *a = m->audio;
    if (a->qn == GC_MAILQ) { /* a CPU that never reads: drop the oldest */
        a->qh = (a->qh + 1) % GC_MAILQ;
        a->qn--;
    }
    if (irq) {
        if (a->qn == 0)
            gc_dsp_schedule_int(m, delay);
        else
            a->q[(a->qh + a->qn - 1) % GC_MAILQ].irq_next = true; /* when the one before it is read */
    }
    a->q[(a->qh + a->qn) % GC_MAILQ] = (gc_mail){mail, false};
    a->qn++;
    a->mails_out++;
    if (m->verbose >= 2)
        fprintf(stderr, "gc: dsp: mail to CPU %08X%s\n", mail, irq ? " (interrupt)" : "");
}

static uint16_t mail_from_hi(gc_machine *m)
{
    gc_audio *a = m->audio;
    if (!(a->csr & DSPCR_HALT) && a->qn)
        a->last_mail = a->q[a->qh].v;
    return (uint16_t)(a->last_mail >> 16);
}

static uint16_t mail_from_lo(gc_machine *m)
{
    gc_audio *a = m->audio;
    if (!(a->csr & DSPCR_HALT) && a->qn) {
        gc_mail ml = a->q[a->qh];
        a->qh = (a->qh + 1) % GC_MAILQ;
        a->qn--;
        a->last_mail = ml.v;
        if (ml.irq_next)
            gc_dsp_schedule_int(m, 0);
    }
    uint16_t lo = (uint16_t)a->last_mail;
    a->last_mail &= 0x7FFFFFFFu; /* read: the high half's full bit clears, the rest reads the same */
    return lo;
}

/* ---- microcodes ---- */

static const struct {
    uint32_t crc;
    int kind;
} k_ucodes[] = {
    /* AX (Dolphin's list: Paper Mario TTYD, Alien Hominid, F-Zero GX, Billy Hatcher...; 0x4E8A8B21 is the early
     * one, Smash Bros. Melee, Star Fox Adventures, Super Monkey Ball...) */
    {0x3AD3B7ACu, UC_AX}, {0x3DAF59B9u, UC_AX}, {0x07F88145u, UC_AX}, {0xE2136399u, UC_AX}, {0x4E8A8B21u, UC_AX_OLD},
    /* Zelda / JAudio family (Wind Waker, Twilight Princess GC, Sunshine, Luigi's Mansion, Pikmin, the IPL) */
    {0x86840740u, UC_ZELDA}, {0x6CA33A6Du, UC_ZELDA}, {0x6BA3B3EAu, UC_ZELDA}, {0x24B22038u, UC_ZELDA},
    {0x2FCDF1ECu, UC_ZELDA}, {0xDF059F68u, UC_ZELDA}, {0x4BE6A5CBu, UC_ZELDA}, {0x267FD05Au, UC_ZELDA},
    {0x42F64AC4u, UC_ZELDA}, {0x56D36052u, UC_ZELDA},
    {0x65D6CC6Fu, UC_CARD}, {0xDD7E72D5u, UC_GBA},
    /* libogc's ASND, every release */
    {0x8D69A19Bu, UC_ASND}, {0xCC2FD441u, UC_ASND}, {0xA81582E2u, UC_ASND}, {0xDBBEEB61u, UC_ASND},
    {0xBAD876EFu, UC_ASND}, {0xFA9C576Fu, UC_ASND}, {0x614DD145u, UC_ASND}, {0x5DBF8BF1u, UC_ASND},
    {0x373A950Eu, UC_ASND},
    /* libogc's AESND: 2010, 2012, EDuke32's, 2020, 2020 padded, 2022 padded, 2023 */
    {0x008366AFu, UC_AESND}, {0x078066ABu, UC_AESND}, {0x5AD4D933u, UC_AESND}, {0x84C680A9u, UC_AESND},
    {0xA02A6131u, UC_AESND}, {0x2E5E4100u, UC_AESND}, {0x002E5E41u, UC_AESND},
};

static const char *ucode_name(int kind)
{
    switch (kind) {
    case UC_ROM: return "rom";
    case UC_INIT: return "init";
    case UC_AX: return "ax";
    case UC_AX_OLD: return "ax-old";
    case UC_UNKNOWN_AX: return "unknown (as ax)";
    case UC_ASND: return "asnd";
    case UC_CARD: return "card";
    case UC_ZELDA: return "zelda";
    case UC_GBA: return "gba (not emulated)";
    case UC_AESND: return "aesnd";
    default: return "?";
    }
}

static int identify(uint32_t crc)
{
    for (size_t i = 0; i < sizeof k_ucodes / sizeof k_ucodes[0]; i++)
        if (k_ucodes[i].crc == crc)
            return k_ucodes[i].kind;
    return UC_UNKNOWN_AX;
}

void gc_dsp_set_ucode(gc_machine *m, int kind, uint32_t crc)
{
    gc_audio *a = m->audio;
    a->qn = 0;
    a->qh = 0;
    a->ucode = kind;
    a->crc = crc;
    a->uploading = false;
    a->up_step = 0;
    switch (kind) {
    case UC_ROM:
        a->rom_param = 0;
        gc_dsp_push_mail(m, 0x8071FEEDu, false, 0); /* the ROM's greeting */
        break;
    case UC_INIT:
        gc_dsp_push_mail(m, 0x80544348u, false, 0);
        break;
    case UC_AX:
    case UC_AX_OLD:
    case UC_UNKNOWN_AX:
        gc_ax_init(m, kind == UC_AX_OLD);
        gc_dsp_push_mail(m, DSP_MAIL_INIT, true, 0);
        break;
    case UC_ASND:
        gc_asnd_init(m, crc);
        gc_dsp_push_mail(m, DSP_MAIL_INIT, true, 0);
        break;
    case UC_ZELDA:
        gc_zelda_init(m, crc); /* its greeting depends on the version */
        break;
    case UC_AESND:
        gc_aesnd_init(m, crc);
        gc_dsp_push_mail(m, DSP_MAIL_INIT, true, 0);
        break;
    default: /* card, and the ones not emulated: the task starts, then nothing */
        gc_dsp_push_mail(m, DSP_MAIL_INIT, true, 0);
        break;
    }
}

/* A ucode starts from an image in main memory (the ROM's boot, or a switch from a running ucode). */
static void boot_image(gc_machine *m, uint32_t addr, uint32_t len, uint32_t start_pc, bool from_rom)
{
    gc_audio *a = m->audio;
    uint32_t pa = dsp_pa(addr);
    uint32_t n = pa < GC_RAM_SIZE ? (len < GC_RAM_SIZE - pa ? len : GC_RAM_SIZE - pa) : 0;
    uint32_t crc = gc_ucode_hash(m->h.ram + pa, n);
    int kind = identify(crc);
    a->booted++;
    if (kind == UC_UNKNOWN_AX)
        a->unknown++;
    fprintf(stderr, "gc: dsp: ucode %08X, %u bytes from %08X, start %04X: %s%s\n", crc, len, addr, start_pc,
            ucode_name(kind), kind == UC_UNKNOWN_AX ? " -- please report this hash" : "");
    if (!from_rom && a->prev_ucode != UC_ROM && crc == a->prev_crc) { /* back to the ucode we left: it resumes */
        int k = a->ucode;
        uint32_t c = a->crc;
        gc_ax ax = a->ax;
        gc_asnd asnd = a->asnd;
        gc_aesnd aesnd = a->aesnd;
        a->ucode = a->prev_ucode;
        a->crc = a->prev_crc;
        a->ax = a->prev_ax;
        a->asnd = a->prev_asnd;
        a->aesnd = a->prev_aesnd;
        a->prev_ucode = k;
        a->prev_crc = c;
        a->prev_ax = ax;
        a->prev_asnd = asnd;
        a->prev_aesnd = aesnd;
        { /* 6 KB: swapped in place rather than through the stack */
            uint8_t *x = (uint8_t *)&a->zelda, *y = (uint8_t *)&a->prev_zelda;
            for (size_t i = 0; i < sizeof a->zelda; i++) {
                uint8_t t = x[i];
                x[i] = y[i];
                y[i] = t;
            }
        }
        a->qn = a->qh = 0;
        a->uploading = false;
        a->up_step = 0;
        if (a->ucode == UC_AX || a->ucode == UC_AX_OLD || a->ucode == UC_UNKNOWN_AX)
            a->ax.state = 0;
        gc_dsp_push_mail(m, DSP_MAIL_RESUME, true, 0);
        return;
    }
    if (!from_rom) {
        a->prev_ucode = a->ucode;
        a->prev_crc = a->crc;
        a->prev_ax = a->ax;
        a->prev_asnd = a->asnd;
        a->prev_zelda = a->zelda;
        a->prev_aesnd = a->aesnd;
    } else {
        a->prev_ucode = UC_ROM;
    }
    gc_dsp_set_ucode(m, kind, crc);
}

void gc_dsp_upload_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    a->up[a->up_step++] = mail;
    if (a->up_step < 10)
        return;
    /* 0 MRAM dest, 1 size, 2 DRAM source (the outgoing ucode's DMEM saved: not modelled), 3 IRAM image in MRAM,
     * 4 its size, 5 IRAM dest, 6 start PC, 7 DRAM image, 8 size, 9 DRAM dest */
    a->up_step = 0;
    a->uploading = false;
    boot_image(m, a->up[3], a->up[4] & 0xFFFF, a->up[6] & 0xFFFF, false);
}

static void rom_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    if (!a->rom_param) {
        if ((mail & 0xFFFF0000u) == 0x80F30000u)
            a->rom_param = mail;
        else
            gc_dsp_push_mail(m, 0xFEEE0000u | (mail & 0xFFFF), false, 0);
        return;
    }
    switch (a->rom_param) {
    case 0x80F3A001u: a->boot_addr = mail; break;
    case 0x80F3A002u: a->boot_len = mail & 0xFFFF; break;
    case 0x80F3B002u: a->boot_dmem_len = mail & 0xFFFF; break;
    case 0x80F3C002u: a->boot_imem = mail & 0xFFFF; break;
    case 0x80F3D001u:
        a->rom_param = 0;
        boot_image(m, a->boot_addr, a->boot_len, mail & 0xFFFF, true);
        return;
    default: break;
    }
    a->rom_param = 0;
}

/* The ucodes we only start: the card unlock answers done, the rest take task mails and nothing else. */
static void other_mail(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    if (mail == CPU_MAIL_NEW_UCODE) {
        a->uploading = true;
        a->up_step = 0;
    } else if (mail == CPU_MAIL_RESET) {
        gc_dsp_set_ucode(m, UC_ROM, 0);
    } else if (a->ucode == UC_CARD) {
        gc_dsp_push_mail(m, DSP_MAIL_DONE, true, 0);
    } else if (m->verbose) {
        fprintf(stderr, "gc: dsp: mail %08X to a ucode not emulated (%s)\n", mail, ucode_name(a->ucode));
    }
}

static void mail_to_dsp(gc_machine *m, uint32_t mail)
{
    gc_audio *a = m->audio;
    a->mails_in++;
    if (m->verbose >= 2)
        fprintf(stderr, "gc: dsp: mail to DSP %08X (%s)\n", mail, ucode_name(a->ucode));
    if (a->uploading) {
        gc_dsp_upload_mail(m, mail);
        return;
    }
    switch (a->ucode) {
    case UC_ROM: rom_mail(m, mail); break;
    case UC_INIT: break;
    case UC_AX:
    case UC_AX_OLD:
    case UC_UNKNOWN_AX: gc_ax_mail(m, mail); break;
    case UC_ASND: gc_asnd_mail(m, mail); break;
    case UC_ZELDA: gc_zelda_mail(m, mail); break;
    case UC_AESND: gc_aesnd_mail(m, mail); break;
    default: other_mail(m, mail); break;
    }
}

/* ---- ARAM DMA ---- */

static void aram_dma(gc_machine *m)
{
    gc_audio *a = m->audio;
    uint32_t mm = ((uint32_t)a->reg[0x10] << 16 | a->reg[0x11]) & 0x03FFFFE0u;
    uint32_t ar = ((uint32_t)a->reg[0x12] << 16 | a->reg[0x13]) & 0x03FFFFE0u;
    uint32_t cnt = ((uint32_t)(a->reg[0x14] & 0x03FF) << 16 | a->reg[0x15]) & ~31u;
    bool to_mram = a->reg[0x14] & 0x8000;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t x = (ar + i) & (GC_ARAM_SIZE - 1), p = mm + i;
        if (p >= GC_RAM_SIZE)
            break;
        if (to_mram)
            m->h.ram[p] = m->aram[x];
        else
            m->aram[x] = m->h.ram[p];
    }
    a->aram_dmas++;
    /* the registers move on; the count reads 0 once done (the transfer: 246 cycles per 32 bytes) */
    a->reg[0x10] = (uint16_t)((mm + cnt) >> 16);
    a->reg[0x11] = (uint16_t)(mm + cnt);
    a->reg[0x12] = (uint16_t)((ar + cnt) >> 16);
    a->reg[0x13] = (uint16_t)(ar + cnt);
    a->reg[0x14] &= 0x8000;
    a->reg[0x15] = 0;
    a->csr |= DSPCR_DMABUSY;
    a->aram_done_at = m->h.now + (uint64_t)(cnt / 32) * 246 + 1;
    gc_schedule(m);
}

/* ---- registers ---- */

uint16_t gc_dsp_read16(gc_machine *m, unsigned idx)
{
    gc_audio *a = m->audio;
    switch (idx) {
    case 0x00: return (uint16_t)(a->cpu_mail >> 16);
    case 0x01: return (uint16_t)a->cpu_mail;
    case 0x02: return mail_from_hi(m);
    case 0x03: return mail_from_lo(m);
    case 0x05:
        if ((a->csr & DSPCR_INITCODE) && m->h.now >= a->initcode_clear_at)
            a->csr &= ~DSPCR_INITCODE;
        return a->csr;
    case 0x0B: return 1;   /* AR_MODE: the controller is set up */
    case 0x1D: return gc_ai_dma_blocks_left(m);
    default: return idx < 0x100 ? a->reg[idx] : 0;
    }
}

void gc_dsp_write16(gc_machine *m, unsigned idx, uint16_t v)
{
    gc_audio *a = m->audio;
    if (m->verbose >= 3)
        fprintf(stderr, "gc: DSP[%02X] = %04X\n", idx * 2, v);
    switch (idx) {
    case 0x00:
        a->cpu_mail = (a->cpu_mail & 0xFFFF) | (uint32_t)v << 16;
        return;
    case 0x01:
        a->cpu_mail = (a->cpu_mail & 0xFFFF0000u) | v;
        mail_to_dsp(m, a->cpu_mail);
        a->cpu_mail &= 0x7FFFFFFFu; /* taken at once */
        return;
    case 0x02:
    case 0x03:
        return; /* the DSP's mailbox: read only */
    case 0x05: {
        uint16_t old = a->csr;
        uint16_t ints = (uint16_t)(old & (DSPCR_AIDINT | DSPCR_ARINT | DSPCR_DSPINT) & ~v); /* write 1 to clear */
        uint16_t keep = (uint16_t)(old & (DSPCR_DMABUSY | DSPCR_INITCODE));
        a->csr = (uint16_t)((v & (DSPCR_HALT | DSPCR_AIDMASK | DSPCR_ARMASK | DSPCR_DSPMASK | DSPCR_INIT)) | ints | keep);
        if (v & DSPCR_RES) { /* reset: back to the ROM, the audio DMA stops */
            gc_dsp_set_ucode(m, UC_ROM, 0);
            uint16_t ctl = a->reg[0x1B];
            a->reg[0x1B] = 0;
            gc_ai_dma_control(m, ctl, 0);
        }
        if ((old & DSPCR_INIT) && !(v & DSPCR_INIT)) { /* the init code runs from 0x81000000 for a moment */
            gc_dsp_set_ucode(m, UC_INIT, 1);
            a->csr |= DSPCR_INITCODE;
            a->initcode_clear_at = m->h.now + 130ull * GC_TB_DIV;
        }
        dsp_pi_update(m);
        return;
    }
    case 0x09: a->reg[idx] = v & 0x007F; return;   /* AR_INFO */
    case 0x0B: return;                             /* AR_MODE: read only */
    case 0x0D: a->reg[idx] = v & 0x07FF; return;   /* AR_REFRESH */
    case 0x10: case 0x12: a->reg[idx] = v & 0x03FF; return;
    case 0x11: case 0x13: a->reg[idx] = v & 0xFFE0; return;
    case 0x14: a->reg[idx] = v & 0x83FF; return;
    case 0x15:
        a->reg[idx] = v & 0xFFE0;
        aram_dma(m);
        return;
    case 0x18: a->reg[idx] = v & 0x03FF; return;   /* audio DMA start */
    case 0x19: a->reg[idx] = v & 0xFFE0; return;
    case 0x1B: {
        uint16_t old = a->reg[0x1B];
        a->reg[0x1B] = v;
        gc_ai_dma_control(m, old, v);
        return;
    }
    case 0x1D: return;                             /* blocks left: read only */
    default:
        if (idx < 0x100)
            a->reg[idx] = v;
        return;
    }
}

/* ---- events ---- */

uint64_t gc_dsp_next_event(gc_machine *m)
{
    gc_audio *a = m->audio;
    uint64_t t = a->dspint_at;
    if (a->aidint_at < t)
        t = a->aidint_at;
    if (a->aram_done_at < t)
        t = a->aram_done_at;
    return t;
}

void gc_dsp_events(gc_machine *m)
{
    gc_audio *a = m->audio;
    uint64_t now = m->h.now;
    bool upd = false;
    if (now >= a->dspint_at) {
        a->dspint_at = UINT64_MAX;
        a->csr |= DSPCR_DSPINT;
        upd = true;
    }
    if (now >= a->aidint_at) {
        a->aidint_at = UINT64_MAX;
        a->csr |= DSPCR_AIDINT;
        upd = true;
    }
    if (now >= a->aram_done_at) {
        a->aram_done_at = UINT64_MAX;
        a->csr = (uint16_t)((a->csr & ~DSPCR_DMABUSY) | DSPCR_ARINT);
        upd = true;
    }
    if (upd)
        dsp_pi_update(m);
}

/* ---- the machine's side ---- */

bool gc_audio_init(gc_machine *m)
{
    gc_audio *a = calloc(1, sizeof *a);
    if (!a)
        return false;
    m->audio = a;
    a->csr = DSPCR_HALT | DSPCR_INIT; /* power-on: halted, the init code not run */
    a->reg[0x0D] = 156;               /* AR_REFRESH */
    a->dspint_at = a->aidint_at = a->aram_done_at = UINT64_MAX;
    a->prev_ucode = UC_ROM;
    gc_dsp_set_ucode(m, UC_ROM, 0);
    a->mails_out = 0;
    gc_ai_init(m);
    return true;
}

void gc_audio_free(gc_machine *m)
{
    free(m->audio);
    m->audio = NULL;
}

uint64_t gc_audio_next_event(gc_machine *m)
{
    uint64_t t = gc_dsp_next_event(m), u = gc_ai_next_event(m);
    return t < u ? t : u;
}

void gc_audio_event(gc_machine *m)
{
    gc_dsp_events(m);
    gc_ai_events(m);
}

void gc_audio_get_info(const gc_machine *m, gc_audio_info *out)
{
    const gc_audio *a = m->audio;
    memset(out, 0, sizeof *out);
    out->ucode_crc = a->crc;
    out->ucode = ucode_name(a->ucode);
    out->ucodes_booted = a->booted;
    out->unknown_ucodes = a->unknown;
    out->mails_to_dsp = a->mails_in;
    out->mails_from_dsp = a->mails_out;
    out->cmd_lists = a->cmd_lists;
    out->dma_blocks = a->dma_blocks;
    out->stream_frames = a->stream_frames;
    out->frames_out = a->out_done;
    out->overruns = a->overruns;
    out->aram_dmas = a->aram_dmas;
    out->ai_control = a->aicr;
}
