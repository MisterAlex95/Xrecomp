/* mmio_fault_chain_posix (XBA-22): the toolkit's SIGSEGV / SIGBUS handler
 * hands faults that are not guest MMIO to the previous handler.
 *
 *   previous: a handler is installed first (debuggerd's place on Android);
 *             after xbox_PosixInstallMmioFaultHandler, a write to a
 *             PROT_NONE page outside the guest must reach it, with the
 *             faulting address.
 *   default:  no previous handler; the process must still die of SIGSEGV
 *             (checked from a child with waitpid), not hang or exit 0.
 *   replaced: another library installs its own handler after ours (SDL_Init
 *             on Android); a second install call puts ours back in front and
 *             the fault still reaches that library's handler. */
#define _GNU_SOURCE
#include "kernel/xbox_memory_layout.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* What a title provides; nothing here calls into guest code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static sigjmp_buf s_jump;
static volatile void *s_seen_addr;
static volatile int s_seen_sig;

static void previous_handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    s_seen_sig = sig;
    s_seen_addr = si->si_addr;
    siglongjmp(s_jump, 1);
}

static char *guard_page(void)
{
    char *p = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int test_previous(void)
{
    struct sigaction sa;
    char *page = guard_page();
    if (!page) { puts("mmap failed"); return 2; }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = previous_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    if (xbox_PosixInstallMmioFaultHandler() != 0) { puts("FAIL: install"); return 1; }
    if (xbox_PosixInstallMmioFaultHandler() != 0) { puts("FAIL: second install"); return 1; }

    if (sigsetjmp(s_jump, 1) == 0) {
        *(volatile char *)(page + 16) = 1;          /* not guest MMIO */
        puts("FAIL: the write did not fault");
        return 1;
    }
    if (s_seen_sig != SIGSEGV && s_seen_sig != SIGBUS) {
        printf("FAIL: previous handler saw signal %d\n", s_seen_sig);
        return 1;
    }
    if (s_seen_addr != page + 16) {
        printf("FAIL: previous handler saw %p, want %p\n", (void *)s_seen_addr, (void *)(page + 16));
        return 1;
    }
    puts("PASS: a fault outside guest MMIO reaches the previous handler with its address");
    return 0;
}

static int test_default(void)
{
    int status = 0;
    pid_t pid = fork();
    if (pid < 0) { puts("fork failed"); return 2; }
    if (pid == 0) {
        char *page = guard_page();
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        alarm(10);                                   /* a hang would be a failure too */
        if (!page || xbox_PosixInstallMmioFaultHandler() != 0) _exit(3);
        *(volatile char *)(page + 16) = 1;
        _exit(0);                                    /* survived: wrong */
    }
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) {
        printf("PASS: with no previous handler the process dies of signal %d\n", WTERMSIG(status));
        return 0;
    }
    printf("FAIL: child status %#x (want death by SIGSEGV/SIGBUS)\n", status);
    return 1;
}

static int test_replaced(void)
{
    struct sigaction sa, cur;
    char *page = guard_page();
    if (!page) { puts("mmap failed"); return 2; }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    if (xbox_PosixInstallMmioFaultHandler() != 0) { puts("FAIL: install"); return 1; }
    memset(&sa, 0, sizeof sa);                       /* the newcomer */
    sa.sa_sigaction = previous_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    if (xbox_PosixInstallMmioFaultHandler() != 0) { puts("FAIL: re-install"); return 1; }
    sigaction(SIGSEGV, NULL, &cur);
    if (cur.sa_sigaction == previous_handler) { puts("FAIL: not back in front"); return 1; }
    if (sigsetjmp(s_jump, 1) == 0) {
        *(volatile char *)(page + 32) = 1;
        puts("FAIL: the write did not fault");
        return 1;
    }
    if (s_seen_addr != page + 32) { printf("FAIL: newcomer saw %p\n", (void *)s_seen_addr); return 1; }
    puts("PASS: back in front after a newcomer, and the fault still reaches the newcomer");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "default") == 0)
        return test_default();
    if (argc > 1 && strcmp(argv[1], "replaced") == 0)
        return test_replaced();
    return test_previous();
}
