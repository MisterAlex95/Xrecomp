/* win32_runtime.c - guest memory, the PE loader, the import dispatch and the run of a recompiled Win32 program
 * (XBA-233). See win32_runtime.h. POSIX hosts (Linux x86-64 / arm64, macOS); the spike has no Windows host path. */
#include "win32_runtime.h"
#include "recomp_types.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── What the lifted code links against (recomp_types.h) ─── */
RECOMP_TLS RecompCpu g_cpu;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo, g_xbox_code_hi;
int g_force_return;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
volatile int g_recomp_cas64_lock;
int g_w32_trace;

/* A title's own overrides (its recomp_manual.c) win; without one, none. */
__attribute__((weak)) recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return NULL;
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[win32] indirect call to 0x%08X: no lifted function there\n", va);
}

void recomp_icall_not_code_log(uint32_t va)
{
    fprintf(stderr, "[win32] indirect call to 0x%08X: not code\n", va);
}

void recomp_unimpl(const char *text, uint32_t va)
{
    static uint32_t last;
    if (va != last)
        fprintf(stderr, "[win32] unimplemented instruction at 0x%08X: %s\n", va, text);
    last = va;
}

void recomp_divide_error(uint32_t va, uint32_t divisor)
{
    fprintf(stderr, "[win32] divide error at 0x%08X (divisor %u): no SEH in the spike, stopping\n", va, divisor);
    w32_exit(0xC0000094u);
}

/* A body the analysis cut short (after a call to an import that never returns, ExitProcess for one, it is never
 * reached) and string port I/O: stop, as the Xbox runtime does. */
void recomp_cut_body(uint32_t va)
{
    fflush(stdout);
    fprintf(stderr, "[win32] ran off a cut body at 0x%08X, stopping\n", va);
    w32_exit(W32_EXIT_MISSING);
}

void recomp_misdecoded(const char *text, uint32_t va)
{
    fflush(stdout);
    fprintf(stderr, "[win32] misdecoded instruction at 0x%08X (%s), stopping\n", va, text);
    w32_exit(W32_EXIT_MISSING);
}

/* cpuid: a Pentium III-class answer (MMX, SSE, CMOV, TSC, FPU), the Xbox runtime's, for now. */
void xbox_Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    (void)subleaf;
    if (leaf == 0) {
        out[0] = 2;
        memcpy(&out[1], "Genu", 4);
        memcpy(&out[3], "ineI", 4);
        memcpy(&out[2], "ntel", 4);
    } else if (leaf == 1) {
        out[0] = 0x0683; out[1] = 0; out[2] = 0;
        out[3] = 0x0383F9FFu;
    } else {
        out[0] = out[1] = out[2] = out[3] = 0;
    }
}

/* No save states here: a resume entry is never taken. */
uint32_t recomp_resume_site(uint32_t func) { (void)func; return 0; }
uint32_t recomp_resume_ebp(void) { return 0; }
uint32_t recomp_resume_target(void) { return 0; }
void recomp_resume_lost(uint32_t func, uint32_t site)
{
    fprintf(stderr, "[win32] resume of 0x%08X at 0x%08X: no save states in the Win32 runtime\n", func, site);
    abort();
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
int recomp_dispatch_init(void);

/* ── Guest memory ─────────────────────────────────────────── */
#define GUEST_SPAN (1ull << 32)
#define PAGE 0x1000u
static uint8_t *g_mem;

int w32_mem_init(void)
{
    if (g_mem)
        return 0;
    /* The whole 32-bit space, reserved without access; w32_commit opens what is used. */
    void *p = mmap(NULL, GUEST_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("[win32] reserving the 4 GiB guest space");
        return -1;
    }
    g_mem = (uint8_t *)p;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    return 0;
}

int w32_commit(uint32_t va, uint32_t size)
{
    uint64_t lo = va & ~(uint64_t)(PAGE - 1), hi = ((uint64_t)va + size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (!size || hi > GUEST_SPAN)
        return -1;
    return mprotect(g_mem + lo, (size_t)(hi - lo), PROT_READ | PROT_WRITE) ? -1 : 0;
}

void *w32_host(uint32_t va)
{
    return g_mem + va;
}

/* The guest heap: a bump allocator from 0x10000000, committed 1 MiB at a time. Each block keeps its size in the
 * 16 bytes before it (for a realloc); free does not give memory back (a spike). */
#define HEAP_LO  0x10000000u
#define HEAP_HI  0x70000000u
static uint32_t g_heap_top = HEAP_LO, g_heap_committed = HEAP_LO;

uint32_t w32_alloc(uint32_t size)
{
    uint32_t need = (size + 16u + 15u) & ~15u;
    if (size > HEAP_HI - HEAP_LO || need > HEAP_HI - g_heap_top)
        return 0;
    uint32_t block = g_heap_top + 16u, end = g_heap_top + need;
    if (end > g_heap_committed) {
        uint32_t grow = ((end - g_heap_committed) + 0xFFFFFu) & ~0xFFFFFu;
        if (w32_commit(g_heap_committed, grow))
            return 0;
        g_heap_committed += grow;
    }
    g_heap_top = end;
    w32_wr32(block - 16u, size);
    memset(w32_host(block), 0, size);
    return block;
}

uint32_t w32_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, va = w32_alloc(n);
    if (va)
        memcpy(w32_host(va), s, n);
    return va;
}

/* ── The PE loader ────────────────────────────────────────── */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const w32_export *g_slots[W32_THUNK_MAX];
static const char *g_slot_names[W32_THUNK_MAX];
static unsigned g_nslots;

static uint32_t resolve_import(w32_image *img, const char *dll, const char *name)
{
    char label[300];
    const w32_export *e = w32_find_export(dll, name);
    if (e && !e->fn) {                  /* data: its own guest block */
        uint32_t va = w32_alloc(e->data_size ? e->data_size : 4);
        w32_hle_data_init(e, va);
        img->imports++;
        return va;
    }
    if (g_nslots >= W32_THUNK_MAX)
        return 0;
    snprintf(label, sizeof label, "%s!%s", dll, name);
    g_slots[g_nslots] = e;              /* NULL: no HLE, reported when called */
    g_slot_names[g_nslots] = strdup(label);
    if (e)
        img->imports++;
    else {
        img->missing++;
        if (g_w32_trace)
            fprintf(stderr, "[win32] import %s has no HLE (fatal if called)\n", label);
    }
    return W32_THUNK_BASE + 4u * g_nslots++;
}

int w32_load(const char *path, w32_image *img)
{
    FILE *f = fopen(path, "rb");
    uint8_t *file = NULL;
    long len;
    memset(img, 0, sizeof *img);
    if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0x200 || fseek(f, 0, SEEK_SET)
        || !(file = malloc((size_t)len)) || fread(file, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "[win32] cannot read %s\n", path);
        if (f) fclose(f);
        free(file);
        return -1;
    }
    fclose(f);
    uint32_t nt = rd32(file + 0x3C);
    /* 64-bit: e_lfanew near 4 GiB wrapped the sum and passed the check. */
    if (file[0] != 'M' || file[1] != 'Z' || (uint64_t)nt + 0xF8 > (uint64_t)len || memcmp(file + nt, "PE\0\0", 4)
        || rd16(file + nt + 4) != 0x014C || rd16(file + nt + 24) != 0x10B) {
        fprintf(stderr, "[win32] %s: not a PE32 i386 image\n", path);
        free(file);
        return -1;
    }
    const uint8_t *fh = file + nt + 4, *opt = file + nt + 24;
    unsigned nsec = rd16(fh + 2), optsize = rd16(fh + 16);
    img->base = rd32(opt + 28);
    img->size = rd32(opt + 56);
    img->entry = img->base + rd32(opt + 16);
    img->stack_reserve = rd32(opt + 72);
    uint32_t headers = rd32(opt + 60), ndirs = rd32(opt + 92);
    uint32_t import_rva = ndirs > 1 ? rd32(opt + 96 + 8) : 0, tls_rva = ndirs > 9 ? rd32(opt + 96 + 72) : 0;
    const uint8_t *sec = opt + optsize;
    if ((uint64_t)nt + 24 + optsize + nsec * 40u > (uint64_t)len || headers > (uint64_t)len) {
        fprintf(stderr, "[win32] %s: section table outside the file\n", path);
        free(file);
        return -1;
    }
    /* The headers are copied to the image's base: no more than it commits. */
    if (headers > img->size) {
        fprintf(stderr, "[win32] %s: headers (0x%X bytes) larger than the image (0x%X)\n", path, headers,
                img->size);
        free(file);
        return -1;
    }

    /* Map at the preferred base: headers, then each section and its zero fill. */
    if (w32_commit(img->base, img->size)) {
        fprintf(stderr, "[win32] cannot map 0x%X bytes at 0x%08X\n", img->size, img->base);
        free(file);
        return -1;
    }
    memcpy(w32_host(img->base), file, headers);
    img->code_lo = 0xFFFFFFFFu;
    for (unsigned i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize = rd32(sec + 8), rva = rd32(sec + 12), raw = rd32(sec + 16), off = rd32(sec + 20);
        uint32_t chars = rd32(sec + 36);
        if (!vsize)
            vsize = raw;
        if (raw > vsize)
            raw = vsize;
        if ((uint64_t)off + raw > (uint64_t)len || (uint64_t)rva + vsize > img->size) {
            fprintf(stderr, "[win32] %s: section %u outside the file or the image\n", path, i);
            free(file);
            return -1;
        }
        if (!(chars & 0x80))            /* not uninitialized data */
            memcpy(w32_host(img->base + rva), file + off, raw);
        if (chars & 0x20000020u) {      /* code */
            if (img->base + rva < img->code_lo) img->code_lo = img->base + rva;
            if (img->base + rva + vsize > img->code_hi) img->code_hi = img->base + rva + vsize;
        }
    }
    free(file);

    /* Imports, from the mapped image. */
    if (import_rva) {
        for (uint32_t d = img->base + import_rva; w32_rd32(d + 12); d += 20) {
            uint32_t ilt = w32_rd32(d), iat = w32_rd32(d + 16);
            const char *dll = (const char *)w32_host(img->base + w32_rd32(d + 12));
            uint32_t names = img->base + (ilt ? ilt : iat), slots = img->base + iat;
            for (uint32_t t; (t = w32_rd32(names)) != 0; names += 4, slots += 4) {
                char ord[16];
                const char *name = ord;
                if (t & 0x80000000u)
                    snprintf(ord, sizeof ord, "#%u", t & 0xFFFFu);
                else
                    name = (const char *)w32_host(img->base + t + 2);
                w32_wr32(slots, resolve_import(img, dll, name));
            }
        }
    }
    img->tls_dir = tls_rva ? img->base + tls_rva : 0;
    return 0;
}

/* ── Import dispatch ──────────────────────────────────────── */
static RECOMP_TLS unsigned g_call_slot;

uint32_t w32_arg_va(int i)
{
    return g_esp + 4u + 4u * (uint32_t)i;
}

uint32_t w32_arg(int i)
{
    return w32_rd32(w32_arg_va(i));
}

static void import_thunk(void)
{
    unsigned slot = g_call_slot;
    const w32_export *e = g_slots[slot];
    if (!e || !e->fn) {
        fflush(stdout);
        fprintf(stderr, "[win32] %s called: no HLE for it, stopping\n", g_slot_names[slot]);
        w32_exit(W32_EXIT_MISSING);
    }
    if (g_w32_trace)
        fprintf(stderr, "[win32] %s(0x%X, 0x%X, 0x%X, 0x%X)\n", g_slot_names[slot],
                w32_arg(0), w32_arg(1), w32_arg(2), w32_arg(3));
    uint32_t r = e->fn();
    g_eax = r;
    g_esp += 4u + (e->stdcall_args > 0 ? 4u * (uint32_t)e->stdcall_args : 0u);
}

recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= W32_THUNK_BASE && xbox_va < W32_THUNK_BASE + 4u * g_nslots && !(xbox_va & 3u)) {
        g_call_slot = (xbox_va - W32_THUNK_BASE) / 4u;
        return import_thunk;
    }
    return NULL;
}

#define RETURN_SENTINEL 0xFEDFFFF0u     /* the return address a host-made call pushes (never executed) */

uint32_t w32_call_guest(uint32_t va, const uint32_t *args, int n)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        fprintf(stderr, "[win32] call to 0x%08X: no lifted function there\n", va);
        return 0;
    }
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        w32_wr32(g_esp, args[i]);
    }
    g_esp -= 4;
    w32_wr32(g_esp, RETURN_SENTINEL);
    fn();
    g_esp = esp0;                       /* stdcall or cdecl alike */
    return g_eax;
}

/* ── The process ──────────────────────────────────────────── */
static jmp_buf g_exit_jmp;
static uint32_t g_exit_code;
static int g_running;

void w32_exit(uint32_t code)
{
    fflush(stdout);
    g_exit_code = code;
    if (g_running)
        longjmp(g_exit_jmp, 1);
    exit((int)(code & 0xFF));
}

#define TEB_VA 0x7FFDE000u
#define PEB_VA 0x7FFDF000u

static void setup_tls(const w32_image *img)
{
    /* IMAGE_TLS_DIRECTORY32: StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks,
     * SizeOfZeroFill. One module, one thread: slot 0, the template copied once. */
    uint32_t d = img->tls_dir, start = w32_rd32(d), end = w32_rd32(d + 4), zero = w32_rd32(d + 16);
    uint32_t block = w32_alloc(end - start + zero + 4), slots = w32_alloc(64 * 4);
    if (end > start)
        memcpy(w32_host(block), w32_host(start), end - start);
    w32_wr32(slots, block);
    w32_wr32(TEB_VA + 0x2C, slots);     /* ThreadLocalStoragePointer */
    if (w32_rd32(d + 8))
        w32_wr32(w32_rd32(d + 8), 0);
}

static void run_tls_callbacks(const w32_image *img, uint32_t reason)
{
    uint32_t cbs = img->tls_dir ? w32_rd32(img->tls_dir + 12) : 0;
    for (uint32_t cb; cbs && (cb = w32_rd32(cbs)) != 0; cbs += 4) {
        uint32_t args[3] = {img->base, reason, 0};
        w32_call_guest(cb, args, 3);
    }
}

int w32_main(int argc, char **argv)
{
    w32_image img;
    const char *t = getenv("XBR_WIN32_TRACE");
    g_w32_trace = t && *t && *t != '0';
    if (argc < 2) {
        fprintf(stderr, "usage: %s PROGRAM.exe [ARGS...]\n", argv[0]);
        return 2;
    }
    if (w32_mem_init() || w32_load(argv[1], &img))
        return 1;
    w32_hle_set_args(argc - 1, argv + 1);
    g_w32_image_base = img.base;
    g_xbox_code_lo = img.code_lo;
    g_xbox_code_hi = img.code_hi;
    if (g_w32_trace)
        fprintf(stderr, "[win32] %s at 0x%08X, entry 0x%08X, %u imports (%u without HLE)\n", argv[1], img.base,
                img.entry, img.imports + img.missing, img.missing);

    /* The stack (the header's reserve, at least 1 MiB), the TEB and the PEB. */
    uint32_t reserve = img.stack_reserve < 0x100000u ? 0x100000u : img.stack_reserve;
    uint32_t stack = w32_alloc(reserve);
    if (!stack || w32_commit(TEB_VA, 0x2000)) {
        fprintf(stderr, "[win32] no memory for the stack or the TEB\n");
        return 1;
    }
    w32_wr32(TEB_VA + 0x00, 0xFFFFFFFFu);           /* ExceptionList: end of the SEH chain */
    w32_wr32(TEB_VA + 0x04, stack + reserve);       /* StackBase */
    w32_wr32(TEB_VA + 0x08, stack);                 /* StackLimit */
    w32_wr32(TEB_VA + 0x18, TEB_VA);                /* Self */
    w32_wr32(TEB_VA + 0x20, (uint32_t)getpid());    /* ClientId */
    w32_wr32(TEB_VA + 0x24, 1);
    w32_wr32(TEB_VA + 0x30, PEB_VA);
    w32_wr32(PEB_VA + 0x08, img.base);              /* ImageBaseAddress */
    g_fs_base = TEB_VA;
    g_esp = stack + reserve - 16u;
    g_cpu.r_fp_control_word = 0x027F;
    g_cpu.r_mxcsr = 0x1F80;

    if (img.tls_dir)
        setup_tls(&img);
    recomp_dispatch_init();                         /* the generated flat dispatch table */

    g_running = 1;
    if (setjmp(g_exit_jmp) == 0) {
        run_tls_callbacks(&img, 1);                 /* DLL_PROCESS_ATTACH */
        uint32_t r = w32_call_guest(img.entry, NULL, 0);
        fflush(stdout);
        g_exit_code = r;                            /* an entry point that returns: its eax */
    }
    g_running = 0;
    fflush(stdout);
    if (g_w32_trace)
        fprintf(stderr, "[win32] exit code %u\n", g_exit_code);
    return (int)(g_exit_code & 0xFF);
}
