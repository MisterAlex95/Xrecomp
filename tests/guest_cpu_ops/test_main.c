/*
 * The runtime side of lifted instructions that call into it.
 *
 *  - recomp_divide_error (the lifted div/idiv's #DE): with no guest
 *    handler chain it reports the fault and stops the way an unhandled
 *    kernel-mode exception stops the console, KeBugCheckEx(0x1E, status,
 *    va): STATUS_INTEGER_DIVIDE_BY_ZERO for a zero divisor,
 *    STATUS_INTEGER_OVERFLOW for a quotient that does not fit. Run in a
 *    child process, since it does not return (POSIX only).
 *  - recomp_cut_body (a lifted body that ran off its end, WP2): one
 *    [CUT] line naming the last instruction, a {"t":"cut_body"} discovery
 *    entry, then it stops like the divide error, KeBugCheckEx(0x1E,
 *    STATUS_ILLEGAL_INSTRUCTION, va), and never returns into the caller.
 *  - recomp_misdecoded (string port I/O in lifted code, XBA-139): one
 *    [MISDECODE] line, an "unimpl" discovery entry, the same stop; with
 *    RECOMP_MISDECODE_TRAP=0 it reports and returns.
 *  - xbox_GuestInterruptFlag (the lifted cli/sti/popfd): a cli on a thread
 *    below DISPATCH_LEVEL masks device interrupts the way a raise does --
 *    counted in xbox_IrqlBlocksInterrupts, and with RECOMP_DISPATCH_GATE
 *    holding the one-CPU gate, so an interrupt posted meanwhile does not
 *    run at a safe point inside the section but at the sti -- without
 *    changing the guest-visible IRQL. Nested and redundant calls count
 *    once; on a thread already at DISPATCH_LEVEL they change nothing;
 *    RECOMP_CLI_GATE=0 turns the masking off.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

void recomp_divide_error(uint32_t va, uint32_t divisor);
void recomp_cut_body(uint32_t va);
void recomp_misdecoded(const char *text, uint32_t va);
void xbox_GuestInterruptFlag(int enabled);
int  xbox_GuestInterruptsMasked(void);

extern ptrdiff_t g_xbox_mem_offset;

static void set_env(const char *name, const char *v)
{
#ifdef _WIN32
    _putenv_s(name, v ? v : "");
#else
    if (v)
        setenv(name, v, 1);
    else
        unsetenv(name);
#endif
    xbox_IrqReloadConfig();
}

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#ifndef _WIN32
/* Run recomp_divide_error(va, divisor) in a child -- or recomp_cut_body(va)
 * when divisor is CUT -- ; its exit code and what it printed to stderr. */
#define CUT 0xFFFFFFFFu
#define MISDECODE 0xFFFFFFFEu   /* recomp_misdecoded("outsb ...", va) */
static int divide_error_child(uint32_t va, uint32_t divisor, char *line,
                              size_t cap)
{
    int fds[2], status = 0;
    pid_t pid;
    ssize_t n;

    if (pipe(fds) != 0)
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        /* No TIB, so no guest handler chain: the unhandled path. (With one,
         * the error is raised to the guest first: tests/seh_dispatch.) */
        g_fs_base = 0;
        if (divisor == CUT)
            recomp_cut_body(va);
        else if (divisor == MISDECODE)
            recomp_misdecoded("outsb dx, byte ptr [esi]", va);
        else
            recomp_divide_error(va, divisor);
        _exit(99);                      /* it must not return */
    }
    close(fds[1]);
    /* Read to EOF: closing early would kill the child on SIGPIPE. */
    {
        size_t used = 0;
        char sink[256];
        while ((n = read(fds[0], used < cap - 1 ? line + used : sink,
                         used < cap - 1 ? cap - 1 - used : sizeof sink)) > 0)
            if (used < cap - 1)
                used += (size_t)n;
        line[used] = 0;
    }
    close(fds[0]);
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_divide_error(void)
{
    char line[1024];
    int rc;

    printf("recomp_divide_error\n");
    rc = divide_error_child(0x00012340u, 0, line, sizeof line);
    check(rc == 0x1E, "a zero divisor stops as KeBugCheckEx(0x1E)");
    check(strstr(line, "[#DE]") && strstr(line, "0x00012340")
          && strstr(line, "divide by zero") && strstr(line, "0xC0000094"),
          "the report names the address, the cause and the status");
    rc = divide_error_child(0x00056780u, 2, line, sizeof line);
    check(rc == 0x1E, "an overflowing quotient stops the same way");
    check(strstr(line, "0x00056780") && strstr(line, "quotient overflow")
          && strstr(line, "0xC0000095"),
          "and is reported as STATUS_INTEGER_OVERFLOW");
}

static void test_cut_body(void)
{
    char line[1024], path[256];
    char text[512] = "";
    FILE *f;
    int rc;

    printf("recomp_cut_body\n");
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/guest_cpu_ops_cut_%d.jsonl",
             tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(path);
    setenv("RECOMP_DISCOVERY", path, 1);
    rc = divide_error_child(0x001C1208u, CUT, line, sizeof line);
    unsetenv("RECOMP_DISCOVERY");
    check(rc == 0x1E, "a cut body stops as KeBugCheckEx(0x1E), never returns");
    check(strstr(line, "[CUT]") && strstr(line, "0x001C1208")
          && !strstr(strstr(line, "[CUT]") + 5, "[CUT]"),
          "one [CUT] line names the last instruction");
    f = fopen(path, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
    }
    remove(path);
    check(strstr(text, "{\"t\":\"cut_body\",\"va\":\"0x001C1208\"}") != NULL,
          "and writes a cut_body discovery entry");
}

static void test_misdecoded(void)
{
    char line[1024], path[256];
    char text[512] = "";
    FILE *f;
    int rc;

    printf("recomp_misdecoded\n");
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/guest_cpu_ops_misdecode_%d.jsonl",
             tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(path);
    setenv("RECOMP_DISCOVERY", path, 1);
    rc = divide_error_child(0x00034560u, MISDECODE, line, sizeof line);
    unsetenv("RECOMP_DISCOVERY");
    check(rc == 0x1E, "string port I/O stops as KeBugCheckEx(0x1E)");
    check(strstr(line, "[MISDECODE]") && strstr(line, "0x00034560")
          && strstr(line, "outsb"), "one [MISDECODE] line names the site");
    f = fopen(path, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
    }
    remove(path);
    check(strstr(text, "\"t\":\"unimpl\"") && strstr(text, "0x00034560"),
          "and writes an unimpl discovery entry");
    setenv("RECOMP_MISDECODE_TRAP", "0", 1);
    rc = divide_error_child(0x00034560u, MISDECODE, line, sizeof line);
    unsetenv("RECOMP_MISDECODE_TRAP");
    check(rc == 99 && strstr(line, "[MISDECODE]") != NULL,
          "RECOMP_MISDECODE_TRAP=0 reports it and returns");
}
#endif

static volatile LONG s_runs;
static unsigned long s_run_tid;

static void apu_routine(void)
{
    InterlockedIncrement(&s_runs);
    s_run_tid = (unsigned long)GetCurrentThreadId();
}

static volatile LONG s_post_result;

static DWORD WINAPI poster(LPVOID p)
{
    (void)p;
    InterlockedExchange(&s_post_result, xbox_IrqPost(XBOX_IRQ_APU));
    return 0;
}

static int post_from_other_thread(void)
{
    HANDLE t = CreateThread(NULL, 0, poster, NULL, 0, NULL);
    WaitForSingleObject(t, 10000);
    CloseHandle(t);
    return (int)s_post_result;
}

static void test_interrupt_flag(void)
{
    unsigned long me = (unsigned long)GetCurrentThreadId();
    KIRQL old;

    printf("xbox_GuestInterruptFlag, gate off\n");
    check(xbox_IrqlRaisedCount() == 0 && !xbox_GuestInterruptsMasked(),
          "starts unmasked");
    xbox_GuestInterruptFlag(0);
    check(xbox_IrqlBlocksInterrupts() && xbox_IrqlRaisedCount() == 1,
          "cli at PASSIVE_LEVEL holds device interrupts off");
    check(xbox_IrqlCurrent() == PASSIVE_LEVEL && xbox_GuestInterruptsMasked(),
          "and leaves the guest-visible IRQL alone");
    xbox_GuestInterruptFlag(0);
    check(xbox_IrqlRaisedCount() == 1, "a second cli counts nothing more");
    xbox_GuestInterruptFlag(1);
    check(xbox_IrqlRaisedCount() == 0 && !xbox_GuestInterruptsMasked(),
          "sti lets them in again");
    xbox_GuestInterruptFlag(1);
    check(xbox_IrqlRaisedCount() == 0, "an sti without a cli takes nothing off");

    old = xbox_KfRaiseIrql(DISPATCH_LEVEL);
    xbox_GuestInterruptFlag(0);
    check(xbox_IrqlRaisedCount() == 1, "cli at DISPATCH_LEVEL: already masked");
    xbox_GuestInterruptFlag(1);
    check(xbox_IrqlRaisedCount() == 1, "and its sti does not lower anything");
    xbox_KfLowerIrql(old);
    check(xbox_IrqlRaisedCount() == 0, "the lower still balances");

    set_env("RECOMP_CLI_GATE", "0");
    xbox_GuestInterruptFlag(0);
    check(xbox_IrqlRaisedCount() == 0 && xbox_GuestInterruptsMasked(),
          "RECOMP_CLI_GATE=0: IF only, nothing masked");
    xbox_GuestInterruptFlag(1);
    set_env("RECOMP_CLI_GATE", NULL);

    printf("xbox_GuestInterruptFlag, RECOMP_DISPATCH_GATE=1\n");
    set_env("RECOMP_DISPATCH_GATE", "1");
    xbox_GuestInterruptFlag(0);
    check(xbox_DispatchGateOwner() == me, "cli takes the one-CPU gate");
    check(post_from_other_thread() == XBOX_IRQ_POSTED && s_runs == 0,
          "an interrupt meanwhile is posted, not run beside the section");
    xbox_IrqSafePoint();
    check(s_runs == 0, "a safe point inside the section does not run it");
    xbox_GuestInterruptFlag(1);
    check(s_runs == 1 && s_run_tid == me,
          "sti runs it, on this thread, as the CPU takes it after sti");
    check(xbox_DispatchGateOwner() == 0 && xbox_IrqlRaisedCount() == 0,
          "and gives the gate back");
    set_env("RECOMP_DISPATCH_GATE", NULL);
}

int main(void)
{
#ifndef _WIN32
    test_divide_error();
    test_cut_body();
    test_misdecoded();
#else
    printf("recomp_divide_error: POSIX only (fork), skipped\n");
#endif
    /* Guest memory only where the worker stacks are: a posted routine runs
     * on one (as in tests/irq_safe_points). */
    g_fs_base = 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)calloc(
        XBOX_WORKER_STACK_COUNT, XBOX_WORKER_STACK_SIZE)
        - (ptrdiff_t)XBOX_WORKER_STACK_BASE;
    xbox_IrqSetHandler(XBOX_IRQ_APU, apu_routine);
    test_interrupt_flag();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
