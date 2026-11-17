/* gc_dsp_internal.h -- the GameCube sound hardware's state, shared by gc_dsp.c (DSP interface, mailboxes, ucode
 * detection, ARAM DMA, the audio DMA), gc_ai.c (audio interface, mixing to 48 kHz), gc_ax.c and gc_asnd.c (the
 * microcodes' HLE) (XBA-263). */
#ifndef GC_DSP_INTERNAL_H
#define GC_DSP_INTERNAL_H

#include "gc_audio.h"
#include "gc_internal.h"

/* DSP control / status register (0xCC00500A) bits. */
enum {
    DSPCR_RES = 1u << 0,       /* reset (reads 0 once done) */
    DSPCR_PIINT = 1u << 1,     /* the CPU interrupts the DSP (ignored: HLE) */
    DSPCR_HALT = 1u << 2,
    DSPCR_AIDINT = 1u << 3,    /* audio DMA interrupt, write 1 to clear */
    DSPCR_AIDMASK = 1u << 4,
    DSPCR_ARINT = 1u << 5,     /* ARAM DMA done */
    DSPCR_ARMASK = 1u << 6,
    DSPCR_DSPINT = 1u << 7,    /* the DSP's interrupt (mail) */
    DSPCR_DSPMASK = 1u << 8,
    DSPCR_DMABUSY = 1u << 9,   /* an ARAM DMA in flight */
    DSPCR_INITCODE = 1u << 10, /* the init code is running */
    DSPCR_INIT = 1u << 11,     /* libogc's DSPCR_DSPRESET: clearing it runs the init code */
};

/* Mails (Nintendo's task protocol, also libogc's). */
#define DSP_MAIL_INIT 0xDCD10000u
#define DSP_MAIL_RESUME 0xDCD10001u
#define DSP_MAIL_YIELD 0xDCD10002u
#define DSP_MAIL_DONE 0xDCD10003u
#define DSP_MAIL_SYNC 0xDCD10004u
#define CPU_MAIL_RESUME 0xCDD10000u
#define CPU_MAIL_NEW_UCODE 0xCDD10001u
#define CPU_MAIL_RESET 0xCDD10002u
#define CPU_MAIL_CONTINUE 0xCDD10003u

enum gc_ucode {
    UC_ROM, UC_INIT, UC_AX, UC_AX_OLD, UC_ASND, UC_CARD, UC_ZELDA, UC_GBA, UC_AESND, UC_UNKNOWN_AX,
};

#define GC_AX_SAMPLES 160 /* 5 ms at 32 kHz */
#define GC_AX_CMDLIST_MAX 512

typedef struct gc_ax {
    int32_t buf[9][GC_AX_SAMPLES]; /* main L R S, aux A L R S, aux B L R S */
    uint16_t cmd[GC_AX_CMDLIST_MAX];
    uint16_t cmd_size;
    int state;                     /* 0 waiting for a list's size, 1 its address, 2 the next task mail */
    uint16_t compressor_pos;
    bool old;                      /* the 2001 ucode (0x4E8A8B21): no low-pass filter in the PB, other mixer bits */
    bool warned_cmd;
} gc_ax;

typedef struct gc_asnd {
    uint32_t voice_addr;
    bool next_is_voice;
    bool new_flags, swap_lr, has_terminate;
    uint16_t in[32];               /* the 32-byte sample buffer the ucode DMAs into (16 words used) */
    uint16_t out[1024 * 2];        /* its output buffer, right then left */
    uint32_t v[16];                /* the voice structure, 64 bytes as words */
} gc_asnd;

typedef struct gc_mail {
    uint32_t v;
    bool irq_next;                 /* raise DSPINT when this one is read (a later mail asked for one) */
} gc_mail;

#define GC_MAILQ 64
#define GC_AID_FIFO 4096 /* frames */
#define GC_OUT_RING 32768 /* 48 kHz frames kept for the host (0.68 s) */

typedef struct gc_audio {
    /* ---- DSP interface ---- */
    uint16_t csr;
    uint16_t reg[0x100];           /* the 16-bit registers as stored (ARAM / audio DMA addresses, AR_INFO...) */
    uint32_t cpu_mail;             /* CPU -> DSP, bit 31 while not taken */
    gc_mail q[GC_MAILQ];           /* DSP -> CPU, pending */
    unsigned qh, qn;
    uint32_t last_mail;
    uint64_t dspint_at, aidint_at, initcode_clear_at, aram_done_at;
    /* the microcode */
    int ucode;
    uint32_t crc;
    unsigned booted, unknown;
    uint32_t rom_param;            /* the ROM's next parameter (0: waiting for 0x80F3xxxx) */
    uint32_t boot_addr, boot_len, boot_imem, boot_dmem_len;
    bool uploading;                /* a running ucode was told MAIL_NEW_UCODE: the next 10 mails describe the next */
    unsigned up_step;
    uint32_t up[10];
    bool resume_mail;              /* a ucode we switched back to says DSP_RESUME */
    int prev_ucode;                /* the ucode switched away from, and its state, for switching back */
    uint32_t prev_crc;
    gc_ax ax, prev_ax;
    gc_asnd asnd, prev_asnd;
    /* ---- audio DMA (registers in the DSP block) ---- */
    uint32_t aid_cur, aid_left;
    bool aid_on;
    /* ---- audio interface ---- */
    uint32_t aicr, aivr, aiit;
    uint32_t scnt_base;            /* the streaming sample counter at scnt_t0 */
    uint64_t scnt_t0;
    uint64_t aiit_at;
    /* timing: the DMA block tick, the 48 kHz output clock */
    uint64_t tick_at;              /* next 32-byte DMA block (8 frames at the DMA rate) */
    uint64_t out_done;             /* 48 kHz frames produced since power-on */
    /* the DMA's frames waiting for the resampler (R L as the hardware reads them, stored L R) */
    int16_t fifo[GC_AID_FIFO][2];
    unsigned fh, fn;
    gc_resampler rs_dma, rs_stream;
    /* the 48 kHz output for the host */
    int16_t ring[GC_OUT_RING][2];
    unsigned rh, rn;
    /* counters */
    uint64_t mails_in, mails_out, cmd_lists, dma_blocks, stream_frames, overruns, aram_dmas;
    bool warned_unknown;
} gc_audio;

/* gc_dsp.c */
uint16_t gc_dsp_read16(gc_machine *m, unsigned idx);
void gc_dsp_write16(gc_machine *m, unsigned idx, uint16_t v);
void gc_dsp_push_mail(gc_machine *m, uint32_t mail, bool irq, uint64_t delay);
void gc_dsp_set_ucode(gc_machine *m, int ucode, uint32_t crc);
void gc_dsp_irq(gc_machine *m, uint16_t bit);       /* AIDINT / ARINT / DSPINT now */
void gc_dsp_events(gc_machine *m);                  /* due DSP interface events */
uint64_t gc_dsp_next_event(gc_machine *m);
/* A running ucode got MAIL_NEW_UCODE's ten parameters: hand over to the next one. */
void gc_dsp_upload_mail(gc_machine *m, uint32_t mail);

/* physical memory helpers for the HLE (big-endian MEM1, out of range reads 0 / writes dropped) */
static inline uint32_t dsp_pa(uint32_t a) { return a & 0x3FFFFFFFu; }
static inline uint16_t dsp_rd16(gc_machine *m, uint32_t a)
{
    a = dsp_pa(a);
    return a + 2 <= GC_RAM_SIZE ? (uint16_t)(m->h.ram[a] << 8 | m->h.ram[a + 1]) : 0;
}
static inline uint32_t dsp_rd32(gc_machine *m, uint32_t a)
{
    return (uint32_t)dsp_rd16(m, a) << 16 | dsp_rd16(m, a + 2);
}
static inline void dsp_wr16(gc_machine *m, uint32_t a, uint16_t v)
{
    a = dsp_pa(a);
    if (a + 2 <= GC_RAM_SIZE) {
        m->h.ram[a] = (uint8_t)(v >> 8);
        m->h.ram[a + 1] = (uint8_t)v;
    }
}
static inline void dsp_wr32(gc_machine *m, uint32_t a, uint32_t v)
{
    dsp_wr16(m, a, (uint16_t)(v >> 16));
    dsp_wr16(m, a + 2, (uint16_t)v);
}

/* gc_ax.c */
void gc_ax_init(gc_machine *m, bool old);
void gc_ax_mail(gc_machine *m, uint32_t mail);
/* gc_asnd.c */
void gc_asnd_init(gc_machine *m, uint32_t crc);
void gc_asnd_mail(gc_machine *m, uint32_t mail);

/* gc_ai.c */
void gc_ai_dma_control(gc_machine *m, uint16_t old, uint16_t v); /* AUDIO_DMA_CONTROL_LEN written */
uint16_t gc_ai_dma_blocks_left(gc_machine *m);
void gc_ai_events(gc_machine *m);
uint64_t gc_ai_next_event(gc_machine *m);
void gc_ai_init(gc_machine *m);

#endif
