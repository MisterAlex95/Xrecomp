/*
 * apu_dsp_watch.c -- what a title asks of the APU's DSPs (XBA-122).
 * apu_dsp_watch.h has the what and why. This project's own code (MIT).
 */
#include "apu_dsp_watch.h"
#include "../kernel/recomp_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EP_CANARY_WORD  6u
#define EP_CANARY_VALUE 0x00CCCCCCu
#define SIGNAL_FLOOR    1e-5f        /* -100 dBFS */
#define SEEN_MAX        8

static ApuDspWatchStats s;
static uint64_t s_pending_run;
static uint64_t s_gp_candidate, s_ep_candidate;
static uint64_t s_gp_xy_hash;
static uint64_t s_gp_seen[SEEN_MAX], s_ep_seen[SEEN_MAX];
static int s_rst_read;
static uint64_t s_class_frames[4];   /* bed, xtlk, i3dl2, fx */
static const char *s_dump_dir;
static int s_dump_read;

void apu_dsp_watch_reset(void)
{
    memset(&s, 0, sizeof s);
    s_pending_run = 0;
    s_gp_candidate = s_ep_candidate = 0;
    s_gp_xy_hash = 0;
    memset(s_gp_seen, 0, sizeof s_gp_seen);
    memset(s_ep_seen, 0, sizeof s_ep_seen);
    memset(s_class_frames, 0, sizeof s_class_frames);
    s_rst_read = 0;
    s_dump_read = 0;
    s_dump_dir = NULL;
}

uint64_t apu_dsp_hash_words(const uint32_t *w, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= w[i];
        h *= 1099511628211ull;
    }
    return h;
}

void apu_dsp_watch_cmd(uint32_t phys, uint32_t value)
{
    uint32_t i;
    s.cmds++;
    for (i = 0; i < s.cmd_kinds && i < 16; i++)
        if (s.cmd_value[i] == value) {
            s.cmd_count[i]++;
            return;
        }
    if (s.cmd_kinds < 16) {
        s.cmd_value[s.cmd_kinds] = value;
        s.cmd_count[s.cmd_kinds] = 1;
    }
    s.cmd_kinds++;
    if (s.cmd_kinds <= 16)
        fprintf(stderr, "[APU-DSP] GP doorbell 0x%08X: new command value 0x%08X"
                        " (%u distinct, %llu acknowledged)\n", phys, value,
                s.cmd_kinds, (unsigned long long)s.cmds);
}

void apu_dsp_watch_pending(int reason, uint32_t phys)
{
    static const char *why[APU_DSP_PEND_COUNT] = { "GP held in reset" };
    if (reason < 0) {
        s_pending_run = 0;
        return;
    }
    s.pending_frames++;
    s_pending_run++;
    if (s_pending_run > s.pending_max)
        s.pending_max = s_pending_run;
    if (s_pending_run == APU_DSP_STALL_FRAMES) {
        s.stalls++;
        fprintf(stderr, "[APU-DSP] GP doorbell 0x%08X pending for %u frames (%s):"
                        " the title is waiting on a DSP that cannot answer\n",
                phys, APU_DSP_STALL_FRAMES,
                reason < APU_DSP_PEND_COUNT ? why[reason] : "?");
    }
}

void apu_dsp_watch_reverb(int ran)
{
    if (ran) s.reverb_frames++;
    else     s.reverb_bypassed++;
}

const ApuDspWatchStats *apu_dsp_watch_stats(void)
{
    return &s;
}

/* Last non-zero word + 1 over n words; skip_word/skip_value excludes one word
 * that this runtime writes itself (the EP canary). */
static uint32_t used_words(const uint32_t *w, uint32_t n, uint32_t skip_word,
                           uint32_t skip_value)
{
    while (n > 0) {
        uint32_t v = w[n - 1];
        if (v && !(n - 1 == skip_word && v == skip_value))
            break;
        n--;
    }
    return n;
}

static uint32_t nonzero_words(const uint32_t *w, uint32_t n)
{
    uint32_t i, c = 0;
    for (i = 0; i < n; i++)
        c += w[i] != 0;
    return c;
}

static int seen_before(uint64_t *seen, uint64_t h)
{
    int i;
    for (i = 0; i < SEEN_MAX; i++) {
        if (seen[i] == h)
            return 1;
        if (!seen[i]) {
            seen[i] = h;
            return 0;
        }
    }
    return 0;           /* list full: count it as new */
}

static void dump_image(const char *kind, uint64_t h, const uint8_t *window)
{
    char path[1024];
    FILE *f;
    if (!s_dump_read) {
        const char *d = getenv("RECOMP_APU_DSP_DUMP");
        s_dump_dir = (d && *d) ? d : NULL;
        s_dump_read = 1;
    }
    if (!s_dump_dir)
        return;
    snprintf(path, sizeof path, "%s/%s-%016llx.bin", s_dump_dir, kind,
             (unsigned long long)h);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[APU-DSP] cannot write %s\n", path);
        return;
    }
    fwrite(window, 1, 0x10000, f);
    fclose(f);
    fprintf(stderr, "[APU-DSP] %s window (X/Y/P, 64 KB) written to %s\n", kind, path);
}

#define P_WORDS ((APU_DSP_RST - APU_DSP_PMEM) / 4u)
#define X_WORDS ((APU_DSP_MIXBUF - APU_DSP_XMEM) / 4u)
#define Y_WORDS ((APU_DSP_PMEM - APU_DSP_YMEM) / 4u)

static void check_memory(const uint8_t *ap)
{
    const uint8_t *gp = ap + APU_GP_BASE, *ep = ap + APU_EP_BASE;
    const uint32_t *gpp = (const uint32_t *)(gp + APU_DSP_PMEM);
    const uint32_t *epp = (const uint32_t *)(ep + APU_DSP_PMEM);
    uint32_t gp_rst = *(const volatile uint32_t *)(gp + APU_DSP_RST);
    uint32_t ep_rst = *(const volatile uint32_t *)(ep + APU_DSP_RST);
    uint32_t n;
    uint64_t h;

    if (!s_rst_read || gp_rst != s.gp_rst || ep_rst != s.ep_rst) {
        if (s_rst_read)
            s.gp_rst_changes += gp_rst != s.gp_rst;
        if (s_rst_read || gp_rst || ep_rst)
            fprintf(stderr, "[APU-DSP] GPRST 0x%X EPRST 0x%X (aperture words;"
                            " the GP runs with bits 0 and 1 set)\n", gp_rst, ep_rst);
        s.gp_rst = gp_rst;
        s.ep_rst = ep_rst;
        s_rst_read = 1;
    }

    /* GP image: a new P-memory content counts once it held still for two
     * checks, so a check that lands mid-upload is not an image. */
    n = used_words(gpp, P_WORDS, ~0u, 0);
    h = n ? apu_dsp_hash_words(gpp, n) : 0;
    if (h != s.gp_image_hash) {
        if (h == s_gp_candidate) {
            uint32_t xw = nonzero_words((const uint32_t *)(gp + APU_DSP_XMEM), X_WORDS);
            uint32_t yw = nonzero_words((const uint32_t *)(gp + APU_DSP_YMEM), Y_WORDS);
            int is_new = h && !seen_before(s_gp_seen, h);
            s.gp_image_hash = h;
            s.gp_image_words = n;
            s.gp_images += is_new;
            if (h) {
                fprintf(stderr, "[APU-DSP] GP image loaded: P %u words (%u bytes),"
                                " hash %016llx; X %u, Y %u non-zero words;"
                                " %u distinct image(s) so far\n",
                        n, n * 3, (unsigned long long)h, xw, yw, s.gp_images);
                if (is_new)
                    dump_image("gp", h, gp);
            } else {
                fprintf(stderr, "[APU-DSP] GP P memory cleared\n");
            }
            s_gp_xy_hash = 0;
        }
        s_gp_candidate = h;
    } else {
        s_gp_candidate = h;
    }

    /* GP parameters: X/Y rewritten under an unchanged image. */
    if (s.gp_image_hash) {
        uint64_t xy = apu_dsp_hash_words((const uint32_t *)(gp + APU_DSP_XMEM), X_WORDS)
                    ^ (apu_dsp_hash_words((const uint32_t *)(gp + APU_DSP_YMEM), Y_WORDS) * 31u);
        if (s_gp_xy_hash && xy != s_gp_xy_hash)
            s.gp_param_updates++;
        s_gp_xy_hash = xy;
    }

    /* EP image: the canary at P:6 is this runtime's, not the title's. */
    n = used_words(epp, P_WORDS, EP_CANARY_WORD, EP_CANARY_VALUE);
    h = n ? apu_dsp_hash_words(epp, n) : 0;
    if (h != s.ep_image_hash) {
        if (h == s_ep_candidate) {
            int is_new = h && !seen_before(s_ep_seen, h);
            s.ep_image_hash = h;
            s.ep_image_words = n;
            s.ep_images += is_new;
            if (h) {
                fprintf(stderr, "[APU-DSP] EP image loaded: P %u words, hash %016llx"
                                " (output stays the PCM stereo downmix)\n",
                        n, (unsigned long long)h);
                if (is_new)
                    dump_image("ep", h, ep);
            }
        }
        s_ep_candidate = h;
    } else {
        s_ep_candidate = h;
    }
}

static void publish(void)
{
    char buf[1024];
    if (apu_dsp_watch_json(buf, sizeof buf) > 0)
        recomp_status_apu_dsp(buf);
}

void apu_dsp_watch_frame(const float bins[32][32], const uint8_t *aperture)
{
    int b, i, cls_hit[4] = { 0, 0, 0, 0 };

    s.frames++;
    if (bins) {
        for (b = 0; b < 32; b++) {
            const float *x = bins[b];
            for (i = 0; i < 32; i++)
                if (x[i] > SIGNAL_FLOOR || x[i] < -SIGNAL_FLOOR)
                    break;
            if (i < 32) {
                s.bins_mask |= 1u << b;
                s.bin_frames[b]++;
                cls_hit[b < 6 ? 0 : b < 10 ? 1 : b == 10 ? 2 : 3] = 1;
            }
        }
        for (i = 0; i < 4; i++)
            s_class_frames[i] += cls_hit[i];
    }
    if (s.frames % APU_DSP_WATCH_PERIOD == 0) {
        if (aperture)
            check_memory(aperture);
        publish();
    }
}

int apu_dsp_watch_json(char *buf, int size)
{
    uint32_t top = 0, i;
    uint64_t top_n = 0;
    for (i = 0; i < s.cmd_kinds && i < 16; i++)
        if (s.cmd_count[i] > top_n) {
            top_n = s.cmd_count[i];
            top = s.cmd_value[i];
        }
    return snprintf(buf, (size_t)size,
        "{\"cmds\":%llu,\"cmd_kinds\":%u,\"cmd_top\":\"0x%08X\","
        "\"pending_max\":%llu,\"stalls\":%u,"
        "\"gp_image\":\"%016llx\",\"gp_words\":%u,\"gp_images\":%u,"
        "\"gp_param_updates\":%u,\"gp_rst\":\"0x%X\","
        "\"ep_image\":\"%016llx\",\"ep_words\":%u,"
        "\"bins\":\"0x%08X\",\"bed\":%llu,\"xtlk\":%llu,\"i3dl2\":%llu,\"fx\":%llu,"
        "\"reverb\":%llu,\"reverb_bypassed\":%llu}",
        (unsigned long long)s.cmds, s.cmd_kinds, top,
        (unsigned long long)s.pending_max, s.stalls,
        (unsigned long long)s.gp_image_hash, s.gp_image_words, s.gp_images,
        s.gp_param_updates, s.gp_rst,
        (unsigned long long)s.ep_image_hash, s.ep_image_words,
        s.bins_mask, (unsigned long long)s_class_frames[0],
        (unsigned long long)s_class_frames[1], (unsigned long long)s_class_frames[2],
        (unsigned long long)s_class_frames[3],
        (unsigned long long)s.reverb_frames, (unsigned long long)s.reverb_bypassed);
}
