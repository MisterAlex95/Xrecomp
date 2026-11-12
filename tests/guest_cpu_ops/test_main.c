/*
 * The runtime side of lifted instructions that call into it.
 *
 *  - recomp_divide_error (the lifted div/idiv's #DE): with no guest
 *    exception dispatch it reports the fault and stops the way an unhandled
 *    kernel-mode exception stops the console, KeBugCheckEx(0x1E, status,
 *    va): STATUS_INTEGER_DIVIDE_BY_ZERO for a zero divisor,
 *    STATUS_INTEGER_OVERFLOW for a quotient that does not fit. Run in a
 *    child process, since it does not return (POSIX only).
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

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#ifndef _WIN32
/* Run recomp_divide_error(va, divisor) in a child; its exit code and the
 * first line it printed to stderr. */
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
#endif

int main(void)
{
#ifndef _WIN32
    test_divide_error();
#else
    printf("recomp_divide_error: POSIX only (fork), skipped\n");
#endif
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
