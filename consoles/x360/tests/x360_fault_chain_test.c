/* The X360 fault handler hands a crash on (x360_thread.c on_fault): after its message it calls the handler that was
 * there before it (debuggerd on Android, through the shell's), never _exit(1), which hid the crash from the system and
 * lost the message in the log pipe. Installed twice (x360_run does it once a frame), it still chains to that handler,
 * not to itself. The child process faults; the parent checks it ended through the previous handler.
 *
 * WP8: an HLE that touches an unmapped guest address stops the machine, not the process: KeSetEvent on the pointer
 * Adventure Time passed (3F7FFFFE) gives X360_ERROR and a message naming the export and the address; a second one
 * after it is caught too (the signal is unblocked again); a fault in an HLE called by another HLE names both. Outside
 * an HLE (and on another host thread than the HLE's) a guest fault still reaches the previous handler. */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x360_hle_util.h"

#define PREV_EXIT 42

static void prev_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si; (void)ctx;
    static const char m[] = "previous handler reached\n";
    if (write(2, m, sizeof m - 1) < 0)
        _exit(1);
    _exit(PREV_EXIT);
}

static int child(int with_prev)
{
    if (with_prev) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = prev_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
    }
    x360_install_fault_handler(NULL);
    x360_install_fault_handler(NULL);   /* again, as every x360_run does */
    static volatile uintptr_t bad = 16;   /* read at run time: the compiler cannot see the fault coming */
    return *(volatile int *)bad;
}

static void set_prev(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prev_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL); /* macOS: a PROT_NONE guest page is a SIGBUS */
}

#define BAD_GUEST 0x3F7FFFFEu /* Adventure Time's "event" (report 3.1): unmapped in a bare machine */

/* a guest fault with the machine's handler installed, outside any HLE: the previous handler, as before WP8 */
static int child_guest_outside(int unused)
{
    (void)unused;
    char why[256] = "";
    x360_machine *x = x360_create_bare(why, sizeof why);
    if (!x)
        return 3;
    set_prev();
    x360_install_fault_handler(x);
    return (int)x360_r32(x, BAD_GUEST);
}

static void *idle_thread(void *arg)
{
    pause(); /* until the process ends */
    return arg;
}

/* an HLE marked as running on another host thread (host threads: a worker's lifted code faults while another worker
 * is inside an HLE): not that HLE's fault, the previous handler */
static int child_guest_other_thread(int unused)
{
    (void)unused;
    char why[256] = "";
    x360_machine *x = x360_create_bare(why, sizeof why);
    if (!x)
        return 3;
    set_prev();
    x360_install_fault_handler(x);
    pthread_t other;
    if (pthread_create(&other, NULL, idle_thread, NULL))
        return 4;
    static sigjmp_buf never;
    x->hle_host = other;
    atomic_store_explicit(&x->hle_jmp, (void *)&never, memory_order_release);
    return (int)x360_r32(x, BAD_GUEST);
}

static int run_fn(int (*fn)(int), int arg, int *status)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(fn(arg));
    return waitpid(pid, status, 0) == pid ? 0 : -1;
}

static int run(int with_prev, int *status)
{
    return run_fn(child, with_prev, status);
}

/* in this process: the HLE faults come back as a stopped machine */
static void hle_faults(void)
{
    if (hle_util_start())
        exit(1);
    K(X360_xboxkrnl_KeSetEvent, 3, BAD_GUEST, 1, 0);
    CHECK(X->status == X360_ERROR, "KeSetEvent(%08X): status %d, not X360_ERROR", BAD_GUEST, X->status);
    CHECK(strstr(X->error, "xboxkrnl!KeSetEvent touched the unmapped guest address 3F800002"), "message: %s",
          X->error);
    CHECK(strstr(X->error, "thread ") && strstr(X->error, "LR "), "message without thread / LR: %s", X->error);
    CHECK(!atomic_load(&X->hle_jmp), "the HLE mark is still set after the stop");
    sigset_t m;
    pthread_sigmask(SIG_BLOCK, NULL, &m);
    CHECK(!sigismember(&m, SIGSEGV) && !sigismember(&m, SIGBUS), "SIGSEGV / SIGBUS still blocked after the jump");

    /* again (the signal was unblocked), through an HLE that calls another: both named */
    X->status = X360_RUNNING;
    X->error[0] = 0;
    K(X360_xboxkrnl_VdQueryRealVideoMode, 1, BAD_GUEST);
    CHECK(X->status == X360_ERROR, "VdQueryRealVideoMode(%08X): status %d, not X360_ERROR", BAD_GUEST, X->status);
    CHECK(strstr(X->error, "xboxkrnl!VdQueryVideoMode touched the unmapped guest address") &&
              strstr(X->error, "inside xboxkrnl!VdQueryRealVideoMode"),
          "nested message: %s", X->error);

    /* a good call afterwards runs as usual */
    X->status = X360_RUNNING;
    X->error[0] = 0;
    uint32_t ev = galloc(16);
    x360_w32(X, ev, 0); /* a notification event, not signalled */
    x360_w32(X, ev + 4, 0);
    K(X360_xboxkrnl_KeSetEvent, 3, ev, 1, 0);
    CHECK(X->status == X360_RUNNING && x360_r32(X, ev + 4) == 1, "KeSetEvent on a good event: status %d, state %u",
          X->status, x360_r32(X, ev + 4));
    x360_destroy(X);
}

int main(void)
{
    int fails = 0, st;
    if (run(1, &st) || !WIFEXITED(st) || WEXITSTATUS(st) != PREV_EXIT) {
        fprintf(stderr, "FAIL: with a previous handler the fault did not reach it (status 0x%x)\n", st);
        fails++;
    }
    if (run(0, &st) || !WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV) {
        fprintf(stderr, "FAIL: without one the process did not die of SIGSEGV (status 0x%x)\n", st);
        fails++;
    }
    if (run_fn(child_guest_outside, 0, &st) || !WIFEXITED(st) || WEXITSTATUS(st) != PREV_EXIT) {
        fprintf(stderr, "FAIL: a guest fault outside an HLE did not reach the previous handler (status 0x%x)\n", st);
        fails++;
    }
    if (run_fn(child_guest_other_thread, 0, &st) || !WIFEXITED(st) || WEXITSTATUS(st) != PREV_EXIT) {
        fprintf(stderr, "FAIL: a guest fault on another host thread than the HLE's did not reach the previous handler "
                        "(status 0x%x)\n", st);
        fails++;
    }
    hle_faults();
    fails += failures;
    printf("%s\n", fails ? "x360_fault_chain: FAILED" : "x360_fault_chain: ok");
    return fails != 0;
}
