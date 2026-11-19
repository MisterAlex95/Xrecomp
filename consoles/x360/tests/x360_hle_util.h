/* x360_hle_util.h -- what the HLE test programs share (XBA-296): a bare machine, an export called with registers set
 * as a title's call sets them (arguments 9 and up in the caller's stack slots), CHECK. */
#ifndef X360_HLE_UTIL_H
#define X360_HLE_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"

static int failures;
static x360_machine *X;
static uint32_t scratch, stack_top;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static inline xe_cpu *cpu(void) { return &x360_cur(X)->cpu; }

static inline uint64_t callv(int lib, unsigned ord, int n, va_list ap)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    for (int k = 0; k < n; k++) {
        uint32_t v = va_arg(ap, uint32_t);
        if (k < 8)
            c->gpr[3 + k] = v;
        else
            x360_w64(X, stack_top + 0x50 + 8 * (uint32_t)(k - 8), v);
    }
    x360_thunk th = {0, (uint16_t)ord, (uint8_t)lib};
    X->m.slice = 1000;
    x360_hle_call(X, &th);
    return c->gpr[3];
}

/* an xboxkrnl / xam export by name: K(X360_xboxkrnl_Foo, 2, a, b), XAM(X360_xam_Foo, 1, a) */
static inline uint32_t K(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = (uint32_t)callv(X360_LIB_XBOXKRNL, ord, n, ap);
    va_end(ap);
    return r;
}

static inline uint32_t XAM(unsigned ord, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    uint32_t r = (uint32_t)callv(X360_LIB_XAM, ord, n, ap);
    va_end(ap);
    return r;
}

static inline uint32_t galloc(uint32_t size) { return x360_kalloc(X, size); }

static inline uint32_t gstr(const char *s)
{
    uint32_t a = galloc((uint32_t)strlen(s) + 1);
    memcpy(x360_p(X, a), s, strlen(s) + 1);
    return a;
}

/* a guest UTF-16 (big-endian) string from ASCII */
static inline uint32_t gwstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s), a = galloc(2 * n + 2);
    for (uint32_t k = 0; k <= n; k++)
        x360_w16(X, a + 2 * k, (uint8_t)s[k]);
    return a;
}

static inline int hle_util_start(void)
{
    char why[256] = "";
    X = x360_create_bare(why, sizeof why);
    if (!X) {
        printf("cannot create the machine: %s\n", why);
        return -1;
    }
    X->dbg = stdout;
    X->log = stdout;
    x360_install_fault_handler(X);
    scratch = x360_heap_alloc(X, &X->heap_v4k, 0x10000, 0);
    stack_top = scratch + 0x8000;
    return 0;
}

static inline int hle_util_end(const char *what)
{
    x360_destroy(X);
    printf("%s: %s (%d failure%s)\n", what, failures ? "FAILED" : "all checks passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#endif
