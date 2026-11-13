/* The discovery file (XBA-64): recomp_discovery.h has the format and the
 * rules. No dependency on the rest of the runtime, so a test builds it alone. */
#include "recomp_discovery.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define d_open(p)      _open((p), _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY, 0644)
#define d_write        _write
#define d_close        _close
#define d_mkdir(p)     _mkdir(p)
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define d_open(p)      open((p), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)
#define d_write        write
#define d_close        close
#define d_mkdir(p)     mkdir((p), 0755)
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

enum { K_ICALL = 1, K_THREAD, K_THREAD_CTX, K_UNIMPL, K_D3D, K_WAIT_SPIN, K_LAUNCH, K_STACK_SWITCH, K_STUB,
       K_SMC_EXEC, K_SMC_WRITE, K_EXCEPTION, K_D3D_LAYOUT, K_WILD, K_LAST = K_WILD };
/* The two smc kinds share "t":"smc" and differ by "kind" (XBA-118). */
static const char *const kind_name[] = { "", "icall", "thread", "thread_ctx", "unimpl", "d3d", "wait_spin", "launch",
                                         "stack_switch", "stub", "smc", "smc", "exception", "d3d_layout", "wild" };
/* A seed entry written after the process went wrong is keyed apart (profile-free WP1):
 * an untainted entry for the same address, from a later healthy run, is
 * still new and gets written, which is what releases it as a seed. */
#define K_TAINTED 0x80

static int is_seed_kind(int k) { return k == K_ICALL || k == K_THREAD || k == K_THREAD_CTX; }

/* What is already in the file: kind << 32 | va, open addressing. A title that
 * fills it is beyond what one file can help with; the rest is dropped. */
#define SEEN_SLOTS 8192u
static uint64_t s_seen[SEEN_SLOTS];
static unsigned s_seen_n;

static int s_state;             /* 0 not opened yet, 1 open, -1 off */
static int s_fd = -1;
static char s_path[1024];
static unsigned s_written;
static unsigned s_seeds;        /* untainted icall / thread / thread_ctx entries written by this process */
static int s_run_written;
static int s_tainted;           /* the process went wrong: a wild call, an smc or an exception (profile-free WP1) */

static int seen_add(uint64_t key)       /* 1 when new */
{
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (SEEN_SLOTS - 1);
    for (unsigned i = 0; i < SEEN_SLOTS; i++, h = (h + 1) & (SEEN_SLOTS - 1)) {
        if (s_seen[h] == key)
            return 0;
        if (s_seen[h] == 0) {
            if (s_seen_n >= SEEN_SLOTS * 3 / 4)
                return -1;
            s_seen[h] = key;
            s_seen_n++;
            return 1;
        }
    }
    return -1;
}

static int seen_has(uint64_t key)
{
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (SEEN_SLOTS - 1);
    for (unsigned i = 0; i < SEEN_SLOTS; i++, h = (h + 1) & (SEEN_SLOTS - 1)) {
        if (s_seen[h] == key)
            return 1;
        if (s_seen[h] == 0)
            return 0;
    }
    return 0;
}

static int kind_of(const char *s, size_t n)
{
    for (int k = K_ICALL; k <= K_LAST; k++)
        if (strlen(kind_name[k]) == n && !memcmp(s, kind_name[k], n))
            return k;
    return 0;
}

/* The entries an earlier run left: "t" and "va" of each line. */
static void load_existing(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[512];
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        const char *t = strstr(line, "\"t\":\""), *va = strstr(line, "\"va\":\"0x");
        const char *ord = strstr(line, "\"ord\":");
        const char *e;
        uint32_t key;
        int k;
        if (!t || (!va && !ord))
            continue;
        t += 5;
        if (!(e = strchr(t, '"')) || !(k = kind_of(t, (size_t)(e - t))))
            continue;
        if (k == K_SMC_EXEC && strstr(line, "\"kind\":\"code_write\""))
            k = K_SMC_WRITE;
        /* A stub entry is keyed by its ordinal, the others by "va". */
        if (k == K_STUB) {
            if (!ord)
                continue;
            key = (uint32_t)strtoul(ord + 6, NULL, 10);
        } else {
            if (!va)
                continue;
            key = (uint32_t)strtoul(va + 6, NULL, 16);
        }
        if (is_seed_kind(k) && strstr(line, "\"tainted\":true"))
            k |= K_TAINTED;
        (void)seen_add((uint64_t)k << 32 | key);
    }
    fclose(f);
}

static void make_parent(const char *path)
{
    char d[sizeof s_path];
    char *slash;
    snprintf(d, sizeof d, "%s", path);
    slash = strrchr(d, '/');
#ifdef _WIN32
    {
        char *b = strrchr(d, '\\');
        if (b && (!slash || b > slash))
            slash = b;
    }
#endif
    if (slash && slash != d) {
        *slash = 0;
        (void)d_mkdir(d);
    }
}

static int open_locked(void)
{
    const char *p;
    if (s_state)
        return s_state > 0;
    p = getenv("RECOMP_DISCOVERY");
    if (!p || !*p || !strcmp(p, "0") || strlen(p) >= sizeof s_path) {
        s_state = -1;
        return 0;
    }
    snprintf(s_path, sizeof s_path, "%s", p);
    load_existing(s_path);
    make_parent(s_path);
    s_fd = d_open(s_path);
    if (s_fd < 0) {
        fprintf(stderr, "[DISCOVERY] cannot open %s (%s): nothing recorded\n", s_path, strerror(errno));
        s_state = -1;
        return 0;
    }
    fprintf(stderr, "[DISCOVERY] recording to %s (%u entries already there)\n", s_path, s_seen_n);
    s_state = 1;
    return 1;
}

static void put_line(const char *line)
{
    size_t n = strlen(line), off = 0;
    while (off < n) {
        int w = (int)d_write(s_fd, line + off, (unsigned)(n - off));
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return;
        }
        off += (size_t)w;
    }
}

/* One entry: `fields` is the rest of the object after "va". */
static void note(int kind, uint32_t va, const char *fields)
{
    char line[512];
    int added, tainted;

    LOCK();
    if (!open_locked()) {
        UNLOCK();
        return;
    }
    /* The first sign the process went wrong (profile-free WP1): an indirect call to
     * an address that is not code, code run or written outside what was
     * lifted, an exception nothing handled. From then on a call can go
     * through garbage (Fable: 34 targets from one call site, 14 of them in
     * executable bytes), so seed entries carry "tainted":true and the next
     * Prepare takes them only when a healthy run reports them too. Set even
     * when the entry itself is already in the file: the sign is this run's. */
    if ((kind == K_WILD || kind == K_SMC_EXEC || kind == K_SMC_WRITE || kind == K_EXCEPTION) && !s_tainted) {
        s_tainted = 1;
        fprintf(stderr, "[DISCOVERY] the run went wrong here (%s 0x%08X): code addresses it reports from now on"
                " are marked tainted, the next Prepare does not seed them on their own\n", kind_name[kind], va);
    }
    tainted = s_tainted && is_seed_kind(kind);
    /* A tainted entry adds nothing when the address is already in the file
     * untainted; an untainted one is new even when a tainted one is there. */
    if (tainted && seen_has((uint64_t)kind << 32 | va)) {
        UNLOCK();
        return;
    }
    added = seen_add((uint64_t)(kind | (tainted ? K_TAINTED : 0)) << 32 | va);
    if (added == 0) {
        UNLOCK();
        return;
    }
    if (added < 0) {
        static int full_said;
        if (!full_said++)
            fprintf(stderr, "[DISCOVERY] %u entries: the rest is not recorded\n", s_seen_n);
        UNLOCK();
        return;
    }
    if (!s_run_written) {
        time_t now = time(NULL);
        struct tm tm;
        char ts[32] = "";
#ifdef _WIN32
        if (gmtime_s(&tm, &now) == 0)
#else
        if (gmtime_r(&now, &tm))
#endif
            strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        /* "v":2: this writer marks tainted entries and writes "wild" ones
         * (profile-free WP1); a reader derives the taint itself for an older run. */
        snprintf(line, sizeof line, "{\"t\":\"run\",\"time\":\"%s\",\"v\":2}\n", ts);
        put_line(line);
        s_run_written = 1;
    }
    if (kind == K_STUB)     /* keyed by ordinal: no address of its own */
        snprintf(line, sizeof line, "{\"t\":\"stub\",\"ord\":%u%s}\n", va, fields);
    else
        snprintf(line, sizeof line, "{\"t\":\"%s\",\"va\":\"0x%08X\"%s%s}\n", kind_name[kind], va, fields,
                 tainted ? ",\"tainted\":true" : "");
    put_line(line);
    /* The first code the analysis missed in this run (XBA-129): an indirect
     * call to it was skipped (the title goes on, maybe wrong) or a thread did
     * not start. Said once, so a log shows it without a crash; the app reads
     * the entry from the file and the status line's "seeds". A tainted entry
     * is not counted: the next Prepare will not seed it on its own. */
    if (is_seed_kind(kind) && !tainted && !s_seeds++)
        fprintf(stderr, "[DISCOVERY] first code the analysis missed in this run: %s 0x%08X"
                " (skipped; the next Prepare adds it)\n", kind_name[kind], va);
    /* No fsync: a crash of the process keeps what write() handed the kernel
     * (it is in the page cache); only a crash of the whole system would not,
     * and an fsync per entry, under the lock, on a game thread, cost
     * milliseconds on a phone's flash (review of XBA-64). */
    s_written++;
    UNLOCK();
}

void recomp_discovery_icall(uint32_t va, uint32_t ret_va)
{
    char f[48];
    snprintf(f, sizeof f, ",\"ret\":\"0x%08X\"", ret_va);
    note(K_ICALL, va, f);
}

void recomp_discovery_thread(uint32_t routine, uint32_t ctx1)
{
    char f[48];
    snprintf(f, sizeof f, ",\"ctx1\":\"0x%08X\"", ctx1);
    note(K_THREAD, routine, f);
}

void recomp_discovery_thread_ctx(uint32_t ctx1, uint32_t routine)
{
    char f[48];
    snprintf(f, sizeof f, ",\"routine\":\"0x%08X\"", routine);
    note(K_THREAD_CTX, ctx1, f);
}

void recomp_discovery_unimpl(uint32_t va, const char *insn)
{
    /* ,"insn":"..." with the text as printable ASCII, quotes and backslashes escaped. */
    char f[200];
    size_t o = 0;
    o += (size_t)snprintf(f, sizeof f, ",\"insn\":\"");
    for (const char *s = insn ? insn : ""; *s && o < sizeof f - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            f[o++] = '\\';
        f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    f[o++] = '"';
    f[o] = 0;
    note(K_UNIMPL, va, f);
}

void recomp_discovery_d3d(uint32_t device, uint32_t slot)
{
    char f[48];
    snprintf(f, sizeof f, ",\"slot\":\"0x%08X\"", slot);
    note(K_D3D, device, f);
}

void recomp_discovery_wait_spin(uint32_t object, uint32_t ret_va, uint32_t header, uint32_t status)
{
    char f[120];
    snprintf(f, sizeof f, ",\"ret\":\"0x%08X\",\"header\":\"0x%08X\",\"status\":\"0x%08X\"",
             ret_va, header, status);
    note(K_WAIT_SPIN, object, f);
}

void recomp_discovery_launch(const char *path)
{
    /* ,"path":"..." as printable ASCII, quotes and backslashes escaped; the
     * key is a hash of the lowercased path. */
    char f[300];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"path\":\"");
    uint32_t h = 2166136261u;
    for (const char *s = path ? path : ""; *s; s++) {
        unsigned char c = (unsigned char)*s;
        h = (h ^ (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
        if (o < sizeof f - 4) {
            if (c == '"' || c == '\\')
                f[o++] = '\\';
            f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    f[o++] = '"';
    f[o] = 0;
    note(K_LAUNCH, h, f);
}

void recomp_discovery_stack_switch(uint32_t site, uint32_t to, const char *why)
{
    /* ,"to":"0x...","why":"..." with the reason as printable ASCII (it is
     * ours, a constant, but quoted the same way as the others). */
    char f[200];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"to\":\"0x%08X\",\"why\":\"", to);
    for (const char *s = why ? why : ""; *s && o < sizeof f - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            f[o++] = '\\';
        f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    f[o++] = '"';
    f[o] = 0;
    note(K_STACK_SWITCH, site, f);
}

void recomp_discovery_stub(uint32_t ordinal, const char *name, uint32_t ret_va)
{
    /* ,"name":"...","ret":"0x..." -- the name is an identifier, but it is
     * escaped like any other text. */
    char f[160];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"name\":\"");
    for (const char *s = name ? name : ""; *s && o < sizeof f - 40; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            f[o++] = '\\';
        f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    snprintf(f + o, sizeof f - o, "\",\"ret\":\"0x%08X\"", ret_va);
    note(K_STUB, ordinal, f);
}

/* ,"key":"..." with printable ASCII, quotes and backslashes escaped, into f at o. */
static size_t put_str(char *f, size_t o, size_t cap, const char *key, const char *v)
{
    o += (size_t)snprintf(f + o, cap - o, ",\"%s\":\"", key);
    for (const char *s = v ? v : ""; *s && o < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            f[o++] = '\\';
        f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    f[o++] = '"';
    f[o] = 0;
    return o;
}

void recomp_discovery_smc_exec(uint32_t target, uint32_t ret_va, const char *where, const char *bytes)
{
    char f[200];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"kind\":\"exec_outside_image\",\"ret\":\"0x%08X\"", ret_va);
    o = put_str(f, o, sizeof f, "where", where);
    if (bytes && *bytes)
        (void)put_str(f, o, sizeof f, "bytes", bytes);
    note(K_SMC_EXEC, target, f);
}

void recomp_discovery_smc_write(uint32_t page, uint32_t at, const char *section, const char *by, uint32_t ret_va)
{
    char f[240];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"kind\":\"code_write\",\"at\":\"0x%08X\"", at);
    o = put_str(f, o, sizeof f, "section", section);
    o = put_str(f, o, sizeof f, "by", by);
    snprintf(f + o, sizeof f - o, ",\"ret\":\"0x%08X\"", ret_va);
    note(K_SMC_WRITE, page, f);
}

void recomp_discovery_exception(uint32_t va, uint32_t code, const char *why)
{
    /* ,"code":"0x...","why":"..." -- the reason is ours, quoted like the others (XBA-129). */
    char f[200];
    size_t o = (size_t)snprintf(f, sizeof f, ",\"code\":\"0x%08X\"", code);
    (void)put_str(f, o, sizeof f, "why", why);
    note(K_EXCEPTION, va, f);
}

void recomp_discovery_d3d_layout(const char *lib, uint32_t build, const char *rule, uint32_t used)
{
    /* ,"lib":"...","build":N,"rule":"...","used":"0x..." -- lib and rule are
     * ours or the XBE's library name (8 characters): escaped all the same.
     * Keyed by a hash of lib, build and rule: one line per rule and build. */
    char f[200];
    uint32_t h = 2166136261u;
    size_t o = (size_t)snprintf(f, sizeof f, ",\"lib\":\"");
    for (int part = 0; part < 2; part++) {
        const char *s = part ? (rule ? rule : "") : (lib ? lib : "");
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            h = (h ^ c) * 16777619u;
            if (o < sizeof f - 60) {
                if (c == '"' || c == '\\')
                    f[o++] = '\\';
                f[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
        }
        h = (h ^ 0xFFu) * 16777619u;
        if (!part)
            o += (size_t)snprintf(f + o, sizeof f - o, "\",\"build\":%u,\"rule\":\"", build);
    }
    snprintf(f + o, sizeof f - o, "\",\"used\":\"0x%08X\"", used);
    note(K_D3D_LAYOUT, h ^ (build * 2654435761u), f);
}

void recomp_discovery_wild(uint32_t va, uint32_t ret_va)
{
    char f[48];
    snprintf(f, sizeof f, ",\"ret\":\"0x%08X\"", ret_va);
    note(K_WILD, va, f);
}

int recomp_discovery_tainted(void)
{
    return s_tainted;
}

const char *recomp_discovery_path(void)
{
    int on;
    LOCK();
    on = open_locked();
    UNLOCK();
    return on ? s_path : NULL;
}

unsigned recomp_discovery_written(void)
{
    return s_written;
}

unsigned recomp_discovery_seeds(void)
{
    return s_seeds;
}

void recomp_discovery_reset(void)
{
    LOCK();
    if (s_fd >= 0)
        d_close(s_fd);
    s_fd = -1;
    s_state = 0;
    s_written = 0;
    s_seeds = 0;
    s_run_written = 0;
    s_tainted = 0;
    s_seen_n = 0;
    memset(s_seen, 0, sizeof s_seen);
    UNLOCK();
}
