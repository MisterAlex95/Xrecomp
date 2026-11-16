/* Save states of the APU model (XBA-248): a voice mixed, captured, reset,
 * restored and mixed again must give the samples an uninterrupted run gives.
 *
 * The voice processor (apu_vp.c, compiled into this file) against a small
 * fake guest RAM, as in tests/apu_vp. Voice 100 (2D): 16-bit PCM, looping, pitched
 * up (so the linear resampler carries two source samples and a fractional
 * position from frame to frame), its volume changed at every frame (so each
 * frame ramps from the previous frame's gain, which the model keeps). The
 * voice's own descriptor (position, envelope state) is in the fake RAM,
 * which the test snapshots itself: that is the RAM section's job (XBA-245).
 *
 *   1. reference: FRAMES frames, uninterrupted;
 *   2. the same start, CAPTURE frames, then the model captured
 *      (mcpx_apu_model_save) and the RAM copied; the model reset (zeroed, as
 *      a fresh instance) and the RAM scribbled; RAM and model restored; the
 *      rest mixed: every sample equals the reference;
 *   3. the control: the same with the RAM alone restored differs, so the
 *      model's state is what step 2 carried;
 *   4. refusals: a truncated or foreign payload is refused.
 *
 * Run with: ctest --test-dir <build> */
#include "../../src/apu/apu_vp.c"
#include "../../src/apu/apu_savestate.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the VP and the section take from the rest of the APU. */
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;
ApuIrqStats apu_irq_stats;
int apu_env_on(const char *name) { (void)name; return 0; }
void xa2_clear(void) {}
void mcpx_apu_lock_guest(MCPXAPUState *d) { (void)d; }
void mcpx_apu_unlock_guest(MCPXAPUState *d) { (void)d; }
void mcpx_apu_dsp_state_save(XdsWriter *w) { (void)w; }
int mcpx_apu_dsp_state_load(XdsReader *r) { (void)r; return 1; }
void apu_mixer_state_save(XdsWriter *w) { (void)w; }
int apu_mixer_state_load(XdsReader *r) { (void)r; return 1; }
void mcpx_apu_host_after_load(MCPXAPUState *d) { (void)d; }

#define RAM_SIZE   0x100000u
#define VOICE_BASE 0x10000u   /* NV_PAPU_VPVADDR */
#define SGE_BASE   0x20000u   /* NV_PAPU_VPSGEADDR */
#define DATA_BASE  0x30000u   /* what SGE entry 0 names */
#define LOOP_SAMPLES 700u     /* not a multiple of anything the frame uses */
#define FRAMES     120
#define CAPTURE    47
#define VOICE      100        /* past the 3D voices: no HRTF */

static uint8_t *s_ram;
static int s_failures;

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: guest access at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_ram + addr;
}
const char *mcpx_apu_phys_class(uint64_t addr) { (void)addr; return "fake"; }

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

static void set_voice_reg(uint16_t v, uint32_t reg, uint32_t value)
{
    memcpy(s_ram + VOICE_BASE + v * NV_PAVS_SIZE + reg, &value, 4);
}

/* VOICE alone on the 2D list: 16-bit mono PCM looping over LOOP_SAMPLES,
 * pitch +0x600 (about 1.3 source samples per output), bin 0 audible. */
static void setup(MCPXAPUState *d)
{
    uint32_t sge[2] = { DATA_BASE, 0 };
    uint32_t i;

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d->regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    d->regs[NV_PAPU_TVL2D] = VOICE;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    memcpy(s_ram + SGE_BASE, sge, sizeof(sge));
    for (i = 0; i < LOOP_SAMPLES; i++) {
        int16_t smp = (int16_t)(sinf((float)i * 0.071f) * 12000.0f + (float)((i * 7919u) % 2001u) - 1000.0f);
        memcpy(s_ram + DATA_BASE + i * 2, &smp, 2);
    }
    set_voice_reg(VOICE, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
                  NV_PAVS_VOICE_CFG_FMT_LOOP);
    set_voice_reg(VOICE, NV_PAVS_VOICE_CFG_VBIN, 0);                  /* V0BIN = 0 */
    set_voice_reg(VOICE, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    set_voice_reg(VOICE, NV_PAVS_VOICE_PAR_NEXT, LOOP_SAMPLES - 1);    /* EBO */
    set_voice_reg(VOICE, NV_PAVS_VOICE_CUR_PSH_SAMPLE, 0);             /* LBO */
    set_voice_reg(VOICE, NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu);
    set_voice_reg(VOICE, NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu);
    set_voice_reg(VOICE, NV_PAVS_VOICE_TAR_PITCH_LINK, (0x0600u << 16) | 0xFFFFu);
}

/* Frame f: the volume the title sets for it (a fade with steps), then the
 * mix; bin 0 into out. */
static void frame(MCPXAPUState *d, int f, float out[NUM_SAMPLES_PER_FRAME])
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    uint32_t vol = (uint32_t)((f * 37) % 0x300);                    /* 0: full */
    uint32_t vola = 0xFFFFFFFFu & ~NV_PAVS_VOICE_TAR_VOLA_VOLUME0;

    set_voice_reg(VOICE, NV_PAVS_VOICE_TAR_VOLA, vola | (vol << 4));
    memset(mixbins, 0, sizeof(mixbins));
    mcpx_apu_vp_frame(d, mixbins);
    memcpy(out, mixbins[0], sizeof(mixbins[0]));
}

static float s_ref[FRAMES][NUM_SAMPLES_PER_FRAME];
static float s_got[FRAMES][NUM_SAMPLES_PER_FRAME];

/* Frames CAPTURE.. of s_got against the reference: how many samples differ. */
static int count_diff(void)
{
    int f, i, n = 0;
    for (f = CAPTURE; f < FRAMES; f++)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            n += s_got[f][i] != s_ref[f][i];
    return n;
}

int main(void)
{
    static MCPXAPUState d;
    static uint8_t ram_at_capture[RAM_SIZE];
    XdsWriter w = { 0 };
    XdsReader r;
    int f, i, n, nonzero = 0;

    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;
    g_state = &d;

    /* 1. Reference. */
    setup(&d);
    for (f = 0; f < FRAMES; f++)
        frame(&d, f, s_ref[f]);
    for (f = CAPTURE; f < FRAMES; f++)
        for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            nonzero += s_ref[f][i] != 0.0f;
    CHECK(nonzero > (FRAMES - CAPTURE) * NUM_SAMPLES_PER_FRAME / 2,
          "the voice is silent (%d samples sound)", nonzero);

    /* 2. Capture, reset, restore. */
    setup(&d);
    for (f = 0; f < CAPTURE; f++)
        frame(&d, f, s_got[f]);
    mcpx_apu_model_save(&d, &w);
    CHECK(!w.err, "capture failed");
    memcpy(ram_at_capture, s_ram, RAM_SIZE);
    printf("APU model section: %zu bytes\n", w.len);
    CHECK(w.len < 256u * 1024u, "the section is %zu bytes", w.len);

    memset(&d, 0, sizeof(d));                  /* a fresh instance */
    memset(s_ram, 0x5A, RAM_SIZE);             /* and RAM that is anything */
    memcpy(s_ram, ram_at_capture, RAM_SIZE);   /* the RAM section */
    r = xds_reader(w.p, w.len);
    CHECK(mcpx_apu_model_load(&d, &r) && !r.err && r.pos == r.len,
          "restore failed (read %zu of %zu)", r.pos, w.len);
    for (f = CAPTURE; f < FRAMES; f++)
        frame(&d, f, s_got[f]);
    n = count_diff();
    printf("restored: %d of %d samples differ from the uninterrupted run\n", n,
           (FRAMES - CAPTURE) * NUM_SAMPLES_PER_FRAME);
    CHECK(n == 0, "%d samples differ after the restore", n);

    /* 3. Control: the RAM alone. */
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;      /* what a title would set again */
    d.regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    d.regs[NV_PAPU_TVL3D] = 0xFFFF;
    d.regs[NV_PAPU_TVLMP] = 0xFFFF;
    memcpy(s_ram, ram_at_capture, RAM_SIZE);
    for (f = CAPTURE; f < FRAMES; f++)
        frame(&d, f, s_got[f]);
    n = count_diff();
    printf("RAM alone: %d samples differ\n", n);
    CHECK(n > 0, "the model's state made no difference: the test proves nothing");

    /* 4. Refusals. */
    r = xds_reader(w.p, w.len / 2);
    CHECK(!mcpx_apu_model_load(&d, &r), "a truncated payload was taken");
    {
        uint8_t *bad = (uint8_t *)malloc(w.len);
        memcpy(bad, w.p, w.len);
        bad[0] ^= 0xFF;
        r = xds_reader(bad, w.len);
        CHECK(!mcpx_apu_model_load(&d, &r), "a foreign payload was taken");
        free(bad);
    }
    /* The section framing (xbox_dev_state.h) round-trips one section. */
    {
        XdsWriter c = { 0 };
        XdsReader cr, pl;
        uint32_t tag = 0, ver = 0;
        size_t at = xds_section_begin(&c, XDS_TAG_APU, XDS_APU_VERSION);
        xds_put_bytes(&c, w.p, w.len);
        xds_section_end(&c, at);
        cr = xds_reader(c.p, c.len);
        CHECK(xds_section_next(&cr, &tag, &ver, &pl) && tag == XDS_TAG_APU
              && ver == XDS_APU_VERSION && pl.len == w.len && !memcmp(pl.p, w.p, w.len),
              "section framing");
        CHECK(!xds_section_next(&cr, &tag, &ver, &pl) && !cr.err, "one section only");
        xds_writer_free(&c);
    }

    xds_writer_free(&w);
    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: a voice restored from a captured APU model mixes the samples "
         "an uninterrupted run does; RAM alone does not.");
    return 0;
}
