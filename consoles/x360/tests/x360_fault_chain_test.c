/* The X360 fault handler hands a crash on (x360_thread.c on_fault): after its message it calls the handler that was
 * there before it (debuggerd on Android, through the shell's), never _exit(1), which hid the crash from the system and
 * lost the message in the log pipe. Installed twice (x360_run does it once a frame), it still chains to that handler,
 * not to itself. The child process faults; the parent checks it ended through the previous handler. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "x360_internal.h"

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

static int run(int with_prev, int *status)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(child(with_prev));
    return waitpid(pid, status, 0) == pid ? 0 : -1;
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
    printf("%s\n", fails ? "x360_fault_chain: FAILED" : "x360_fault_chain: ok");
    return fails != 0;
}
