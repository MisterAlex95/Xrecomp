/*
 * apu_dsp_watch.h -- what a title asks of the APU's DSPs (XBA-122).
 *
 * The GP and EP are not emulated (apu_dsp.c). This records what the title
 * does with them, so a run tells which titles depend on the DSPs and how:
 *
 *   - the GP doorbell commands the ack answered (count, distinct values),
 *     and doorbells left pending because nothing could answer them;
 *   - the effects image the title loaded into GP P memory (size, hash,
 *     how many times it changed) and how often it rewrote GP X/Y memory
 *     (the image's parameters: DirectSound writes the I3DL2 listener and
 *     the effect settings there);
 *   - the EP image (size, hash), and the GPRST/EPRST words;
 *   - which mixbins carried signal (bed, XTLK, I3DL2 send, FX sends).
 *
 * It goes to stderr as [APU-DSP] lines and into the status file as the
 * "apu_dsp" object (src/kernel/recomp_status.h). RECOMP_APU_DSP_DUMP=<dir>
 * also writes each distinct GP/EP image to <dir> -- that is the title's
 * data: keep it out of the repository.
 *
 * GP and EP memory is plain RAM in the aperture (xbox_memory_layout.c traps
 * only 0x00000-0x2FFFF), so a title's *reads* of DSP memory are invisible
 * here: only what it wrote is seen. docs/technical/apu-dsp.md.
 *
 * Everything runs on the APU frame thread. This project's own code (MIT).
 */
#ifndef APU_DSP_WATCH_H
#define APU_DSP_WATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Aperture offsets (from 0xFE800000) of the DSP windows, apu_regs.h. */
#define APU_GP_BASE   0x30000u
#define APU_EP_BASE   0x50000u
#define APU_DSP_XMEM  0x0000u   /* GP X: 0x0000-0x4FFF */
#define APU_DSP_MIXBUF 0x5000u  /* GP mix buffers: 0x5000-0x5FFF */
#define APU_DSP_YMEM  0x6000u   /* 0x6000-0x9FFF */
#define APU_DSP_PMEM  0xA000u   /* 0xA000-0xFFFB */
#define APU_DSP_RST   0xFFFCu

/* Why a doorbell could not be answered. */
enum {
    APU_DSP_PEND_RESET = 0,     /* GP held in reset (trapped GPRST write) */
    APU_DSP_PEND_COUNT
};

typedef struct ApuDspWatchStats {
    uint64_t frames;
    uint64_t cmds;              /* doorbells acknowledged */
    uint32_t cmd_kinds;         /* distinct doorbell values seen */
    uint32_t cmd_value[16];     /* the first 16 distinct values */
    uint64_t cmd_count[16];
    uint64_t pending_frames;    /* frames with a doorbell nobody could answer */
    uint64_t pending_max;       /* longest such run, in frames */
    uint32_t stalls;            /* runs longer than APU_DSP_STALL_FRAMES */
    uint32_t gp_images;         /* distinct GP P-memory contents seen */
    uint64_t gp_image_hash;     /* the current one (0: none) */
    uint32_t gp_image_words;    /* last non-zero P word + 1 */
    uint32_t gp_param_updates;  /* checks where GP X/Y changed, image unchanged */
    uint32_t gp_rst, ep_rst;    /* last GPRST/EPRST words in the aperture */
    uint32_t gp_rst_changes;
    uint32_t ep_images;
    uint64_t ep_image_hash;
    uint32_t ep_image_words;
    uint32_t bins_mask;         /* bins that ever carried signal */
    uint64_t bin_frames[32];    /* frames each bin carried signal */
    uint64_t reverb_frames;     /* frames the native reverb ran */
    uint64_t reverb_bypassed;   /* frames it was on but bypassed (image not listed) */
} ApuDspWatchStats;

/* A doorbell nobody answers for this many frames (1.5 s) counts as a stall
 * and is logged. */
#define APU_DSP_STALL_FRAMES 2250

void apu_dsp_watch_reset(void);

/* From the doorbell ack: the value it cleared at phys. */
void apu_dsp_watch_cmd(uint32_t phys, uint32_t value);
/* Once per poll: whether a known doorbell is set and could not be answered
 * (reason: APU_DSP_PEND_*), or -1 when none is pending. */
void apu_dsp_watch_pending(int reason, uint32_t phys);

/* Once per frame, with the frame's mixbins and the aperture (NULL: no DSP
 * memory to look at). Checks the DSP memory every APU_DSP_WATCH_PERIOD
 * frames. */
#define APU_DSP_WATCH_PERIOD 64
void apu_dsp_watch_frame(const float bins[32][32], const uint8_t *aperture);

/* The native reverb's frames (ran / bypassed). */
void apu_dsp_watch_reverb(int ran);

const ApuDspWatchStats *apu_dsp_watch_stats(void);

/* The "apu_dsp" status object, as JSON (no trailing newline). */
int apu_dsp_watch_json(char *buf, int size);

/* FNV-1a 64 over n 32-bit words (what the image hash is). */
uint64_t apu_dsp_hash_words(const uint32_t *w, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* APU_DSP_WATCH_H */
