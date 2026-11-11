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
#define d_sync         _commit
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
#define d_sync         fsync
#define d_close        close
#define d_mkdir(p)     mkdir((p), 0755)
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

enum { K_ICALL = 1, K_THREAD, K_THREAD_CTX, K_UNIMPL };
static const char *const kind_name[] = { "", "icall", "thread", "thread_ctx", "unimpl" };

/* What is already in the file: kind << 32 | va, open addressing. A title that
 * fills it is beyond what one file can help with; the rest is dropped. */
#define SEEN_SLOTS 8192u
static uint64_t s_seen[SEEN_SLOTS];
static unsigned s_seen_n;

static int s_state;             /* 0 not opened yet, 1 open, -1 off */
static int s_fd = -1;
static char s_path[1024];
static unsigned s_written;
static int s_run_written;

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

static int kind_of(const char *s, size_t n)
{
    for (int k = K_ICALL; k <= K_UNIMPL; k++)
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
        const char *e;
        int k;
        if (!t || !va)
            continue;
        t += 5;
        if (!(e = strchr(t, '"')) || !(k = kind_of(t, (size_t)(e - t))))
            continue;
        (void)seen_add((uint64_t)k << 32 | (uint32_t)strtoul(va + 6, NULL, 16));
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
    int added;

    LOCK();
    if (!open_locked()) {
        UNLOCK();
        return;
    }
    added = seen_add((uint64_t)kind << 32 | va);
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
        snprintf(line, sizeof line, "{\"t\":\"run\",\"time\":\"%s\"}\n", ts);
        put_line(line);
        s_run_written = 1;
    }
    snprintf(line, sizeof line, "{\"t\":\"%s\",\"va\":\"0x%08X\"%s}\n", kind_name[kind], va, fields);
    put_line(line);
    (void)d_sync(s_fd);
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

void recomp_discovery_reset(void)
{
    LOCK();
    if (s_fd >= 0)
        d_close(s_fd);
    s_fd = -1;
    s_state = 0;
    s_written = 0;
    s_run_written = 0;
    s_seen_n = 0;
    memset(s_seen, 0, sizeof s_seen);
    UNLOCK();
}
