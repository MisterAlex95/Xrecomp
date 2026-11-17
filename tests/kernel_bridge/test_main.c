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
 *
 * Ordinals 95 and 96, KeBugCheck and KeBugCheckEx, stop the title: a forked
 * child calls each through the dispatcher and must never get control back
 * (it returned, 2.9 M times in 40 s, before -- Yu-Gi-Oh's abort()). The
 * child ends with the bug-check code as its status (1 for KeBugCheck(0)),
 * after one [BUGCHECK] line naming the code, the parameters and the guest
 * return address, the exit handler getting the reason, and an exception
 * entry in the discovery file. POSIX only: it needs fork.
 */
/* kernel.h brings the NT type vocabulary: <windows.h> on Windows, the
 * shim in platform/xbox_winnt.h elsewhere. Nothing below is
 * platform-specific. */
#include "kernel.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
/* KeBugCheck(Ex) from a forked child, through the dispatcher for the thunk
 * entry `entry` (looked up there: a dispatcher serves the slot looked up
 * last), with the guest stack laid out at stack_va: the child's exit status (-1 on a signal),
 * what it printed to stderr in err, what its exit handler got in why. */
static int bugcheck_exit_fd = -1;

static void bugcheck_exit_handler(const char *why)
{
    if (bugcheck_exit_fd >= 0 && write(bugcheck_exit_fd, why, strlen(why)) < 0)
        bugcheck_exit_fd = -1;
}

static void read_all(int fd, char *out, size_t cap)
{
    size_t used = 0;
    ssize_t n;
    char sink[256];

    /* To EOF: closing early would kill the child on SIGPIPE. */
    while ((n = read(fd, used < cap - 1 ? out + used : sink,
                     used < cap - 1 ? cap - 1 - used : sizeof sink)) > 0)
        if (used < cap - 1)
            used += (size_t)n;
    out[used] = 0;
}

static int bugcheck_child(uint32_t entry, uint8_t *mem, uint32_t stack_va,
                          const uint32_t *args, int nargs,
                          char *err, size_t err_cap, char *why, size_t why_cap)
{
    int efd[2], wfd[2], status = 0;
    pid_t pid;

    if (pipe(efd) != 0 || pipe(wfd) != 0)
        return -2;
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        uint32_t *sp = (uint32_t *)(mem + stack_va);
        int i;

        dup2(efd[1], 2);
        close(efd[0]);
        close(wfd[0]);
        bugcheck_exit_fd = wfd[1];
        xbox_SetExitHandler(bugcheck_exit_handler);
        sp[0] = 0x00012345u;          /* the guest return address */
        for (i = 0; i < nargs; i++)
            sp[1 + i] = args[i];
        g_esp = stack_va;
        recomp_lookup_kernel(entry)();
        /* Guest code after the call: it must never run. */
        fprintf(stderr, "RETURNED TO THE GUEST\n");
        _exit(99);
    }
    close(efd[1]);
    close(wfd[1]);
    read_all(efd[0], err, err_cap);
    read_all(wfd[0], why, why_cap);
    close(efd[0]);
    close(wfd[0]);
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

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
    *(uint32_t *)(mem + THUNK_VA + 20) = 0x80000000u | 95u;   /* KeBugCheck */
    *(uint32_t *)(mem + THUNK_VA + 24) = 0x80000000u | 96u;   /* KeBugCheckEx */
    *(uint32_t *)(mem + THUNK_VA + 28) = 0x80000000u | 104u;  /* KeGetCurrentThread */
    *(uint32_t *)(mem + THUNK_VA + 32) = 0x80000000u | 246u;  /* ObReferenceObjectByHandle */
    *(uint32_t *)(mem + THUNK_VA + 36) = 0x80000000u | 187u;  /* NtClose */
    *(uint32_t *)(mem + THUNK_VA + 40) = 0x80000000u | 189u;  /* NtCreateEvent */
    *(uint32_t *)(mem + THUNK_VA - 4) = 0x80000000u | 173u;
    *(uint32_t *)(mem + THUNK_VA - 8) = 0xF2345678u;
    *(uint32_t *)(mem + THUNK_VA - 12) = 0x80000000u;
    *(uint32_t *)(mem + THUNK_VA - 16) = 0x80000000u | 379u;
    xbox_kernel_set_thunk_address(THUNK_VA, 11);
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

    /* NtDuplicateObject(NtCurrentThread()) from the main thread. The host
     * may give the thread a real handle (the duplicate is then a fresh token
     * that leads to the caller's own KTHREAD) or hand back the -2
     * pseudo-handle itself, which is no duplicate: the call must then fail
     * with no token allocated. It once landed -2 in the handle table, which
     * made every later save-state capture refuse, and attached no KTHREAD to
     * the token on either host. Either way the thread stays usable after
     * NtClose, and a second duplicate is a token of its own. */
    {
        const uint32_t ENTRY_DUP = *(uint32_t *)(mem + THUNK_VA + 4);
        const uint32_t ENTRY_KT = *(uint32_t *)(mem + THUNK_VA + 28);
        const uint32_t ENTRY_REF = *(uint32_t *)(mem + THUNK_VA + 32);
        const uint32_t ENTRY_CLOSE = *(uint32_t *)(mem + THUNK_VA + 36);
        const uint32_t ENTRY_EVENT = *(uint32_t *)(mem + THUNK_VA + 40);
        const uint32_t OUT_VA = 0x31000, OA_VA = 0x31100;
        const uint32_t TIB_VA = 0x32000, KTHREAD_VA = 0x33000;
        uint32_t fs_saved = g_fs_base;
        uint32_t *sp = (uint32_t *)(mem + STACK_VA);
        uint32_t *out = (uint32_t *)(mem + OUT_VA);
        uint32_t kthread, dup1, dup2, st1, st2, probe1, probe2;
        char detail[160];

        #define CALL(entry) do { g_esp = STACK_VA; g_eax = 0xDEADBEEFu; \
            recomp_lookup_kernel(entry)(); } while (0)
        #define DUP(status, token) do { sp[0] = 0xBEEF0003u; sp[1] = 0xFFFFFFFEu; \
            sp[2] = OUT_VA; sp[3] = 0x2u; out[0] = 0; CALL(ENTRY_DUP); \
            (status) = g_eax; (token) = out[0]; } while (0)
        #define CLOSE(token) do { sp[0] = 0xBEEF0004u; sp[1] = (token); \
            CALL(ENTRY_CLOSE); } while (0)
        /* ObReferenceObjectByHandle(token, NULL, &object): the KTHREAD it
         * reaches, 0 when none (status in g_eax). */
        #define REF(token, obj) do { sp[0] = 0xBEEF0005u; sp[1] = (token); \
            sp[2] = 0; sp[3] = OUT_VA + 4; out[1] = 0; CALL(ENTRY_REF); \
            (obj) = g_eax == 0 ? out[1] : 0; } while (0)
        /* NtCreateEvent with an unnamed OBJECT_ATTRIBUTES: the next table
         * token, closed again, so the lowest free slot can be compared. */
        #define PROBE(token) do { memset(mem + OA_VA, 0, 16); \
            sp[0] = 0xBEEF0006u; sp[1] = OUT_VA + 8; sp[2] = OA_VA; sp[3] = 0; \
            sp[4] = 0; out[2] = 0; CALL(ENTRY_EVENT); (token) = out[2]; \
            CLOSE(token); } while (0)

        /* The thread's KTHREAD, as a TIB names it: the stand-in has no
         * contiguous window to build a pool object in. */
        memset(mem + KTHREAD_VA, 0, 0x200);
        memset(mem + TIB_VA, 0, 0x100);
        *(uint32_t *)(mem + TIB_VA + 0x28) = KTHREAD_VA;
        g_fs_base = TIB_VA;
        CALL(ENTRY_KT);
        kthread = g_eax;
        snprintf(detail, sizeof detail, "0x%08X", kthread);
        check(kthread == KTHREAD_VA, "the main thread's KTHREAD is its TIB's",
              detail);

        PROBE(probe1);
        DUP(st1, dup1);
        PROBE(probe2);
        snprintf(detail, sizeof detail, "status 0x%08X, token 0x%08X",
                 st1, dup1);
        check((st1 == 0 && dup1 != 0 && dup1 != 0xFFFFFFFEu
               && dup1 != 0xFFFFFFFFu) || (st1 == 0xC0000001u && dup1 == 0),
              "NtDuplicateObject(NtCurrentThread()): a token or a clean failure",
              detail);
        if (st1 == 0) {
            uint32_t obj;
            REF(dup1, obj);
            snprintf(detail, sizeof detail, "object 0x%08X, KTHREAD 0x%08X",
                     obj, kthread);
            check(obj == kthread,
                  "the duplicate leads to the caller's own KTHREAD", detail);
            check(probe2 != probe1 && probe1 != 0,
                  "the duplicate holds a handle-table slot", NULL);
        } else {
            snprintf(detail, sizeof detail, "slot 0x%08X before, 0x%08X after",
                     probe1, probe2);
            check(probe1 != 0 && probe2 == probe1,
                  "a failed duplicate allocates no handle-table slot", detail);
        }

        DUP(st2, dup2);
        check(st2 == st1, "a second duplicate behaves as the first", NULL);
        if (st1 == 0 && st2 == 0) {
            uint32_t obj;
            snprintf(detail, sizeof detail, "0x%08X twice", dup1);
            check(dup2 != dup1, "a second duplicate is a token of its own",
                  detail);
            REF(dup2, obj);
            check(obj == kthread,
                  "the second duplicate leads to the same KTHREAD", NULL);
            CLOSE(dup1);
            REF(dup1, obj);
            check(obj == 0, "a closed duplicate leads nowhere", NULL);
            REF(dup2, obj);
            check(obj == kthread,
                  "closing one duplicate leaves the other's thread", NULL);
            CLOSE(dup2);
        } else {
            check(dup2 == 0, "a second failed duplicate allocates nothing",
                  NULL);
        }
        {
            uint32_t obj;
            CALL(ENTRY_KT);
            check(g_eax == kthread,
                  "the thread keeps its KTHREAD after NtClose", NULL);
            REF(0xFFFFFFFEu, obj);
            check(obj == kthread,
                  "NtCurrentThread() still resolves after NtClose", NULL);
        }
        g_fs_base = fs_saved;
        #undef CALL
        #undef DUP
        #undef CLOSE
        #undef REF
        #undef PROBE
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

#ifndef _WIN32
    /* Ordinals 95 / 96: the title asking the kernel to die never gets
     * control back, and the run ends with the code, the parameters and the
     * call site named. */
    {
        recomp_func_t bc = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 20));
        recomp_func_t bcx = recomp_lookup_kernel(*(uint32_t *)(mem + THUNK_VA + 24));
        char err[4096], why[512], disc_path[512], text[1024] = "", detail[600];
        const char *tmp = getenv("TMPDIR");
        FILE *f;
        int rc;

        check(bc && bcx, "ordinals 95, 96 resolve", NULL);
        snprintf(disc_path, sizeof disc_path, "%s/kernel_bridge_bugcheck_%d.jsonl",
                 tmp && *tmp ? tmp : "/tmp", (int)getpid());
        remove(disc_path);
        setenv("RECOMP_DISCOVERY", disc_path, 1);
        if (bc) {
            /* abort() in the title's CRT: KeBugCheck(0). */
            const uint32_t args[1] = { 0 };
            rc = bugcheck_child(*(uint32_t *)(mem + THUNK_VA + 20), mem, STACK_VA, args, 1, err, sizeof err,
                                why, sizeof why);
            snprintf(detail, sizeof detail, "status %d, stderr: %.400s", rc, err);
            check(rc == 1 && !strstr(err, "RETURNED TO THE GUEST"),
                  "KeBugCheck(0) never returns, ends with status 1", detail);
            check(strstr(err, "[BUGCHECK] KeBugCheck(0x00000000) called from"
                              " 0x00012345") != NULL,
                  "one [BUGCHECK] line names the code and the caller", detail);
            check(strstr(err, "[EXIT] KeBugCheck 0x00000000") != NULL,
                  "and the [EXIT] line the compat runner reads", detail);
            check(strstr(why, "bug check 0x00000000 at 0x00012345") != NULL,
                  "the exit handler gets the reason", why);
        }
        if (bcx) {
            const uint32_t args[5] = { 0xDEADu, 0x11111111u, 0x22222222u,
                                       0x33333333u, 0x44444444u };
            rc = bugcheck_child(*(uint32_t *)(mem + THUNK_VA + 24), mem, STACK_VA, args, 5, err, sizeof err,
                                why, sizeof why);
            snprintf(detail, sizeof detail, "status %d, stderr: %.400s", rc, err);
            check(rc == 0xAD && !strstr(err, "RETURNED TO THE GUEST"),
                  "KeBugCheckEx never returns, status is the code", detail);
            check(strstr(err, "[BUGCHECK] KeBugCheckEx(0x0000DEAD, 0x11111111,"
                              " 0x22222222, 0x33333333, 0x44444444) called from"
                              " 0x00012345") != NULL,
                  "its line names the four parameters too", detail);
        }
        unsetenv("RECOMP_DISCOVERY");
        f = fopen(disc_path, "rb");
        if (f) {
            size_t n = fread(text, 1, sizeof text - 1, f);
            text[n] = 0;
            fclose(f);
        }
        remove(disc_path);
        check(strstr(text, "{\"t\":\"exception\",\"va\":\"0x00012345\","
                           "\"code\":\"0x00000000\",\"why\":\"KeBugCheck(") != NULL,
              "the discovery file has the stop as an exception", text);
    }
#endif

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
