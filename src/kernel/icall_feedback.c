/*
 * Indirect-branch target feedback.
 *
 * Records which guest addresses the title actually reaches through indirect
 * branches, so the next codegen run can seed function detection with them
 * instead of guessing. This is the local equivalent of the two inputs
 * Microsoft's own recompiler takes -- VirtualDispatchTraceFiles (recorded
 * indirect-branch target sets) and UpdateEnlightenments (a persisted analysis
 * database the compiler rewrites every build). See
 * docs/technical/ms-fusion-codegen-teardown.md.
 *
 * Static analysis cannot see where a vtable call goes. Running the title can.
 * tools/recomp/analyze_unresolved.py currently classifies unresolved targets by
 * where they land relative to known functions, which is inference; this is
 * measurement.
 *
 * Opt-in: define RECOMP_ICALL_FEEDBACK. Without it the part below compiles to
 * nothing and the RECOMP_ICALL_OBSERVE hook in recomp_types.h expands to
 * (void)0, so release builds pay neither the store nor the 8 MiB. Every build
 * records the targets the analysis found (the hits above, XBA-304), from the
 * generated dispatch's own window, at no cost on a hit.
 *
 * Cost when enabled: one byte OR per indirect branch, into a flat array indexed
 * by guest VA. A byte array rather than a bitmap because a byte store needs no
 * read-modify-write, so concurrent recompiled threads cannot lose each other's
 * writes -- and 8 MiB is not worth a CAS loop. Dedup is free: the same target
 * hit a million times is still one byte.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_icall_feedback.h"

/* ── Hits in every build (XBA-304) ───────────────────────────────────────
 * The generated dispatch's window (recomp_dispatch_init): a slot is filled
 * the first time the miss path resolves a translated target, so the filled
 * slots are the targets the run reached that the analysis found -- what
 * tools.compat recall divides by. Read here without a lock: a slot is one
 * aligned pointer, written once (or rewritten with the same function). */
static recomp_icall_func_t const *s_hits;
static uint32_t s_hits_base, s_hits_span;
static volatile int s_hits_dumping;

static void hits_at_exit(void)
{
    recomp_icall_hits_dump();
}

void recomp_icall_hits_register(recomp_icall_func_t const *table, uint32_t base, uint32_t span)
{
    static int registered;
    s_hits = table;
    s_hits_base = base;
    s_hits_span = table ? span : 0;
    if (table && !registered) {
        registered = 1;
        atexit(hits_at_exit);   /* a desktop run's clean exit; the other ends dump themselves */
    }
}

void recomp_icall_hits_reset(void)
{
    s_hits = NULL;
    s_hits_base = s_hits_span = 0;
}

/* The dump's path: $RECOMP_ICALL_DUMP, else icall_targets.dump in the
 * discovery file's folder; 0 when there is none. */
static int hits_path(char *out, size_t size)
{
    const char *e = getenv("RECOMP_ICALL_DUMP");
    const char *disc, *slash;
    size_t dir;
    if (e && *e) {
        if (!strcmp(e, "0"))
            return 0;
        return snprintf(out, size, "%s", e) < (int)size;
    }
    disc = getenv("RECOMP_DISCOVERY");   /* the discovery file (recomp_discovery.h): same folder */
    if (!disc || !*disc || !strcmp(disc, "0"))
        return 0;
    slash = strrchr(disc, '/');
#ifdef _WIN32
    {
        const char *bs = strrchr(disc, '\\');
        if (bs && (!slash || bs > slash))
            slash = bs;
    }
#endif
    dir = slash ? (size_t)(slash - disc) + 1 : 0;
    return snprintf(out, size, "%.*sicall_targets.dump", (int)dir, disc) < (int)size;
}

static int hit_at(uint32_t va)
{
    uint32_t off = va - s_hits_base;
    return off < s_hits_span && s_hits[off] != NULL;
}

int recomp_icall_hits_dump(void)
{
    char path[1024], tmp[1100], line[64];
    FILE *old, *f;
    unsigned long now = 0, kept = 0;
    uint32_t off;
    int rc = -1;

    if (!s_hits || !hits_path(path, sizeof path))
        return -1;
#if defined(__GNUC__) || defined(__clang__)
    if (__sync_lock_test_and_set(&s_hits_dumping, 1))
        return -1;              /* another thread is writing it (the window's end and atexit) */
#endif
    for (off = 0; off < s_hits_span; off++)
        now += s_hits[off] != NULL;
    old = fopen(path, "r");
    if (!now && !old)
        goto out;               /* nothing reached (a pack from before XBA-304): no file, recall stays unknown */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        if (old)
            fclose(old);
        goto out;
    }
    fprintf(f, "# icall-feedback v1\n");
    fprintf(f, "# va flags   (1=resolved): indirect-call targets reached that the analysis found (XBA-304)\n");
    fprintf(f, "# window 0x%08X-0x%08X\n", (unsigned)s_hits_base, (unsigned)(s_hits_base + s_hits_span));
    if (old) {
        /* Earlier runs' targets this one did not reach. */
        while (fgets(line, sizeof line, old)) {
            unsigned va, flags;
            if (line[0] == '#' || sscanf(line, "%x %u", &va, &flags) != 2 || !flags)
                continue;
            if (hit_at((uint32_t)va))
                continue;
            fprintf(f, "%08X %u\n", va, flags);
            kept++;
        }
        fclose(old);
    }
    for (off = 0; off < s_hits_span; off++)
        if (s_hits[off])
            fprintf(f, "%08X 1\n", (unsigned)(s_hits_base + off));
    if (fclose(f) != 0) {
        remove(tmp);
        goto out;
    }
#ifdef _WIN32
    remove(path);               /* rename does not replace on Windows */
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        goto out;
    }
    fprintf(stderr, "[icall] %s: %lu target(s) reached this run, %lu from earlier runs\n", path, now, kept);
    rc = (int)(now + kept);
out:
#if defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(&s_hits_dumping);
#endif
    return rc;
}

#ifdef RECOMP_ICALL_FEEDBACK

/* One byte per guest VA in the image window. Zero-initialised in BSS; the OS
 * only commits the pages actually touched, so a title that reaches 4k distinct
 * targets does not resident 8 MiB. */
volatile unsigned char g_icall_seen[RECOMP_ICALL_FB_SIZE];

/* Guarded indirect-call arms taken and missed (RECOMP_ICALL_GUARD_HIT/MISS in
 * recomp_types.h), reported beside the per-site dump. */
volatile uint64_t g_icall_guard_hits = 0;
volatile uint64_t g_icall_guard_misses = 0;

/* Per-site targets. Open addressing over 32K slots (a large title has a few
 * thousand indirect call sites); each slot holds up to SITE_MAX distinct
 * targets and a saturation mark beyond that, which the lifter reads as "do
 * not guard this one". */
uint32_t g_icall_site_hs[65536];
uint32_t g_icall_site_hl[65536];
#define SITE_SLOTS 32768u
#define SITE_MAX 6
typedef struct {
    uint32_t site;
    uint8_t n;
    uint8_t saturated;
    uint32_t t[SITE_MAX];
} icall_site_rec;
static icall_site_rec s_sites[SITE_SLOTS];
static unsigned long s_site_records, s_site_saturated;

void recomp_icall_observe_site(uint32_t site, uint32_t va)
{
    if (!site)
        return;
    uint32_t h = (site * 2654435761u) >> 17;
    for (unsigned probe = 0; probe < SITE_SLOTS; probe++) {
        icall_site_rec *r = &s_sites[(h + probe) & (SITE_SLOTS - 1u)];
        if (r->site == 0) {
            r->site = site; r->t[0] = va; r->n = 1;
            s_site_records++;
            return;
        }
        if (r->site != site)
            continue;
        for (unsigned i = 0; i < r->n; i++)
            if (r->t[i] == va)
                return;
        if (r->n < SITE_MAX) {
            r->t[r->n++] = va;
        } else if (!r->saturated) {
            r->saturated = 1;
            s_site_saturated++;
        }
        return;
    }
}

/* icall_sites.dump beside the targets dump: `site t1 t2 ... [+]`, where `+`
 * says the site reached more targets than the record holds. */
static void dump_sites(const char *targets_path)
{
    char path[1024];
    size_t n = strlen(targets_path);
    const char *suffix = "targets.dump";
    size_t sl = strlen(suffix);
    if (n >= sl && strcmp(targets_path + n - sl, suffix) == 0
            && n - sl + 12 < sizeof path) {
        memcpy(path, targets_path, n - sl);
        memcpy(path + n - sl, "sites.dump", 11);
    } else if (n + 7 < sizeof path) {
        memcpy(path, targets_path, n);
        memcpy(path + n, ".sites", 7);
    } else {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# icall-sites v1\n");
    fprintf(f, "# site target...   (+ = more targets than recorded)\n");
    for (unsigned i = 0; i < SITE_SLOTS; i++) {
        const icall_site_rec *r = &s_sites[i];
        if (!r->site)
            continue;
        fprintf(f, "%08X", (unsigned)r->site);
        for (unsigned k = 0; k < r->n; k++)
            fprintf(f, " %08X", (unsigned)r->t[k]);
        if (r->saturated)
            fprintf(f, " +");
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[icall-feedback] %s: %lu sites, %lu saturated;"
            " guarded arms hit=%llu missed=%llu\n",
            path, s_site_records, s_site_saturated,
            (unsigned long long)g_icall_guard_hits,
            (unsigned long long)g_icall_guard_misses);
}

void recomp_icall_feedback_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[icall-feedback] cannot write %s\n", path);
        return;
    }

    /* Plain text, one record per line: VA and the flags observed for it.
     * Deliberately not JSON -- this is written from a possibly-crashing
     * process, so a truncated file must still be parseable line by line.
     * tools/recomp/icall_feedback.py merges it. */
    unsigned long resolved = 0, unresolved = 0;
    fprintf(f, "# icall-feedback v1\n");
    fprintf(f, "# va flags   (1=resolved, 2=unresolved, 3=both)\n");
    for (uint32_t off = 0; off < RECOMP_ICALL_FB_SIZE; off++) {
        unsigned char v = g_icall_seen[off];
        if (!v)
            continue;
        fprintf(f, "%08X %u\n", (unsigned)(RECOMP_ICALL_FB_BASE + off), v);
        if (v & RECOMP_ICALL_SEEN_RESOLVED)   resolved++;
        if (v & RECOMP_ICALL_SEEN_UNRESOLVED) unresolved++;
    }
    fclose(f);

    fprintf(stderr, "[icall-feedback] %s: %lu resolved, %lu unresolved targets\n",
            path, resolved, unresolved);
    dump_sites(path);
}

static void dump_at_exit(void)
{
    recomp_icall_feedback_dump(RECOMP_ICALL_FEEDBACK_PATH);
}

void recomp_icall_feedback_init(void)
{
    /* atexit only fires on a clean exit. A title killed by a watchdog timeout
     * (run.sh uses `timeout`, which is exit 124) never reaches it, which is why
     * the crash handler dumps too -- and why, if a title neither exits nor
     * crashes, you must call the dump yourself from wherever you decide the run
     * is over. There is deliberately no timer thread doing it behind your back. */
    atexit(dump_at_exit);
}

#endif  /* RECOMP_ICALL_FEEDBACK */
