/* The voice processor against a small fake guest RAM.
 *
 * apu_vp.c is compiled into this file so its static helpers can be called
 * directly. Each check below fails on the code it was written for:
 *   - adpcm_*: the decode buffers held two 36-byte blocks' worth, so a
 *     wider samples-per-block value overran them on the stack (a mono
 *     voice already at two).
 *   - voice_lock_*: VOICE_LOCK took d->lock, which the frame thread holds for
 *     a whole frame of voices, so a title's lock waited out the frame.
 *   - gain_*: a volume change stepped at the frame boundary instead of
 *     ramping across the frame.
 *   - mono_hrtf_*: a mono 3D voice was filtered into two HRTF channels, but
 *     the bin mixer still selected channels using the one-channel source
 *     format and copied the left result into every bin.
 *   - pcm_*: every PCM sample resolved its SGE entry and each channel's
 *     address through mcpx_apu_phys (three calls a stereo sample); now once
 *     per page, the samples the same across an SGE page boundary.
 *   - guest_*: values the title wrote (a zero-length stream segment, voice
 *     handles and HRTF entries past the arrays, a misaligned SSL page) hit
 *     asserts that aborted a build without NDEBUG, or indexed past the VP's
 *     arrays in one with it; now ignored.
 *
 * Run with: ctest --test-dir <build> */
#include "../../src/apu/apu_vp.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the VP takes from the rest of the APU. */
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;

#define RAM_SIZE   0x100000u
#define VOICE_BASE 0x10000u   /* NV_PAPU_VPVADDR */
#define SGE_BASE   0x20000u   /* NV_PAPU_VPSGEADDR */
#define DATA_BASE  0x30000u   /* what SGE entry 0 names */

static uint8_t *s_ram;
static int s_failures;
static unsigned long s_phys_calls;

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    s_phys_calls++;
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: guest access at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_ram + addr;
}

const char *mcpx_apu_phys_class(uint64_t addr) { (void)addr; return "fake"; }
int apu_env_on(const char *name) { (void)name; return 0; }
ApuIrqStats apu_irq_stats;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

static void set_voice_reg(uint16_t v, uint32_t reg, uint32_t value)
{
    memcpy(s_ram + VOICE_BASE + v * NV_PAVS_SIZE + reg, &value, 4);
}

/* Voice 0: an ADPCM mono buffer voice whose block holds `blocks` * 36 bytes,
 * the data read through SGE entries 0 and 1. */
static void setup_adpcm_voice(MCPXAPUState *d, unsigned blocks)
{
    uint32_t fmt = ((uint32_t)NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM << 30) |
                   (((blocks - 1) & 0x1F) << 16);
    uint32_t sge[4] = { DATA_BASE, 0, DATA_BASE + 0x1000u, 0 };

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d->regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    memcpy(s_ram + SGE_BASE, sge, sizeof(sge));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT, fmt);
    set_voice_reg(0, NV_PAVS_VOICE_PAR_NEXT, 0x1000);         /* EBO */

    /* A block header (predictor 0, step index 20) and varied nibbles. */
    s_ram[DATA_BASE + 2] = 20;
    for (unsigned i = 4; i < 36u * blocks; i++)
        s_ram[DATA_BASE + i] = (uint8_t)(i * 37u + 11u);
}

static void adpcm_wide_block_decodes(void)
{
    static MCPXAPUState d;
    float samples[NUM_SAMPLES_PER_FRAME][2];
    int16_t expect[65 * ADPCM_MAX_BLOCKS * 2];

    for (unsigned blocks = 1; blocks <= ADPCM_MAX_BLOCKS; blocks++) {
        setup_adpcm_voice(&d, blocks);
        adpcm_decode_block(expect, s_ram + DATA_BASE, 36u * blocks, 1);
        memset(samples, 0, sizeof(samples));
        int n = voice_get_samples(&d, 0, samples, NUM_SAMPLES_PER_FRAME);
        CHECK(n == NUM_SAMPLES_PER_FRAME, "%u blocks: %d samples", blocks, n);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            if (samples[i][0] != int16_to_float(expect[i])) {
                CHECK(0, "%u blocks: sample %d is %f, decoder says %f",
                      blocks, i, samples[i][0], int16_to_float(expect[i]));
                break;
            }
    }
}

static void adpcm_whole_field_fits(void)
{
    /* samples-per-block is 5 bits plus one: the buffers hold every value
     * the descriptor can name, so adpcm_wide_block_decodes covers them all
     * and no voice is ever refused. */
    CHECK(ADPCM_MAX_BLOCKS >= 0x1F + 1, "ADPCM_MAX_BLOCKS %d < 32", ADPCM_MAX_BLOCKS);
}

static MCPXAPUState s_lock_d;
static volatile LONG s_lock_done;

static DWORD WINAPI lock_voice_3(LPVOID arg)
{
    (void)arg;
    voice_lock(&s_lock_d, 3, true);
    InterlockedExchange(&s_lock_done, 1);
    return 0;
}

static int lock_finishes_within(DWORD ms)
{
    HANDLE t;
    DWORD r;

    InterlockedExchange(&s_lock_done, 0);
    t = CreateThread(NULL, 0, lock_voice_3, NULL, 0, NULL);
    r = WaitForSingleObject(t, ms);
    CloseHandle(t);
    return r == WAIT_OBJECT_0 && s_lock_done;
}

static void voice_lock_waits_only_for_its_voice(void)
{
    MCPXAPUState *d = &s_lock_d;
    HANDLE t;

    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);

    /* The frame thread holds d->lock for a whole frame of voices. */
    qemu_mutex_lock(&d->lock);
    CHECK(lock_finishes_within(1000),
          "VOICE_LOCK waited for d->lock, which the frame holds");
    qemu_mutex_unlock(&d->lock);
    CHECK(is_voice_locked(d, 3), "voice 3 is not locked");
    CHECK(!is_voice_locked(d, 4), "voice 4 is locked");

    /* While the processor mixes voice 3, a lock on it waits for that. */
    voice_lock(d, 3, false);
    voice_spin_lock(d, 3);
    InterlockedExchange(&s_lock_done, 0);
    t = CreateThread(NULL, 0, lock_voice_3, NULL, 0, NULL);
    CHECK(WaitForSingleObject(t, 200) == WAIT_TIMEOUT,
          "VOICE_LOCK did not wait while voice 3 was being mixed");
    voice_spin_unlock(d, 3);
    CHECK(WaitForSingleObject(t, 1000) == WAIT_OBJECT_0,
          "VOICE_LOCK did not finish once voice 3 was mixed");
    CloseHandle(t);
    CHECK(is_voice_locked(d, 3), "voice 3 is not locked");
}

/* Voice 100 (not 3D, so no HRTF) in multipass mode: its input is mixbin 20,
 * held at 1.0, and only bin 1 is audible. Full volume for a frame, then
 * silent: the second frame must fall from the first's level to nothing. */
static void gain_ramps_across_a_frame(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const uint16_t v = 100;
    const uint32_t vola_silent = 0xFFFFFFFFu;  /* every volume 0xFFF: off */
    const uint32_t vola_full = vola_silent & ~NV_PAVS_VOICE_TAR_VOLA_VOLUME0;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    set_voice_reg(v, NV_PAVS_VOICE_CFG_FMT,
                  NV_PAVS_VOICE_CFG_FMT_MULTIPASS | (20u << 16));
    set_voice_reg(v, NV_PAVS_VOICE_PAR_STATE,
                  NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);  /* envelopes off: 1.0 */
    set_voice_reg(v, NV_PAVS_VOICE_CFG_VBIN, 1);              /* V0BIN = 1 */
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        mixbins[20][i] = 1.0f;

    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLA, vola_full);
    voice_process(&d, mixbins, d.vp.sample_buf, v, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        if (mixbins[1][i] != 1.0f) {
            CHECK(0, "frame 1 sample %d is %f, expected the voice's level 1.0",
                  i, mixbins[1][i]);
            break;
        }

    memset(mixbins[1], 0, sizeof(mixbins[1]));
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLA, vola_silent);
    voice_process(&d, mixbins, d.vp.sample_buf, v, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float expect = 1.0f - (float)(i + 1) / NUM_SAMPLES_PER_FRAME;
        if (fabsf(mixbins[1][i] - expect) > 1e-6f) {
            CHECK(0, "frame 2 sample %d is %f, expected the ramp's %f",
                  i, mixbins[1][i], expect);
            break;
        }
    }
}

static void mono_hrtf_keeps_binaural_channels(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const uint16_t v = 0;
    const uint32_t vola_binaural =
        0xFFFFFFFFu &
        ~NV_PAVS_VOICE_TAR_VOLA_VOLUME0 &
        ~NV_PAVS_VOICE_TAR_VOLA_VOLUME1;
    HrtfFilter *hrtf;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    memset(mixbins, 0, sizeof(mixbins));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d.vp.hrtf_submix[0] = 6;
    d.vp.hrtf_submix[1] = 7;
    d.vp.hrtf_submix[2] = 8;
    d.vp.hrtf_submix[3] = 9;

    set_voice_reg(v, NV_PAVS_VOICE_CFG_FMT,
                  NV_PAVS_VOICE_CFG_FMT_MULTIPASS | (20u << 16));
    set_voice_reg(v, NV_PAVS_VOICE_PAR_STATE,
                  NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    set_voice_reg(v, NV_PAVS_VOICE_CFG_HRTF_TARGET, 0);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLA, vola_binaural);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        mixbins[20][i] = 1.0f;

    hrtf = &d.vp.filters[v].hrtf;
    hrtf->ch[0].hrir_coeff_cur[0] = 1.0f;
    hrtf->ch[0].hrir_coeff_tar[0] = 1.0f;
    hrtf->ch[1].hrir_coeff_cur[0] = -1.0f;
    hrtf->ch[1].hrir_coeff_tar[0] = -1.0f;

    voice_process(&d, mixbins, d.vp.sample_buf, v, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        if (fabsf(mixbins[6][i] - 1.0f) > 1e-6f ||
            fabsf(mixbins[7][i] + 1.0f) > 1e-6f) {
            CHECK(0,
                  "mono HRTF sample %d routed left/right as %f/%f, expected 1/-1",
                  i, mixbins[6][i], mixbins[7][i]);
            break;
        }
    }
}

/* Voice 0: S16 stereo buffer voice, read through SGE entries 0 and 1, which
 * name pages that are not next to each other; the frame starts 16 frames
 * before the end of the first page. */
static void pcm_resolves_once_per_page(void)
{
    static MCPXAPUState d;
    float samples[NUM_SAMPLES_PER_FRAME][2];
    const uint32_t page1 = 0x50000u, ba = 0x1000u - 16u * 4u;
    uint32_t sge[4] = { DATA_BASE, 0, page1, 0 };
    uint32_t fmt = (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                   (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
                   NV_PAVS_VOICE_CFG_FMT_STEREO | (1u << 16);  /* 2 samples a block */

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d.regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    memcpy(s_ram + SGE_BASE, sge, sizeof(sge));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT, fmt);
    set_voice_reg(0, NV_PAVS_VOICE_CUR_PSL_START, ba);
    set_voice_reg(0, NV_PAVS_VOICE_PAR_NEXT, 0x1000);         /* EBO */
    for (int k = 0; k < NUM_SAMPLES_PER_FRAME; k++) {
        uint32_t lin = ba + 4u * (uint32_t)k;
        uint32_t phys = sge[2 * (lin / 4096u)] + lin % 4096u;
        int16_t l = (int16_t)(1000 + k), r = (int16_t)(-1000 - k);
        memcpy(s_ram + phys, &l, 2);
        memcpy(s_ram + phys + 2, &r, 2);
    }

    s_phys_calls = 0;
    int n = voice_get_samples(&d, 0, samples, NUM_SAMPLES_PER_FRAME);
    unsigned long calls = s_phys_calls;
    CHECK(n == NUM_SAMPLES_PER_FRAME, "pcm: %d samples", n);
    for (int k = 0; k < n; k++)
        if (samples[k][0] != int16_to_float((int16_t)(1000 + k)) ||
            samples[k][1] != int16_to_float((int16_t)(-1000 - k))) {
            CHECK(0, "pcm: frame %d is %f / %f", k, samples[k][0], samples[k][1]);
            break;
        }
    printf("pcm: %lu physical address resolutions for a %d-sample stereo frame\n",
           calls, NUM_SAMPLES_PER_FRAME);
    /* The voice's own registers take about 15; the samples used to take 96. */
    CHECK(calls < 40, "pcm: %lu resolutions, expected under 40", calls);
}

static void guest_values_are_ignored(void)
{
    static MCPXAPUState d;
    float samples[NUM_SAMPLES_PER_FRAME][2];
    const uint32_t ssl_base = 0x40000u;

    /* A stream voice whose segment descriptor is all zero: skipped. */
    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d.regs[NV_PAPU_VPSSLADDR] = ssl_base;
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_PERSIST |
                  NV_PAVS_VOICE_CFG_FMT_LOOP);
    d.vp.ssl[0].count[0] = 1;
    CHECK(voice_get_samples(&d, 0, samples, NUM_SAMPLES_PER_FRAME) == -1,
          "a zero-length stream segment is not skipped");

    /* Methods naming voices / HRTF entries past the arrays, a misaligned
     * SSL page: each ignored. */
    fe_method(&d, NV1BA0_PIO_VOICE_OFF, 0x1234);
    fe_method(&d, NV1BA0_PIO_VOICE_ON, 0x1234);
    fe_method(&d, NV1BA0_PIO_VOICE_RELEASE, 0x1234);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_VOICE, 0x1000);
    fe_method(&d, NV1BA0_PIO_VOICE_LOCK, 1);
    fe_method(&d, NV1BA0_PIO_SET_VOICE_SSL_A, 0x12345678);
    fe_method(&d, NV1BA0_PIO_SET_VOICE_SSL_B, 0x12345678);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_VOICE, 0);
    fe_method(&d, NV1BA0_PIO_SET_VOICE_TAR_HRTF, 0x1FF);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY, 0x200);
    fe_method(&d, NV1BA0_PIO_SET_HRIR, 0x01020304);
    fe_method(&d, NV1BA0_PIO_SET_HRIR_X, 0x01020304);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_SSL, 0x41);
    CHECK(d.vp.ssl_base_page == 0, "a misaligned SSL page was taken");
    fe_method(&d, NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET + 1, 0);
    CHECK(1, "unreached");
}

int main(void)
{
    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;

    voice_lock_waits_only_for_its_voice();
    adpcm_wide_block_decodes();
    adpcm_whole_field_fits();
    gain_ramps_across_a_frame();
    mono_hrtf_keeps_binaural_channels();
    pcm_resolves_once_per_page();
    guest_values_are_ignored();

    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: every ADPCM block count the descriptor can name decodes, "
         "VOICE_LOCK waits only for its own voice, and volume "
         "changes ramp across a frame; mono HRTF keeps both output channels.");
    return 0;
}
