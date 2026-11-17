/*
 * The kernel bridge.
 *
 * A title does not call the kernel by name. Its indirect calls land on a
 * thunk table that xbox_kernel_bridge_init() has rewritten to synthetic VAs,
 * and kernel_thunk_dispatch turns each of those into a bridge_* function.
 * That is the path the runtime actually executes, and it is easy to leave
 * untested: bridge_* functions are static and read guest CPU state rather
 * than taking arguments, so they cannot be called directly.
 *
 * They are reachable all the same. recomp_lookup_kernel is not static: given
 * a buffer standing in for guest memory it selects the slot and hands back
 * the dispatcher. tests/kernel_directory uses the same seam for the directory
 * ABI; this one covers kernel memory ordinals.
 *
 * Checked here with ordinal 173, MmGetPhysicalAddress, which had two
 * implementations that disagreed -- the bridge translating through the
 * contiguous window and xbox_MmGetPhysicalAddress returning its argument
 * unchanged. The bridge now calls the latter, and that delegation is what
 * these checks pin: asserting only on xbox_MmGetPhysicalAddress leaves a
 * bridge that stops delegating entirely green.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; a regression harness has no title, and a
 * static archive resolves only what gets pulled in. Nothing here calls a
 * guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* The seam, plus the guest CPU state the dispatcher reads and writes. */
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    const uint32_t THUNK_VA = 0x10000, STACK_VA = 0x20000;

    uint8_t *mem = VirtualAlloc(NULL, 16 * 1024 * 1024,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(mem != NULL, "guest memory stand-in allocated", NULL);
    if (!mem) {
        printf("\n%d failure(s); the rest needs guest memory.\n", failures);
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;

    /* Two imports, in the form an XBE stores them: 0x80000000 | ordinal. */
    *(uint32_t *)(mem + THUNK_VA) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA + 4) = 0x80000000u | 197u;
    *(uint32_t *)(mem + THUNK_VA + 8) = 0x80000000u | 67u;    /* IoCreateSymbolicLink */
    *(uint32_t *)(mem + THUNK_VA + 12) = 0x80000000u | 203u;  /* NtOpenSymbolicLinkObject */
    *(uint32_t *)(mem + THUNK_VA + 16) = 0x80000000u | 215u;  /* NtQuerySymbolicLinkObject */
    *(uint32_t *)(mem + THUNK_VA - 4) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA - 8) = 0xF2345678u;
    *(uint32_t *)(mem + THUNK_VA - 12) = 0x80000000u;
    *(uint32_t *)(mem + THUNK_VA - 16) = 0x80000000u | 379u;
    xbox_kernel_set_thunk_address(THUNK_VA, 5);
    xbox_kernel_bridge_init();

    check(recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA - 4)) != NULL,
          "a valid lookbehind ordinal still resolves", NULL);
    check(*(uint32_t *)(mem + THUNK_VA - 8) == 0xF2345678u,
          "lookbehind preserves unrelated high-bit data", NULL);
    check(*(uint32_t *)(mem + THUNK_VA - 12) == 0x80000000u,
          "lookbehind preserves ordinal zero", NULL);
    check(*(uint32_t *)(mem + THUNK_VA - 16) == (0x80000000u | 379u),
          "lookbehind preserves an out-of-range ordinal", NULL);

    /* bridge_init rewrites the entry in place to a synthetic VA. */
    recomp_func_t fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA));
    check(fn != NULL, "ordinal 173 resolves to a bridge entry point",
          "the thunk entry was not rewritten to a synthetic VA");

    if (fn) {
        /* stdcall: the guest pushed a return address, then the argument. */
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        sp[0] = 0xBEEF0001u;          /* guest return address */
        sp[1] = 0x80001000u;          /* inside the contiguous window */
        g_esp = STACK_VA;
        g_eax = 0xDEADBEEFu;
        fn();

        char detail[128];
        snprintf(detail, sizeof detail,
                 "bridge returned 0x%08X, expected 0x00001000", g_eax);
        check(g_eax == 0x00001000u,
              "ordinal 173 translates through the contiguous window", detail);

        /* And the argument must come off the guest stack. A wrong
         * stdcall_args_for_ordinal entry corrupts the caller's frame away
         * from the call, with nothing naming the ordinal. */
        snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                 g_esp, STACK_VA + 8);
        check(g_esp == STACK_VA + 8,
              "ordinal 173 pops its stdcall argument", detail);
    }

    /* Ordinal 197, NtDuplicateObject(Source, &Target, Options), on the
     * guest's pseudo-handles. They are 32-bit -1 (current process) and -2
     * (current thread); widened without sign extension they are no handle
     * at all, and the duplicate fails (Windows) or faults (POSIX shim). A
     * title duplicating NtCurrentThread() to keep a handle to itself is the
     * case that found this. */
    fn = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 4));
    check(fn != NULL, "ordinal 197 resolves to a bridge entry point", NULL);
    if (fn) {
        static const struct { uint32_t src; const char *what; } pseudo[] = {
            { 0xFFFFFFFEu, "NtDuplicateObject(NtCurrentThread()) succeeds" },
            { 0xFFFFFFFFu, "NtDuplicateObject(NtCurrentProcess()) succeeds" },
        };
        const uint32_t TARGET_VA = 0x30000;
        for (size_t k = 0; k < sizeof pseudo / sizeof pseudo[0]; k++) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA);
            sp[0] = 0xBEEF0002u;      /* guest return address */
            sp[1] = pseudo[k].src;    /* SourceHandle */
            sp[2] = TARGET_VA;        /* TargetHandle (out) */
            sp[3] = 0x2u;             /* DUPLICATE_SAME_ACCESS */
            *(uint32_t *)(mem + TARGET_VA) = 0;
            g_esp = STACK_VA;
            g_eax = 0xDEADBEEFu;
            fn();

            char detail[128];
            snprintf(detail, sizeof detail,
                     "status 0x%08X, target 0x%08X", g_eax,
                     *(uint32_t *)(mem + TARGET_VA));
            check(g_eax == 0 && *(uint32_t *)(mem + TARGET_VA) != 0,
                  pseudo[k].what, detail);
            snprintf(detail, sizeof detail, "esp is 0x%08X, expected 0x%08X",
                     g_esp, STACK_VA + 16);
            check(g_esp == STACK_VA + 16,
                  "ordinal 197 pops its three stdcall arguments", detail);
        }
    }

    /* KRN-14: NtOpenSymbolicLinkObject resolves the name and
     * NtQuerySymbolicLinkObject answers the link's own target; both said
     * \Device\CdRom0 for anything. The dispatcher a lookup returns serves
     * the slot looked up last, so each call looks its slot up again. */
    {
        recomp_func_t mk = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 8));
        recomp_func_t op = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 12));
        recomp_func_t qu = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 16));
        const uint32_t S = 0x40000;   /* strings and out-parameters live here */
        char detail[160];
        check(mk && op && qu, "ordinals 67, 203, 215 resolve", NULL);
        if (mk && op && qu) {
            uint32_t *sp = (uint32_t *)(mem + STACK_VA), h;
            /* ANSI_STRINGs: link at S, target at S+0x10, name for open at
             * S+0x20 (in an OBJECT_ATTRIBUTES at S+0x30), out string S+0x40 */
            #define ANSI(at, text) do { strcpy((char *)mem + (at) + 0x100, text); \
                *(uint16_t *)(mem + (at)) = (uint16_t)strlen(text); \
                *(uint16_t *)(mem + (at) + 2) = (uint16_t)strlen(text) + 1; \
                *(uint32_t *)(mem + (at) + 4) = (at) + 0x100; } while (0)
            ANSI(S, "\\??\\w:");
            ANSI(S + 0x10, "\\Device\\Harddisk0\\Partition1\\TDATA\\4d530004");
            sp[0] = 0xBEEF0003u; sp[1] = S; sp[2] = S + 0x10;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 8))();
            check(g_eax == 0, "IoCreateSymbolicLink(\\??\\w:) succeeds", NULL);

            *(uint32_t *)(mem + S + 0x30) = 0;
            *(uint32_t *)(mem + S + 0x34) = S + 0x20;
            *(uint32_t *)(mem + S + 0x38) = 0x40;
            ANSI(S + 0x20, "\\??\\W:");
            *(uint32_t *)(mem + S + 0x50) = 0;
            sp[0] = 0xBEEF0004u; sp[1] = S + 0x50; sp[2] = S + 0x30;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 12))();
            h = *(uint32_t *)(mem + S + 0x50);
            snprintf(detail, sizeof detail, "status 0x%08X handle 0x%08X", g_eax, h);
            check(g_eax == 0 && h, "open \\??\\W: (made as w:) succeeds", detail);

            *(uint16_t *)(mem + S + 0x40) = 0;
            *(uint16_t *)(mem + S + 0x42) = 128;
            *(uint32_t *)(mem + S + 0x44) = S + 0x200;
            sp[0] = 0xBEEF0005u; sp[1] = h; sp[2] = S + 0x40; sp[3] = 0;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 16))();
            snprintf(detail, sizeof detail, "status 0x%08X target '%.*s'", g_eax,
                     *(uint16_t *)(mem + S + 0x40), (char *)mem + S + 0x200);
            check(g_eax == 0 && !strcmp((char *)mem + S + 0x200,
                                        "\\Device\\Harddisk0\\Partition1\\TDATA\\4d530004"),
                  "query answers the link's own target", detail);

            ANSI(S + 0x20, "\\??\\D:");
            sp[0] = 0xBEEF0006u; sp[1] = S + 0x50; sp[2] = S + 0x30;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 12))();
            h = *(uint32_t *)(mem + S + 0x50);
            sp[0] = 0xBEEF0007u; sp[1] = h; sp[2] = S + 0x40; sp[3] = 0;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 16))();
            check(g_eax == 0 && !strcmp((char *)mem + S + 0x200, "\\Device\\CdRom0"),
                  "\\??\\D: is still the DVD", NULL);

            ANSI(S + 0x20, "\\??\\Q:");
            sp[0] = 0xBEEF0008u; sp[1] = S + 0x50; sp[2] = S + 0x30;
            g_esp = STACK_VA; recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 12))();
            snprintf(detail, sizeof detail, "status 0x%08X", g_eax);
            check(g_eax == 0xC0000034u, "a link nobody made: OBJECT_NAME_NOT_FOUND", detail);
            #undef ANSI
        }
    }

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
