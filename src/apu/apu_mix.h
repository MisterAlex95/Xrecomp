/*
 * apu_mix.h -- what stands in for the GP's mix: the mixbin downmix to stereo
 * and a native I3DL2-style reverb (XBA-122).
 *
 * This project's own code (MIT, see LICENSE), not derived from xemu: the GP's
 * real job is a DSP56300 program (DirectSound's effects image) that this
 * runtime does not run. docs/technical/apu-dsp.md says why and what this
 * approximates.
 *
 * Mixbin numbering is DirectSound's public DSMIXBIN_* enumeration, which a
 * title's voices are routed by whatever image is loaded:
 *
 *    0 front L    1 front R    2 centre    3 LFE    4 back L    5 back R
 *    6..9  XTLK front L/R, back L/R (HRTF'd 3D voices, crosstalk stage)
 *   10     I3DL2 reverb send (mono)
 *   11..30 FX sends 0..19 (the effects image decides what they feed)
 *   31     unnamed
 */
#ifndef APU_MIX_H
#define APU_MIX_H

#include <stdint.h>

#define APU_MIX_BINS    32
#define APU_MIX_SAMPLES 32           /* one GP frame: 32 samples at 48 kHz */
#define APU_MIX_RATE    48000

#define APU_MIXBIN_I3DL2   10
#define APU_MIXBIN_FXSEND0 11

#ifdef __cplusplus
extern "C" {
#endif

/* How the bins become stereo (RECOMP_APU_MIXDOWN). */
enum {
    APU_MIXDOWN_TWO = 0,    /* bins 0/1 only (RECOMP_APU_MIXDOWN_ALL=0) */
    APU_MIXDOWN_SUM = 1,    /* every bin, even left / odd right, unity (before XBA-122) */
    APU_MIXDOWN_ROUTED = 2, /* by role, standard downmix coefficients (default) */
};

/* The mode from RECOMP_APU_MIXDOWN (two|sum|routed), else
 * RECOMP_APU_MIXDOWN_ALL=0 -> two, else routed. Re-read after
 * apu_mix_reset_config(). */
int apu_mix_mode(void);

/* Left/right gain of bin b in a mode. reverb_on: the I3DL2 send (bin 10) is
 * then the reverb's input and has no dry path. */
void apu_mix_bin_gain(int mode, int b, int reverb_on, float *gl, float *gr);

/* ── I3DL2 listener parameters (the I3DL2 guideline's units) ── */
typedef struct ApuI3dl2 {
    int32_t room, room_hf;              /* mB */
    float   room_rolloff;
    float   decay_time, decay_hf_ratio; /* s, ratio */
    int32_t reflections;                /* mB */
    float   reflections_delay;          /* s */
    int32_t reverb;                     /* mB */
    float   reverb_delay;               /* s */
    float   diffusion, density;         /* % */
    float   hf_reference;               /* Hz */
} ApuI3dl2;

/* The I3DL2 guideline's named presets ("generic", "room", "cave", ...);
 * returns 0 for an unknown name. */
int apu_i3dl2_preset(const char *name, ApuI3dl2 *out);
const char *apu_i3dl2_preset_name(int i);   /* NULL past the last */

/* The reverb. Opaque, allocated once: no allocation on the audio thread. */
typedef struct ApuReverb ApuReverb;
ApuReverb *apu_reverb_create(void);
void apu_reverb_destroy(ApuReverb *rv);
void apu_reverb_set(ApuReverb *rv, const ApuI3dl2 *p);
void apu_reverb_clear(ApuReverb *rv);
/* One frame: mono send in, stereo wet added to out[i][0..1]. */
void apu_reverb_process(ApuReverb *rv, const float *in, float (*out)[2], int n);

/* RECOMP_APU_REVERB: 0 = off (default), else the preset to run
 * (on/1 = generic). *p is filled when on. */
int apu_mix_reverb_config(ApuI3dl2 *p, const char **name);

/* Forget the cached environment (tests). */
void apu_mix_reset_config(void);

/* The whole stage: bins -> stereo float, the reverb fed from bin 10 when rv
 * is non-NULL. out is overwritten. */
void apu_mix_frame(int mode, const float bins[APU_MIX_BINS][APU_MIX_SAMPLES],
                   ApuReverb *rv, float out[APU_MIX_SAMPLES][2]);

#ifdef __cplusplus
}
#endif

#endif /* APU_MIX_H */
