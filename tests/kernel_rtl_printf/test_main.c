/*
 * RtlSprintf / RtlSnprintf / RtlVsprintf (ordinals 362, 361, 364): the guest's variadic arguments.
 *
 * The bridge formats with the host CRT and reads each argument from the guest stack. On the console
 * the callers are x86 cdecl: every argument is a multiple of 4 bytes at the next 4-byte slot, so a
 * double or an __int64 is two consecutive slots wherever they fall. The bridge used to round the
 * address of a 64-bit argument up to 8 (reading padding that is not there whenever the slot was at
 * 4 mod 8) and to convert a double's bit pattern as an integer ("%f" of 1.5 printed
 * 4609434218613702656.000000).
 *
 * Driven through the thunk table with a buffer standing in for guest memory, as
 * tests/kernel_io_ports does. Each check runs at both stack parities.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static uint8_t *mem;
#define M32(va) (*(volatile uint32_t *)(mem + (va)))

enum { THUNK_VA = 0x10000, STACK_VA = 0x20000, OUT_VA = 0x30000, FMT_VA = 0x30400, STR_VA = 0x30800,
       LIST_VA = 0x30900 };
enum { S_SNPRINTF, S_SPRINTF, S_VSPRINTF, S_COUNT };
static const uint32_t slot_ord[S_COUNT] = { 361, 362, 364 };

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* cdecl: a return address, then the arguments; shift moves the whole frame by 4 bytes. */
static uint32_t call(int slot, const uint32_t *args, int n, int shift)
{
    uint32_t sp = STACK_VA + 4u * (uint32_t)shift;
    int i;

    M32(sp) = 0xBEEF0000u | (uint32_t)slot;
    for (i = 0; i < n; i++)
        M32(sp + 4 + 4 * i) = args[i];
    g_esp = sp;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(M32(THUNK_VA + 4 * slot))();
    return g_eax;
}

/* 7, 1.5, -5 (64-bit), 0xABC, "end" as argument slots. */
static int varargs(uint32_t *a)
{
    const double one_and_half = 1.5;
    const long long minus_five = -5;
    int k = 0;

    a[k++] = 7;
    memcpy(&a[k], &one_and_half, 8); k += 2;
    memcpy(&a[k], &minus_five, 8);   k += 2;
    a[k++] = 0xABC;
    a[k++] = STR_VA;
    return k;
}

int main(void)
{
    static const char FMT[] = "%d|%.2f|%I64d|%x|%s", WANT[] = "7|1.50|-5|abc|end";
    const char *got;
    char d[160];
    int i, shift;

    setvbuf(stdout, NULL, _IONBF, 0);
    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("guest memory stand-in not allocated\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    got = (const char *)(mem + OUT_VA);
    for (i = 0; i < S_COUNT; i++)
        M32(THUNK_VA + 4 * i) = 0x80000000u | slot_ord[i];
    xbox_kernel_set_thunk_address(THUNK_VA, S_COUNT);
    xbox_kernel_init();
    xbox_kernel_bridge_init();
    for (i = 0; i < S_COUNT; i++)
        if (!recomp_lookup_kernel(M32(THUNK_VA + 4 * i))) {
            printf("ordinal %u did not resolve to a bridge entry point\n", slot_ord[i]);
            return 2;
        }
    strcpy((char *)(mem + FMT_VA), FMT);
    strcpy((char *)(mem + STR_VA), "end");

    printf("Rtl*printf: guest varargs\n");
    for (shift = 0; shift < 2; shift++) {
        uint32_t a[16], n;
        int k;

        /* RtlSprintf(dst, fmt, ...) */
        a[0] = OUT_VA;
        a[1] = FMT_VA;
        k = 2 + varargs(a + 2);
        memset(mem + OUT_VA, 0, 64);
        n = call(S_SPRINTF, a, k, shift);
        snprintf(d, sizeof d, "wrote \"%.60s\" (%u)", got, n);
        check(!strcmp(got, WANT) && n == sizeof WANT - 1,
              shift ? "RtlSprintf %d %.2f %I64d %x %s, frame moved by 4" : "RtlSprintf %d %.2f %I64d %x %s", d);

        /* RtlSnprintf(dst, count, fmt, ...): one more fixed argument, the other parity. */
        a[0] = OUT_VA;
        a[1] = 64;
        a[2] = FMT_VA;
        k = 3 + varargs(a + 3);
        memset(mem + OUT_VA, 0, 64);
        n = call(S_SNPRINTF, a, k, shift);
        snprintf(d, sizeof d, "wrote \"%.60s\" (%u)", got, n);
        check(!strcmp(got, WANT) && n == sizeof WANT - 1,
              shift ? "RtlSnprintf, frame moved by 4" : "RtlSnprintf", d);

        /* RtlVsprintf(dst, fmt, va_list): the list is a guest pointer to the same slots. */
        {
            uint32_t list[8], va = LIST_VA + 4u * (uint32_t)shift;
            int j, m = varargs(list);
            for (j = 0; j < m; j++)
                M32(va + 4 * j) = list[j];
            a[0] = OUT_VA;
            a[1] = FMT_VA;
            a[2] = va;
            memset(mem + OUT_VA, 0, 64);
            n = call(S_VSPRINTF, a, 3, 0);
            snprintf(d, sizeof d, "wrote \"%.60s\" (%u)", got, n);
            check(!strcmp(got, WANT) && n == sizeof WANT - 1,
                  shift ? "RtlVsprintf, list at 4 mod 8" : "RtlVsprintf", d);
        }
    }

    /* The longest conversion the parser accepts (7 flags, an 11-character width and precision, "ll") is
     * formatted without writing past the conversion buffer; the output is cut to the count. */
    {
        uint32_t a[8], n;
        strcpy((char *)(mem + FMT_VA), "%-+ 0#-+99999999999.99999999999lld!");
        a[0] = OUT_VA;
        a[1] = 32;
        a[2] = FMT_VA;
        a[3] = 42;
        a[4] = 0;
        memset(mem + OUT_VA, 0x55, 64);
        n = call(S_SNPRINTF, a, 5, 0);
        snprintf(d, sizeof d, "returned %u, byte after the buffer 0x%02X", n, (unsigned)(uint8_t)got[32]);
        check(n == 31 && got[31] == 0 && (uint8_t)got[32] == 0x55, "an over-long width and precision stay inside the count", d);
    }

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
