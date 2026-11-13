/* XBA-122 stage 1: what a title asks of the GP/EP is recorded, and every DSP
 * handshake this runtime knows has a bounded answer.
 *
 * Synthetic inputs only: a zeroed buffer stands in for RAM (as in
 * tests/apu_dsp_ack), another for the MCPX aperture, and the "effects
 * images" are pseudo-random words of our own, never an XDK image.
 *
 * Handshakes (docs/technical/apu-dsp.md has the list):
 *   H1 GP doorbell at SGE[0] + 0x810: acknowledged within one poll, each
 *      command value counted.
 *   H2 GP doorbell while the GP is held in reset: not answered (hardware
 *      would not either), counted per frame, a stall logged and counted
 *      after APU_DSP_STALL_FRAMES, answered on the first poll once the GP
 *      runs.
 *   H3 EP canary (0x00CCCCCC at EP P:6): restored every poll, also after
 *      the title loads another EP image (an AC3 encoder).
 *   H4 EP image change (AC3 / Dolby): output stays the PCM stereo downmix,
 *      sample for sample.
 */
#include "apu_state.h"
#include "apu_shim.h"
#include "apu_dsp_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <direct.h>
#define getpid _getpid
#define mkdir_p(p) _mkdir(p)
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(p) mkdir((p), 0700)
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define FAKE_RAM   (256u << 10)
#define SGE_TABLE  0x00010000u
#define GP_PAGE    0x00020000u
#define DOORBELL   (GP_PAGE + 0x810u)
#define APERTURE   (512u << 10)

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *dword(uint8_t *base, uint32_t off)
{
    return (uint32_t *)(base + off);
}

static uint32_t rng = 0x12345678u;
static uint32_t next_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng & 0x00FFFFFFu;            /* DSP56300 words are 24-bit */
}

static float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

static void frames(MCPXAPUState *d, int n)
{
    while (n-- > 0) {
        mcpx_apu_dsp_ack_poll(d);
        mcpx_apu_dsp_frame(d, bins);
        d->ep_frame_div++;
    }
}

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    uint8_t *ram = (uint8_t *)calloc(1, FAKE_RAM);
    uint8_t *ap = (uint8_t *)calloc(1, APERTURE);
    uint8_t *gp, *ep;
    const ApuDspWatchStats *st;
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    uint32_t img1[1500], img2[900];
    int16_t pcm_before[64][2];
    char json[1024], dir[300], path[400];
    const char *tmp = getenv("TMPDIR");
    FILE *f;
    int i;

    if (!d || !ram || !ap) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    gp = ap + APU_GP_BASE;
    ep = ap + APU_EP_BASE;
    snprintf(dir, sizeof dir, "%s/xba_dsp_dump_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    mkdir_p(dir);
    set_env("RECOMP_APU_DSP_DUMP", dir);
    set_env("RECOMP_APU_DSP_ACK", "auto");

    g_apu_ram_ptr = ram;
    d->ram_ptr = ram;
    mcpx_apu_dsp_set_aperture(ap);
    mcpx_apu_dsp_init(d);
    st = apu_dsp_watch_stats();

    puts("H1 GP doorbell");
    d->regs[NV_PAPU_GPSADDR] = SGE_TABLE;
    *dword(ram, SGE_TABLE) = GP_PAGE;
    *dword(ram, DOORBELL) = 0x101;
    frames(d, 1);
    check(*dword(ram, DOORBELL) == 0 && st->cmds == 1, "acknowledged within one poll, counted");
    for (i = 0; i < 10; i++) {
        *dword(ram, DOORBELL) = (i & 1) ? 0x202 : 0x101;
        frames(d, 1);
    }
    check(st->cmds == 11 && st->cmd_kinds == 2, "11 commands, 2 distinct values");

    puts("H2 doorbell while the GP is held in reset");
    mcpx_apu_dsp_gprst_write(0);
    *dword(ram, DOORBELL) = 0x303;
    frames(d, APU_DSP_STALL_FRAMES + 10);
    check(*dword(ram, DOORBELL) == 0x303, "not answered while held (as hardware)");
    check(st->pending_max == APU_DSP_STALL_FRAMES + 10, "pending frames counted");
    check(st->stalls == 1, "one stall logged and counted, once");
    mcpx_apu_dsp_gprst_write(run);
    frames(d, 1);
    check(*dword(ram, DOORBELL) == 0 && st->cmds == 12, "answered on the first poll once the GP runs");
    mcpx_apu_dsp_gprst_write(0);
    *dword(ram, DOORBELL) = 0x303;
    frames(d, 5);
    mcpx_apu_dsp_gprst_write(run);
    frames(d, 1);
    check(st->stalls == 1 && st->pending_max == APU_DSP_STALL_FRAMES + 10,
          "a short hold is not a stall; the longest run is kept");

    puts("GP effects image");
    for (i = 0; i < 1500; i++) img1[i] = next_word();
    for (i = 0; i < 900; i++) img2[i] = next_word();
    *dword(gp, APU_DSP_RST) = run;
    /* The upload straddles a check: half now, the rest after one period. */
    memcpy(gp + APU_DSP_PMEM, img1, 750 * 4);
    frames(d, APU_DSP_WATCH_PERIOD);
    memcpy(gp + APU_DSP_PMEM + 750 * 4, img1 + 750, 750 * 4);
    for (i = 0; i < 300; i++) *dword(gp, APU_DSP_XMEM + 4 * i) = next_word();
    frames(d, 3 * APU_DSP_WATCH_PERIOD);
    check(st->gp_images == 1, "a straddled upload counts as one image");
    check(st->gp_image_words == 1500, "its size: 1500 P words");
    check(st->gp_image_hash == apu_dsp_hash_words(img1, 1500), "its hash");
    check(st->gp_rst == run, "GPRST read from the aperture");
    *dword(gp, APU_DSP_XMEM + 8) ^= 0x10;   /* a parameter write */
    frames(d, APU_DSP_WATCH_PERIOD);
    check(st->gp_param_updates == 1, "an X-memory write under the same image: one update");
    snprintf(path, sizeof path, "%s/gp-%016llx.bin", dir, (unsigned long long)st->gp_image_hash);
    f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); i = (int)ftell(f); fclose(f); remove(path); } else i = -1;
    check(i == 0x10000, "RECOMP_APU_DSP_DUMP wrote the 64 KB GP window");
    memset(gp + APU_DSP_PMEM, 0, 1500 * 4);
    memcpy(gp + APU_DSP_PMEM, img2, sizeof img2);
    frames(d, 2 * APU_DSP_WATCH_PERIOD);
    memset(gp + APU_DSP_PMEM, 0, sizeof img2);
    memcpy(gp + APU_DSP_PMEM, img1, sizeof img1);
    frames(d, 2 * APU_DSP_WATCH_PERIOD);
    check(st->gp_images == 2 && st->gp_image_words == 1500,
          "a second image, then the first again: 2 distinct");

    puts("H3/H4 EP canary and an EP image (AC3)");
    *dword(ep, APU_DSP_PMEM + 6 * 4) = 0;
    frames(d, 1);
    check(*dword(ep, APU_DSP_PMEM + 6 * 4) == 0x00CCCCCCu, "EP canary restored");
    frames(d, 2 * APU_DSP_WATCH_PERIOD);
    check(st->ep_image_hash == 0, "the canary alone is not an EP image");
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        bins[0][i] = 0.25f * (float)((i % 7) - 3) / 3.0f;
        bins[1][i] = -bins[0][i];
        bins[2][i] = 0.1f;
    }
    d->ep_frame_div = 0;
    frames(d, 2);
    memcpy(pcm_before, d->monitor.frame_buf, sizeof pcm_before);
    for (i = 0; i < 1200; i++)                    /* the title's encoder */
        *dword(ep, APU_DSP_PMEM + 4 * i) = next_word();
    frames(d, 2 * APU_DSP_WATCH_PERIOD);
    check(st->ep_image_hash != 0 && st->ep_image_words == 1200, "EP image seen: 1200 words");
    check(*dword(ep, APU_DSP_PMEM + 6 * 4) == 0x00CCCCCCu, "canary kept over the new image");
    d->ep_frame_div = 0;
    frames(d, 2);
    check(!memcmp(pcm_before, d->monitor.frame_buf, sizeof pcm_before)
          && pcm_before[1][0] != 0 && pcm_before[1][1] != 0,
          "output unchanged: PCM stereo downmix, not silence");
    snprintf(path, sizeof path, "%s/ep-%016llx.bin", dir, (unsigned long long)st->ep_image_hash);
    remove(path);
    snprintf(path, sizeof path, "%s/gp-%016llx.bin", dir,
             (unsigned long long)apu_dsp_hash_words(img2, 900));
    check(remove(path) == 0, "the second GP image was dumped too, each image once");

    puts("Mixbins and the status object");
    memset(bins, 0, sizeof bins);
    for (i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        bins[0][i] = 0.1f;
        bins[7][i] = 0.1f;
        bins[10][i] = 0.1f;
        bins[12][i] = 0.1f;
    }
    {
        uint64_t f0 = st->bin_frames[10];
        frames(d, 10);
        check(st->bin_frames[10] == f0 + 10, "bin 10 (I3DL2 send) counted per frame");
    }
    check((st->bins_mask & 0x1485u) == 0x1485u, "bins 0, 2, 7, 10, 12 in the mask");
    apu_dsp_watch_json(json, sizeof json);
    printf("  %s\n", json);
    check(strstr(json, "\"cmds\":13") && strstr(json, "\"stalls\":1")
          && strstr(json, "\"gp_images\":2") && strstr(json, "\"bins\":\"0x"),
          "JSON carries commands, stalls, images, bins");

    mcpx_apu_dsp_finalize(d);
    mcpx_apu_dsp_set_aperture(NULL);
    remove(dir);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    free(ap);
    free(ram);
    free(d);
    return failures ? 1 : 0;
}
