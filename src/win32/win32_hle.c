/* win32_hle.c - the kernel32 and msvcrt functions a recompiled console program calls (XBA-233).
 *
 * The spike's subset: what a mingw console hello world imports, with and without the CRT (tests/win32_hello). Each
 * function reads its arguments off the guest stack (w32_arg), works on guest memory (w32_host) and returns eax; the
 * table says how many argument words it pops (stdcall) or that the caller does (cdecl). One thread, so the critical
 * sections are no-ops; the heap never frees; standard handles are the host's stdin, stdout and stderr. */
#include "win32_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint32_t g_w32_image_base;

static uint32_t g_last_error;
static int g_argc;
static char **g_argv;

void w32_hle_set_args(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
}

static char *gstr(uint32_t va) { return (char *)w32_host(va); }

/* ── Handles and files ────────────────────────────────────── */
/* Standard handles: 0x10, 0x14, 0x18 for host fds 0, 1, 2. A msvcrt FILE (32 bytes) keeps its fd at +16. */
#define STD_HANDLE(fd) (0x10u + 4u * (fd))

static int handle_fd(uint32_t h)
{
    return h >= STD_HANDLE(0) && h <= STD_HANDLE(2) && !(h & 3u) ? (int)((h - 0x10u) / 4u) : -1;
}

static int file_fd(uint32_t fp)
{
    return fp ? (int)w32_rd32(fp + 16) : -1;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    if (fd == 1)
        fflush(stdout);
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* ── kernel32 ─────────────────────────────────────────────── */
static uint32_t k32_ExitProcess(void) { w32_exit(w32_arg(0)); }

static uint32_t k32_GetStdHandle(void)
{
    int32_t which = (int32_t)w32_arg(0);
    if (which >= -12 && which <= -10)
        return STD_HANDLE((uint32_t)(-10 - which));
    g_last_error = 6;                   /* ERROR_INVALID_HANDLE */
    return 0xFFFFFFFFu;                 /* INVALID_HANDLE_VALUE */
}

static uint32_t k32_WriteFile(void)
{
    uint32_t h = w32_arg(0), buf = w32_arg(1), n = w32_arg(2), written = w32_arg(3);
    int fd = handle_fd(h);
    if (fd < 0 || write_all(fd, w32_host(buf), n)) {
        g_last_error = 6;
        if (written) w32_wr32(written, 0);
        return 0;
    }
    if (written) w32_wr32(written, n);
    return 1;
}

static uint32_t k32_GetLastError(void) { return g_last_error; }
static uint32_t k32_SetLastError(void) { g_last_error = w32_arg(0); return 0; }
static uint32_t k32_nop0(void) { return 0; }
static uint32_t k32_true(void) { return 1; }

static uint32_t k32_GetModuleHandle(void)
{
    return w32_arg(0) ? 0 : g_w32_image_base;   /* only the program itself is loaded */
}

static uint32_t k32_GetCommandLineA(void)
{
    static uint32_t line;
    if (!line) {
        char buf[4096] = "";
        for (int i = 0; i < g_argc; i++) {
            if (i) strncat(buf, " ", sizeof buf - strlen(buf) - 1);
            strncat(buf, g_argv[i], sizeof buf - strlen(buf) - 1);
        }
        line = w32_strdup(buf);
    }
    return line;
}

static uint32_t k32_VirtualProtect(void)
{
    if (w32_arg(3)) w32_wr32(w32_arg(3), 0x04);  /* old protection: PAGE_READWRITE, as everything is */
    return 1;
}

static uint32_t k32_VirtualQuery(void)
{
    uint32_t addr = w32_arg(0), mbi = w32_arg(1), len = w32_arg(2);
    if (len < 28)
        return 0;
    w32_wr32(mbi + 0, addr & ~0xFFFu);          /* BaseAddress */
    w32_wr32(mbi + 4, g_w32_image_base);        /* AllocationBase */
    w32_wr32(mbi + 8, 0x04);                    /* AllocationProtect: PAGE_READWRITE */
    w32_wr32(mbi + 12, 0x1000);                 /* RegionSize */
    w32_wr32(mbi + 16, 0x1000);                 /* State: MEM_COMMIT */
    w32_wr32(mbi + 20, 0x04);                   /* Protect */
    w32_wr32(mbi + 24, 0x1000000);              /* Type: MEM_IMAGE */
    return 28;
}

static uint32_t k32_MultiByteToWideChar(void)
{
    uint32_t src = w32_arg(2), dst = w32_arg(4), dstlen = w32_arg(5);
    int32_t n = (int32_t)w32_arg(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (!dstlen)
        return (uint32_t)n;
    if ((uint32_t)n > dstlen)
        return 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = (uint8_t)gstr(src)[i];
        memcpy(gstr(dst + 2u * (uint32_t)i), &c, 2);
    }
    return (uint32_t)n;
}

static uint32_t k32_WideCharToMultiByte(void)
{
    uint32_t src = w32_arg(2), dst = w32_arg(4), dstlen = w32_arg(5);
    int32_t n = (int32_t)w32_arg(3);
    if (n < 0) {
        uint16_t c;
        n = 0;
        do memcpy(&c, gstr(src + 2u * (uint32_t)n++), 2); while (c);
    }
    if (!dstlen)
        return (uint32_t)n;
    if ((uint32_t)n > dstlen)
        return 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t c;
        memcpy(&c, gstr(src + 2u * (uint32_t)i), 2);
        gstr(dst)[i] = c < 0x80 ? (char)c : '?';
    }
    return (uint32_t)n;
}

/* ── msvcrt ───────────────────────────────────────────────── */
static uint32_t g_errno_va, g_fmode_va, g_commode_va;
static uint32_t g_onexit[64];
static unsigned g_nonexit;

static uint32_t guest_int(uint32_t *slot)
{
    if (!*slot) *slot = w32_alloc(4);
    return *slot;
}

static uint32_t crt_errno(void) { return guest_int(&g_errno_va); }
static uint32_t crt_p_fmode(void) { return guest_int(&g_fmode_va); }
static uint32_t crt_p_commode(void) { return guest_int(&g_commode_va); }

static uint32_t crt_getmainargs(void)
{
    uint32_t argv = w32_alloc(4u * ((uint32_t)g_argc + 1u)), envp = w32_alloc(4);
    for (int i = 0; i < g_argc; i++)
        w32_wr32(argv + 4u * (uint32_t)i, w32_strdup(g_argv[i]));
    w32_wr32(w32_arg(0), (uint32_t)g_argc);
    w32_wr32(w32_arg(1), argv);
    w32_wr32(w32_arg(2), envp);
    return 0;
}

static uint32_t crt_initterm(void)
{
    for (uint32_t p = w32_arg(0), end = w32_arg(1); p < end; p += 4) {
        uint32_t fn = w32_rd32(p);
        if (fn)
            w32_call_guest(fn, NULL, 0);
    }
    return 0;
}

static uint32_t crt_onexit(void)
{
    if (g_nonexit >= sizeof g_onexit / sizeof g_onexit[0])
        return 0;
    g_onexit[g_nonexit++] = w32_arg(0);
    return w32_arg(0);
}

static void run_onexit(void)
{
    while (g_nonexit)
        w32_call_guest(g_onexit[--g_nonexit], NULL, 0);
}

static uint32_t crt_cexit(void) { run_onexit(); return 0; }
static uint32_t crt_exit(void) { uint32_t code = w32_arg(0); run_onexit(); w32_exit(code); }
static uint32_t crt_amsg_exit(void) { fprintf(stderr, "[win32] _amsg_exit(%u)\n", w32_arg(0)); w32_exit(255); }
static uint32_t crt_abort(void) { fprintf(stderr, "[win32] abort()\n"); w32_exit(3); }

static uint32_t crt_malloc(void) { return w32_alloc(w32_arg(0)); }
static uint32_t crt_calloc(void) { return w32_alloc(w32_arg(0) * w32_arg(1)); }
static uint32_t crt_memcpy(void) { memmove(w32_host(w32_arg(0)), w32_host(w32_arg(1)), w32_arg(2)); return w32_arg(0); }
static uint32_t crt_memset(void) { memset(w32_host(w32_arg(0)), (int)w32_arg(1), w32_arg(2)); return w32_arg(0); }
static uint32_t crt_strlen(void) { return (uint32_t)strlen(gstr(w32_arg(0))); }
static uint32_t crt_strncmp(void) { return (uint32_t)strncmp(gstr(w32_arg(0)), gstr(w32_arg(1)), w32_arg(2)); }
static uint32_t crt_atoi(void) { return (uint32_t)atoi(gstr(w32_arg(0))); }

static uint32_t crt_strchr(void)
{
    const char *s = gstr(w32_arg(0)), *hit = strchr(s, (int)(w32_arg(1) & 0xFF));
    return hit ? w32_arg(0) + (uint32_t)(hit - s) : 0;
}

static uint32_t crt_wcslen(void)
{
    uint32_t n = 0;
    uint16_t c;
    for (;; n++) {
        memcpy(&c, gstr(w32_arg(0) + 2u * n), 2);
        if (!c) return n;
    }
}

static uint32_t crt_strerror(void)
{
    static uint32_t s;
    return s ? s : (s = w32_strdup("error"));
}

static uint32_t crt_setlocale(void)
{
    static uint32_t c;
    return c ? c : (c = w32_strdup("C"));
}

static uint32_t crt_localeconv(void)
{
    static uint32_t lc;
    if (!lc) {                          /* struct lconv: decimal_point, thousands_sep, grouping, ... (all "") */
        uint32_t empty = w32_strdup("");
        lc = w32_alloc(18 * 4);
        for (int i = 0; i < 10; i++)
            w32_wr32(lc + 4u * (uint32_t)i, empty);
        w32_wr32(lc, w32_strdup("."));
    }
    return lc;
}

static uint32_t crt_fwrite(void)
{
    uint32_t size = w32_arg(1), n = w32_arg(2);
    return write_all(file_fd(w32_arg(3)), w32_host(w32_arg(0)), (size_t)size * n) ? 0 : n;
}

static uint32_t crt_fputc(void)
{
    char c = (char)w32_arg(0);
    return write_all(file_fd(w32_arg(1)), &c, 1) ? 0xFFFFFFFFu : (uint8_t)c;
}

/* printf-style formatting over guest arguments: the conversions an error message needs (d i u x X o c s p %, with
 * flags, width, precision and the l/h/ll sizes; floating point printed through the host). */
static void guest_format(int fd, const char *fmt, uint32_t ap)
{
    char out[4096];
    size_t n = 0;
    for (const char *p = fmt; *p && n < sizeof out - 64; p++) {
        if (*p != '%') { out[n++] = *p; continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = '%';
        for (p++; *p && strchr("-+ #0123456789.", *p) && k < 20; p++)
            spec[k++] = *p;
        int ll = 0;
        while (*p == 'l' || *p == 'h') { if (*p == 'l') ll++; p++; }
        char conv = *p;
        if (!conv) break;
        spec[k++] = conv;
        spec[k] = 0;
        size_t room = sizeof out - n;
        int w = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (ll >= 2) {
                uint64_t v = w32_rd32(ap) | (uint64_t)w32_rd32(ap + 4) << 32;
                ap += 8;
                spec[k - 1] = 0;
                char s2[40];
                snprintf(s2, sizeof s2, "%sll%c", spec, conv);
                w = snprintf(out + n, room, s2, v);
            } else {
                w = snprintf(out + n, room, spec, w32_rd32(ap));
                ap += 4;
            }
            break;
        case 's': w = snprintf(out + n, room, spec, w32_rd32(ap) ? gstr(w32_rd32(ap)) : "(null)"); ap += 4; break;
        case 'p': w = snprintf(out + n, room, "%08X", w32_rd32(ap)); ap += 4; break;
        case 'f': case 'g': case 'e': case 'G': case 'E': {
            double d;
            memcpy(&d, w32_host(ap), 8);
            ap += 8;
            w = snprintf(out + n, room, spec, d);
            break;
        }
        case '%': out[n++] = '%'; break;
        default: break;
        }
        if (w > 0) n += (size_t)w < room ? (size_t)w : room - 1;
    }
    write_all(fd, out, n);
}

static uint32_t crt_fprintf(void) { guest_format(file_fd(w32_arg(0)), gstr(w32_arg(1)), w32_arg_va(2)); return 0; }
static uint32_t crt_vfprintf(void) { guest_format(file_fd(w32_arg(0)), gstr(w32_arg(1)), w32_arg(2)); return 0; }

void w32_hle_data_init(const w32_export *e, uint32_t va)
{
    if (!strcmp(e->name, "_iob")) {             /* stdin, stdout, stderr: FILE._file at +16 */
        for (uint32_t i = 0; i < 3; i++)
            w32_wr32(va + 32u * i + 16u, i);
    } else if (!strcmp(e->name, "__mb_cur_max")) {
        w32_wr32(va, 1);
    } else if (!strcmp(e->name, "__initenv")) {
        w32_wr32(va, w32_alloc(4));             /* char ** -> an empty environment */
    }
}

/* ── The table ────────────────────────────────────────────── */
#define K32(name, fn, n) {"kernel32.dll", name, fn, n, 0}
#define CRT(name, fn) {"msvcrt.dll", name, fn, -1, 0}
#define CRT_DATA(name, size) {"msvcrt.dll", name, NULL, 0, size}

static const w32_export g_exports[] = {
    K32("ExitProcess", k32_ExitProcess, 1),
    K32("GetStdHandle", k32_GetStdHandle, 1),
    K32("WriteFile", k32_WriteFile, 5),
    K32("GetLastError", k32_GetLastError, 0),
    K32("SetLastError", k32_SetLastError, 1),
    K32("GetCommandLineA", k32_GetCommandLineA, 0),
    K32("GetModuleHandleA", k32_GetModuleHandle, 1),
    K32("GetModuleHandleW", k32_GetModuleHandle, 1),
    K32("GetProcAddress", k32_nop0, 2),
    K32("LoadLibraryA", k32_nop0, 1),
    K32("FreeLibrary", k32_true, 1),
    K32("InitializeCriticalSection", k32_nop0, 1),
    K32("DeleteCriticalSection", k32_nop0, 1),
    K32("EnterCriticalSection", k32_nop0, 1),
    K32("LeaveCriticalSection", k32_nop0, 1),
    K32("IsDBCSLeadByteEx", k32_nop0, 2),
    K32("MultiByteToWideChar", k32_MultiByteToWideChar, 6),
    K32("WideCharToMultiByte", k32_WideCharToMultiByte, 8),
    K32("SetUnhandledExceptionFilter", k32_nop0, 1),
    K32("Sleep", k32_nop0, 1),
    K32("TlsGetValue", k32_nop0, 1),
    K32("VirtualProtect", k32_VirtualProtect, 4),
    K32("VirtualQuery", k32_VirtualQuery, 3),

    CRT("__getmainargs", crt_getmainargs),
    CRT("__p__commode", crt_p_commode),
    CRT("__p__fmode", crt_p_fmode),
    CRT("__set_app_type", k32_nop0),
    CRT("__setusermatherr", k32_nop0),
    CRT("_amsg_exit", crt_amsg_exit),
    CRT("_cexit", crt_cexit),
    CRT("_errno", crt_errno),
    CRT("_initterm", crt_initterm),
    CRT("_lock", k32_nop0),
    CRT("_unlock", k32_nop0),
    CRT("_onexit", crt_onexit),
    CRT("abort", crt_abort),
    CRT("atoi", crt_atoi),
    CRT("calloc", crt_calloc),
    CRT("exit", crt_exit),
    CRT("fprintf", crt_fprintf),
    CRT("fputc", crt_fputc),
    CRT("free", k32_nop0),
    CRT("fwrite", crt_fwrite),
    CRT("localeconv", crt_localeconv),
    CRT("malloc", crt_malloc),
    CRT("memcpy", crt_memcpy),
    CRT("memset", crt_memset),
    CRT("setlocale", crt_setlocale),
    CRT("signal", k32_nop0),
    CRT("strchr", crt_strchr),
    CRT("strerror", crt_strerror),
    CRT("strlen", crt_strlen),
    CRT("strncmp", crt_strncmp),
    CRT("vfprintf", crt_vfprintf),
    CRT("wcslen", crt_wcslen),
    CRT_DATA("_iob", 3 * 32),
    CRT_DATA("__initenv", 4),
    CRT_DATA("__mb_cur_max", 4),
};

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return *a == *b;
}

const w32_export *w32_find_export(const char *dll, const char *name)
{
    for (size_t i = 0; i < sizeof g_exports / sizeof g_exports[0]; i++)
        if (ieq(g_exports[i].dll, dll) && !strcmp(g_exports[i].name, name))
            return &g_exports[i];
    return NULL;
}
