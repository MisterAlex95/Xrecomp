/* gc_rel.c -- REL modules the game loaded, and the discovered file the next Prepare reads (XBA-264).
 *
 * The SDK's OSLink keeps every loaded module on a list in low memory (head at 0x800030C8): each entry is the module's
 * header, i.e. the start of the buffer the REL file was read into; once linked its section table holds absolute
 * addresses (bit 0: executable) and the bss section the address of its separate allocation. After every field the
 * list is walked (a few loads per module); when it changed, every span check is reset (code may have been replaced
 * without an icbi we saw) and, once it stayed the same for a field (OSLink done), each module is recorded:
 *     REL <id> <base> <bss> <FNV-1a of its executable sections> [<id>@<base> of the other modules loaded]
 * Prepare relocates the module from the disc at that base and lifts it when its bytes hash the same (gc_tools.rel).
 * Interpreter misses inside a loaded module's code are recorded like misses in the DOL's text.
 *
 * The discovered file (gc_recomp_write_discovered): addresses one per line (hex) and REL records, merged with what the
 * file already holds (earlier runs), written through a temporary file and a rename. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gc_internal.h"

#define GC_REL_LIST 0x000030C8u
#define GC_REL_MAX 64
#define GC_REL_EXEC_MAX 8
#define GC_REL_RECORDS_MAX 4096
#define GC_DISCOVERED_MAX 65536

typedef struct {
    uint32_t id, base, bss;
    int nexec;
    uint32_t lo[GC_REL_EXEC_MAX], hi[GC_REL_EXEC_MAX];
} rel_mod;

struct gc_rel_state {
    rel_mod cur[GC_REL_MAX];
    int ncur;
    uint32_t sig;
    int settle; /* fields the list stayed the same since it changed; recorded at 1 */
    char **rec;
    size_t nrec, cap;
};

static bool in_mem1(uint32_t a, uint32_t n)
{
    uint32_t p = a & 0x3FFFFFFFu;
    return (a >> 30) == 2 && p < GC_RAM_SIZE && n <= GC_RAM_SIZE - p;
}

static uint32_t r32(gc_machine *m, uint32_t a)
{
    const uint8_t *p = m->h.ram + (a & 0x3FFFFFFFu);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The list now, n modules (0 when there is none or it looks wrong). */
static int walk(gc_machine *m, rel_mod *out)
{
    int n = 0;
    uint32_t p = r32(m, 0x80000000u | GC_REL_LIST);
    while (p && n < GC_REL_MAX) {
        if ((p & 3) || !in_mem1(p, 0x40))
            return n;
        rel_mod *r = &out[n];
        memset(r, 0, sizeof *r);
        r->id = r32(m, p);
        r->base = p;
        uint32_t nsec = r32(m, p + 0x0C), si = r32(m, p + 0x10);
        unsigned bss_sec = m->h.ram[(p + 0x33) & 0x3FFFFFFFu];
        if (nsec == 0 || nsec > 64 || (si & 3) || !in_mem1(si, nsec * 8))
            return n;
        for (uint32_t k = 0; k < nsec; k++) {
            uint32_t off = r32(m, si + 8 * k), size = r32(m, si + 8 * k + 4);
            if (k == bss_sec && k)
                r->bss = off & ~1u;
            if ((off & 1) && size && r->nexec < GC_REL_EXEC_MAX && in_mem1(off & ~1u, size)) {
                r->lo[r->nexec] = off & ~1u;
                r->hi[r->nexec++] = (off & ~1u) + size;
            }
        }
        n++;
        uint32_t next = r32(m, p + 4);
        if (next == p)
            break;
        p = next;
    }
    return n;
}

static uint32_t fnv_range(gc_machine *m, uint32_t lo, uint32_t hi, uint32_t h)
{
    for (uint32_t a = lo; a < hi; a++)
        h = (h ^ m->h.ram[a & 0x3FFFFFFFu]) * 0x01000193u;
    return h;
}

static void add_record(struct gc_rel_state *s, const char *line)
{
    for (size_t i = 0; i < s->nrec; i++)
        if (!strcmp(s->rec[i], line))
            return;
    if (s->nrec >= GC_REL_RECORDS_MAX)
        return;
    if (s->nrec == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char **p = realloc(s->rec, cap * sizeof *p);
        if (!p)
            return;
        s->rec = p;
        s->cap = cap;
    }
    char *d = strdup(line);
    if (d)
        s->rec[s->nrec++] = d;
}

/* One record per module loaded now. */
static void snapshot(gc_machine *m, struct gc_rel_state *s)
{
    for (int i = 0; i < s->ncur; i++) {
        const rel_mod *r = &s->cur[i];
        uint32_t h = 0x811C9DC5u;
        for (int k = 0; k < r->nexec; k++)
            h = fnv_range(m, r->lo[k], r->hi[k], h);
        char line[64 + 20 * GC_REL_MAX];
        int n = snprintf(line, sizeof line, "REL %X %08X %08X %08X", r->id, r->base, r->bss, h);
        /* co-loaded modules, sorted by id then base (the order gc_tools.rel.format_record writes) */
        bool used[GC_REL_MAX] = {false};
        for (int c = 0; c < s->ncur - 1 && n < (int)sizeof line - 24; c++) {
            int best = -1;
            for (int j = 0; j < s->ncur; j++) {
                if (j == i || used[j])
                    continue;
                if (best < 0 || s->cur[j].id < s->cur[best].id ||
                    (s->cur[j].id == s->cur[best].id && s->cur[j].base < s->cur[best].base))
                    best = j;
            }
            if (best < 0)
                break;
            used[best] = true;
            n += snprintf(line + n, sizeof line - (size_t)n, " %X@%08X", s->cur[best].id, s->cur[best].base);
        }
        add_record(s, line);
    }
}

void gc_rel_field(gc_machine *m)
{
    struct gc_rel_state *s = m->rel;
    if (!s) {
        s = m->rel = calloc(1, sizeof *s);
        if (!s)
            return;
    }
    rel_mod now[GC_REL_MAX];
    int n = walk(m, now);
    uint32_t sig = 0x811C9DC5u ^ (uint32_t)n;
    for (int i = 0; i < n; i++)
        sig = ((sig ^ now[i].id) * 0x01000193u ^ now[i].base) * 0x01000193u;
    if (sig != s->sig || n != s->ncur) {
        s->sig = sig;
        s->ncur = n;
        memcpy(s->cur, now, (size_t)n * sizeof *now);
        s->settle = 0;
        if (m->rc && m->span_state) /* code may have been replaced: every span is checked again before use */
            memset(m->span_state, 0, m->rc->nspans ? m->rc->nspans : 1);
        return;
    }
    if (n && ++s->settle == 1)
        snapshot(m, s);
}

bool gc_rel_in_code(gc_machine *m, uint32_t pc)
{
    const struct gc_rel_state *s = m->rel;
    if (!s)
        return false;
    for (int i = 0; i < s->ncur; i++)
        for (int k = 0; k < s->cur[i].nexec; k++)
            if (pc >= s->cur[i].lo[k] && pc < s->cur[i].hi[k])
                return true;
    return false;
}

void gc_rel_free(gc_machine *m)
{
    struct gc_rel_state *s = m->rel;
    if (!s)
        return;
    for (size_t i = 0; i < s->nrec; i++)
        free(s->rec[i]);
    free(s->rec);
    free(s);
    m->rel = NULL;
}

size_t gc_recomp_rel_records(gc_machine *m, const char **out, size_t max)
{
    struct gc_rel_state *s = m->rel;
    if (!s)
        return 0;
    if (s->ncur)
        snapshot(m, s); /* the modules as they are now (later loads may have patched their imports) */
    for (size_t i = 0; i < s->nrec && i < max; i++)
        out[i] = s->rec[i];
    return s->nrec;
}

/* ---- The discovered file ---------------------------------------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void mkdirs_of(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

int gc_recomp_write_discovered(gc_machine *m, const char *path)
{
    if (!m || !path || !*path)
        return -1;
    size_t nmiss = gc_recomp_missed(m, NULL, 0);
    const char *recs[GC_REL_RECORDS_MAX];
    size_t nrec = gc_recomp_rel_records(m, recs, GC_REL_RECORDS_MAX);
    if (nrec > GC_REL_RECORDS_MAX)
        nrec = GC_REL_RECORDS_MAX;
    if (!nmiss && !nrec)
        return 0;
    /* what the file holds */
    size_t cap = nmiss + 1024, na = 0, nold_rec = 0, cap_rec = 64;
    uint32_t *addr = malloc(cap * sizeof *addr);
    char **old_rec = malloc(cap_rec * sizeof *old_rec);
    if (!addr || !old_rec) {
        free(addr);
        free(old_rec);
        return -1;
    }
    FILE *f = fopen(path, "r");
    char line[2048];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#');
            if (h)
                *h = 0;
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t'))
                line[--l] = 0;
            char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                continue;
            if (!strncmp(p, "REL ", 4)) {
                if (nold_rec == cap_rec) {
                    char **q = realloc(old_rec, cap_rec * 2 * sizeof *q);
                    if (!q)
                        continue;
                    old_rec = q;
                    cap_rec *= 2;
                }
                if (nold_rec < GC_REL_RECORDS_MAX && (old_rec[nold_rec] = strdup(p)))
                    nold_rec++;
                continue;
            }
            char *end;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p || v > 0xFFFFFFFFul || (v & 3) || na >= GC_DISCOVERED_MAX)
                continue;
            if (na == cap) {
                uint32_t *q = realloc(addr, cap * 2 * sizeof *q);
                if (!q)
                    continue;
                addr = q;
                cap *= 2;
            }
            addr[na++] = (uint32_t)v;
        }
        fclose(f);
    }
    size_t had = na, had_rec = nold_rec;
    if (na + nmiss > cap) {
        uint32_t *q = realloc(addr, (na + nmiss) * sizeof *q);
        if (!q)
            nmiss = 0;
        else
            addr = q;
    }
    if (nmiss)
        gc_recomp_missed(m, addr + na, nmiss);
    na += nmiss;
    qsort(addr, na, sizeof *addr, cmp_u32);
    size_t u = 0;
    for (size_t i = 0; i < na; i++)
        if (!u || addr[u - 1] != addr[i])
            addr[u++] = addr[i];
    if (u > GC_DISCOVERED_MAX)
        u = GC_DISCOVERED_MAX;
    /* records: the file's, then the new ones */
    size_t added_rec = 0;
    for (size_t i = 0; i < nrec; i++) {
        bool seen = false;
        for (size_t j = 0; j < nold_rec && !seen; j++)
            seen = !strcmp(old_rec[j], recs[i]);
        if (!seen)
            added_rec++;
    }
    int added = (int)(u - (had < u ? had : u)) + (int)added_rec;
    if (u == had && !added_rec) {
        added = 0; /* nothing new: the file stays as it is */
        goto done;
    }
    mkdirs_of(path);
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        added = -1;
        goto done;
    }
    fprintf(f, "# gc_runtime: code addresses the interpreter started at after a jump, and the REL modules loaded "
               "(REL id base bss hash co-loaded...)\n");
    for (size_t i = 0; i < u; i++)
        fprintf(f, "%08X\n", addr[i]);
    for (size_t j = 0; j < nold_rec; j++)
        fprintf(f, "%s\n", old_rec[j]);
    for (size_t i = 0; i < nrec; i++) {
        bool seen = false;
        for (size_t j = 0; j < nold_rec && !seen; j++)
            seen = !strcmp(old_rec[j], recs[i]);
        if (!seen && had_rec + i < GC_REL_RECORDS_MAX)
            fprintf(f, "%s\n", recs[i]);
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        remove(tmp);
        added = -1;
    }
done:
    for (size_t j = 0; j < nold_rec; j++)
        free(old_rec[j]);
    free(old_rec);
    free(addr);
    return added;
}
