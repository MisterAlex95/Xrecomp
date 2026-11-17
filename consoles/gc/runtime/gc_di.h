/* gc_di.h -- the DVD interface's state, shared by gc_di.c and gc_boot.c (XBA-261). */
#ifndef GC_DI_H
#define GC_DI_H

#include "gc_boot.h"
#include "gc_internal.h"

/* Register indices (offset / 4 from 0xCC006000). */
enum {
    DI_SR = 0, DI_CVR = 1, DI_CMD0 = 2, DI_CMD1 = 3, DI_CMD2 = 4, DI_MAR = 5, DI_LEN = 6, DI_CR = 7, DI_IMM = 8,
    DI_CFG = 9, DI_NREG = 16
};

/* DISR / DICVR / DICR bits. */
enum {
    DISR_BRK = 1u << 0, DISR_DEINTMASK = 1u << 1, DISR_DEINT = 1u << 2, DISR_TCINTMASK = 1u << 3,
    DISR_TCINT = 1u << 4, DISR_BRKINTMASK = 1u << 5, DISR_BRKINT = 1u << 6,
    DICVR_OPEN = 1u << 0, DICVR_INTMASK = 1u << 1, DICVR_INT = 1u << 2,
    DICR_TSTART = 1u << 0, DICR_DMA = 1u << 1, DICR_WRITE = 1u << 2,
};

typedef struct gc_di {
    gc_disc *disc;            /* in the drive (NULL: none) */
    gc_disc *next;            /* goes in when the cover closes */
    bool cover_open;
    uint64_t cover_close_at;  /* UINT64_MAX: not scheduled */
    uint32_t reg[DI_NREG];
    /* the command in flight */
    uint64_t done_at;         /* UINT64_MAX: idle */
    uint32_t cmd0, cmd1, cmd2, mar, len;
    uint32_t error;           /* (status << 24) | sense code, for 0xE0 */
    bool fail;                /* the command in flight ends with DEINT */
    uint64_t head;            /* where the drive's head is (byte offset) */
    bool motor_off;
    /* audio streaming (DTK): the track playing, the one queued behind it (it loops by default), the decoder
     * (XBA-263: gc_di_stream_frame, pulled by the audio interface at its streaming rate while it plays) */
    bool stream_on, stream_stop_at_end;
    uint64_t stream_start, stream_len, stream_pos; /* stream_pos: the next block's offset on the disc */
    uint64_t stream_next_start, stream_next_len;
    uint32_t stream_cfg;
    int16_t stream_blk[56];
    int stream_blk_at;                           /* frames of stream_blk already played (28: none left) */
    int32_t stream_hist[4];
    uint8_t stream_cache[0x8000];
    uint64_t stream_cache_off;
    uint32_t stream_cache_n;
    /* boot */
    uint32_t boot_entry;
    unsigned boot_reads;
} gc_di;

/* The drive state, created on first use. NULL only when out of memory. */
gc_di *gc_di_get(gc_machine *m);

#endif
