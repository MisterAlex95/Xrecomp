/* The always-on audio health line ("[APU] health", apu_core.c).
 *
 * 1. Stream dry time: one persistent stream voice, active, with both of its
 *    segment lists empty (the title has not refilled them), run through the
 *    real VP for 150 frames (100 ms). The line must report 100 ms of dry
 *    time in one spell; a second flush, with nothing new, prints nothing.
 *
 * 2. A trap held 30 ms: the real frame thread and the real SDL2 backend on
 *    SDL's dummy driver, which drains the queue in real time. Four 256-sample
 *    buffers put the pacing high mark at 1024 samples (21 ms), so a front
 *    end trapped for 30 ms lets the queue run dry. No frame is made while
 *    trapped. The trap comes from the VP (an idle voice on a list, as in a
 *    game), so it begins partway through a period, where no pacing poll
 *    comes until the next submit: the queue is read before that
 *    submit, so the underrun is counted all the same -- exactly one. The
 *    line for the trap's window must say underruns=1, a trap spell of about
 *    30 ms (20 or more: the frame thread can be in a pacing wait when it
 *    begins) and a submit gap of about as long.
 *
 * stderr is captured to a file per case and parsed. Windows: the trap case
 * would open a real XAudio2 device, so it is skipped there. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

extern uint8_t *g_apu_ram_ptr;

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

typedef struct {
    int found;
    double window_s, gap_max_ms, trap_max_ms, dry_ms, dry_max_ms;
    unsigned underruns, underruns_total, traps, dry_spells, dropped;
    int q_min;
} Health;

/* The last health line in the file; with_trap: the last one that has a
 * trap spell in it. */
static Health read_health(const char *path, int with_trap)
{
    Health h;
    char line[512];
    FILE *f = fopen(path, "r");

    memset(&h, 0, sizeof h);
    while (f && fgets(line, sizeof line, f)) {
        Health t;
        memset(&t, 0, sizeof t);
        if (sscanf(line, "[APU] health window_s=%lf underruns=%u underruns_total=%u q_min=%d"
                         " gap_max_ms=%lf traps=%u trap_max_ms=%lf dry_ms=%lf dry_spells=%u"
                         " dry_max_ms=%lf dropped=%u",
                   &t.window_s, &t.underruns, &t.underruns_total, &t.q_min, &t.gap_max_ms,
                   &t.traps, &t.trap_max_ms, &t.dry_ms, &t.dry_spells, &t.dry_max_ms,
                   &t.dropped) == 11 && (!with_trap || t.traps > 0)) {
            h = t;
            h.found++;
            printf("    %s", line);
        }
    }
    if (f) fclose(f);
    return h;
}

static int capture(const char *path)
{
    fflush(stderr);
    return freopen(path, "w", stderr) != NULL;
}

static void wr32(uint32_t addr, uint32_t v) { memcpy(g_apu_ram_ptr + addr, &v, 4); }

static void dry_case(void)
{
    enum { VPV = 0x10000 };
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    const char *path = "apu_health_dry.txt";
    Health h;
    int n;

    g_apu_ram_ptr = (uint8_t *)calloc(1, 1u << 20);
    if (!d || !g_apu_ram_ptr) { failures++; return; }
    d->regs[NV_PAPU_VPVADDR] = VPV;
    /* The notifiers a dry voice writes each frame: off page 0, which reads
     * as an allocated window page here (mcpx_apu_phys). */
    d->regs[NV_PAPU_FENADDR] = 0x40000;
    d->regs[NV_PAPU_TVL2D] = 0;                  /* voice 0 heads the 2D list */
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    /* A persistent stream voice; d->vp.ssl is zeroed: both lists empty. */
    wr32(VPV + NV_PAVS_VOICE_CFG_FMT,
         NV_PAVS_VOICE_CFG_FMT_DATA_TYPE | NV_PAVS_VOICE_CFG_FMT_PERSIST
         | (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) | (1u << 30));
    wr32(VPV + NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    wr32(VPV + NV_PAVS_VOICE_TAR_PITCH_LINK, 0xFFFF);

    printf("dry stream voice\n");
    if (!capture(path)) { failures++; return; }
    for (n = 0; n < 150; n++) {
        memset(mixbins, 0, sizeof mixbins);
        mcpx_apu_vp_frame(d, mixbins);
    }
    apu_health_flush();
    apu_health_flush();                          /* nothing new: no line */
    fflush(stderr);
    h = read_health(path, 0);
    check(h.found == 1, "one health line (the second flush has nothing to say)");
    check(h.dry_ms > 99.9 && h.dry_ms < 100.1, "dry_ms=100.0 (150 frames of 32 samples)");
    check(h.dry_spells == 1, "one dry spell");
    check(h.dry_max_ms > 99.9 && h.dry_max_ms < 100.1, "longest spell 100.0 ms");
    check(h.underruns == 0 && h.dropped == 0, "no underrun, nothing dropped");
    {
        unsigned st;
        memcpy(&st, g_apu_ram_ptr + VPV + NV_PAVS_VOICE_PAR_STATE, 4);
        check((st & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE) != 0, "the persistent voice stays on");
    }
    free(g_apu_ram_ptr);
    g_apu_ram_ptr = NULL;
    free(d);
}

#if !defined(_WIN32)
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

static void trap_case(void)
{
    const char *path = "apu_health_trap.txt";
    uint8_t *ram = (uint8_t *)calloc(1, 256u << 10);
    MCPXAPUState *d;
    enum { VPV = 0x10000 };
    unsigned u0 = 0, u1 = 0;
    int attempt, n;
    Health h;
    char what[128];

    printf("trap held 30 ms, SDL dummy driver\n");
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("RECOMP_AUDIO_BUF_SAMPLES", "256", 1);
    setenv("RECOMP_AUDIO_BUF_COUNT", "4", 1);
    if (!ram || !capture(path)) { failures++; return; }
    d = mcpx_apu_init_standalone(ram);
    if (!d || !xa2_is_active()) {
        fflush(stderr);
        printf("  no SDL audio device (dummy driver): skipped\n");
        if (d) mcpx_apu_shutdown(d);
        free(ram);
        exit(failures ? 1 : 77);
    }
    mcpx_apu_lock_guest(d);
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;              /* no voices */
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    /* Voice 0, inactive and alone: what traps the front end below. */
    d->regs[NV_PAPU_VPVADDR] = VPV;
    d->regs[NV_PAPU_FETFORCE1] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    {
        uint32_t link = 0xFFFF;
        memcpy(ram + VPV + NV_PAVS_VOICE_TAR_PITCH_LINK, &link, 4);
    }
    mcpx_apu_unlock_guest(d);
    mcpx_apu_mmio_write(d, NV_PAPU_SECTL, 0x0F, 4);   /* the sequencer runs */

    sleep_ms(1000);                               /* pacing settles at the marks */
    /* Up to three tries: a loaded host (ctest -j) can oversleep the frame
     * thread past the queue now and then, an underrun of its own that is not
     * the trap's. A count that misses the trap's underrun (no poll before
     * the submit: 0) or counts it twice fails every try. */
    for (attempt = 1; attempt <= 3; attempt++) {
        mcpx_apu_lock_guest(d);
        /* Whatever came before is printed and the window starts over, so
         * the next line covers the trap only. */
        apu_health_flush();
        u0 = xa2_underruns();
        mcpx_apu_unlock_guest(d);

        /* An idle voice on a list traps the front end from inside the VP,
         * in the first frame of a period: the thread then waits trapped
         * partway through the period, where pace_step does not poll, as
         * when DirectSound's voices go idle in a game. Off the list again
         * once trapped, so the next frames do not trap anew. */
        mcpx_apu_lock_guest(d);
        d->regs[NV_PAPU_TVL2D] = 0;
        mcpx_apu_unlock_guest(d);
        for (n = 0; n < 1000 && !(qatomic_read(&d->regs[NV_PAPU_FECTL])
                                  & NV_PAPU_FECTL_FEMETHMODE_TRAPPED); n++)
            sleep_ms(1);
        mcpx_apu_lock_guest(d);
        d->regs[NV_PAPU_TVL2D] = 0xFFFF;
        mcpx_apu_unlock_guest(d);
        sleep_ms(30);
        mcpx_apu_mmio_write(d, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING, 4);
        sleep_ms(50);                             /* the recovery: a few periods */

        mcpx_apu_lock_guest(d);
        apu_health_flush();                       /* the trap's window */
        u1 = xa2_underruns();
        mcpx_apu_unlock_guest(d);
        printf("  try %d: %u underrun(s)\n", attempt, u1 - u0);
        if (u1 - u0 == 1)
            break;
        sleep_ms(200);
    }
    mcpx_apu_shutdown(d);
    fflush(stderr);

    h = read_health(path, 1);
    snprintf(what, sizeof what, "the device counted one underrun (%u)", u1 - u0);
    check(u1 - u0 == 1, what);
    check(h.found >= 1, "a health line for the trap's window");
    snprintf(what, sizeof what, "the line says underruns=1 (%u)", h.underruns);
    check(h.underruns == 1, what);
    snprintf(what, sizeof what, "a trap spell of about 30 ms (%u, max %.1f ms)", h.traps, h.trap_max_ms);
    check(h.traps >= 1 && h.trap_max_ms >= 20.0 && h.trap_max_ms < 200.0, what);
    snprintf(what, sizeof what, "a submit gap as long (%.1f ms)", h.gap_max_ms);
    check(h.gap_max_ms >= 20.0, what);
    snprintf(what, sizeof what, "queue seen empty before a submit (q_min=%d)", h.q_min);
    check(h.q_min == 0, what);
    check(h.dropped == 0 && h.dry_ms == 0.0, "nothing dropped, no dry time");
    free(ram);
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    dry_case();
#if !defined(_WIN32)
    trap_case();
#else
    printf("trap case: needs SDL's dummy driver, POSIX only: skipped\n");
#endif
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
