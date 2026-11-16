/* xbox_cheats.c -- see xbox_cheats.h (XBA-241). */
#include "xbox_cheats.h"

#if defined(_WIN32)

void xbox_CheatsVblank(void) {}
int xbox_CheatsGuestOk(uint32_t va, uint32_t size, int write)
{
    (void)va; (void)size; (void)write;
    return 0;
}
int xbox_CheatsLoadText(const char *text)
{
    (void)text;
    return 0;
}
size_t xbox_CheatsApply(void) { return 0; }
void xbox_CheatsReset(void) {}
unsigned xbox_CheatsRefused(void) { return 0; }

#else

#include "recomp_cheats.h"
#include "recomp_smc.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void *xbox_GetMemoryBase(void);
extern ptrdiff_t xbox_GetMemoryOffset(void);
extern size_t xbox_GetMappedSize(void);
extern int xbox_ReserveHolds(uint32_t va, uint32_t bytes);

#define LOW_LIMIT   0x00010000u   /* the XBE base: below it, the kernel's own structures */
#define POLL_EVERY  30u           /* vblanks between two looks at the file */
#define LOG_REFUSED 4u

static recomp_cheats s_cheats;
static int s_init;                /* s_cheats initialised */
static int s_env_done;            /* RECOMP_CHEATS_FILE looked at */
static unsigned s_ticks;
static volatile unsigned s_refused;
static int s_pipe[2] = {-1, -1}; /* guest copies through the kernel: EFAULT, not SIGSEGV */

/* A title may have made a RAM page read-only or no-access (NtProtectVirtualMemory): copying through a pipe makes
 * the kernel do the access, so such a page fails the copy instead of faulting the timer thread. memcpy when no
 * pipe could be made. */
static int pipe_ready(void)
{
    if (s_pipe[0] >= 0)
        return 1;
    int fd[2];
    if (pipe(fd) != 0)
        return 0;
    for (int i = 0; i < 2; i++) {
        fcntl(fd[i], F_SETFL, fcntl(fd[i], F_GETFL) | O_NONBLOCK);
        fcntl(fd[i], F_SETFD, FD_CLOEXEC);
    }
    s_pipe[0] = fd[0];
    s_pipe[1] = fd[1];
    return 1;
}

static void pipe_drain(void)
{
    char junk[16];
    while (read(s_pipe[0], junk, sizeof junk) > 0) {
    }
}

static int safe_copy(void *dst, const void *src, size_t n)
{
    if (!pipe_ready()) {
        memcpy(dst, src, n);
        return 1;
    }
    if (write(s_pipe[1], src, n) != (ssize_t)n) {
        pipe_drain();
        return 0;
    }
    if (read(s_pipe[0], dst, n) != (ssize_t)n) {
        pipe_drain();
        return 0;
    }
    return 1;
}

static int in_window(uint32_t va, uint32_t size)
{
    return xbox_GetMemoryBase() != NULL && size > 0
           && ((va >= LOW_LIMIT && (uint64_t)va + size <= (uint64_t)xbox_GetMappedSize())
               || xbox_ReserveHolds(va, size));
}

static void refused(uint32_t va, uint32_t size, int write)
{
    unsigned n = __atomic_add_fetch(&s_refused, 1, __ATOMIC_RELAXED);
    if (n <= LOG_REFUSED)
        fprintf(stderr, "xbox: cheats: %s of %u byte(s) at 0x%08X refused (not mapped guest RAM%s)%s\n",
                write ? "write" : "read", size, va, write ? ", or code" : "",
                n == LOG_REFUSED ? "; further refusals not logged" : "");
}

int xbox_CheatsGuestOk(uint32_t va, uint32_t size, int write)
{
    int ok = in_window(va, size) && !(write && xbox_SmcInCode(va, size));
    if (!ok)
        refused(va, size, write);
    return ok;
}

static int bus_ok(void *user, uint32_t addr, int size, int write)
{
    (void)user;
    if (!xbox_CheatsGuestOk(addr, (uint32_t)size, write))
        return 0;
    if (!write) { /* a conditional: the page must also be readable */
        uint32_t v;
        if (!safe_copy(&v, (const uint8_t *)xbox_GetMemoryOffset() + addr, (size_t)size)) {
            refused(addr, (uint32_t)size, 0);
            return 0;
        }
    }
    return 1;
}

static uint32_t bus_read(void *user, uint32_t addr, int size)
{
    (void)user;
    uint32_t v = 0; /* an unreadable address reads 0 */
    if (in_window(addr, (uint32_t)size)
        && !safe_copy(&v, (const uint8_t *)xbox_GetMemoryOffset() + addr, (size_t)size))
        v = 0;
    return v; /* little-endian host and guest */
}

static void bus_write(void *user, uint32_t addr, uint32_t v, int size)
{
    (void)user;
    if (!xbox_CheatsGuestOk(addr, (uint32_t)size, 1))
        return;
    if (!safe_copy((uint8_t *)xbox_GetMemoryOffset() + addr, &v, (size_t)size))
        refused(addr, (uint32_t)size, 1); /* a page the title protected */
}

static void ensure_init(void)
{
    if (!s_init) {
        recomp_cheats_init(&s_cheats, RECOMP_CHEAT_XBOX);
        s_init = 1;
    }
}

static size_t apply_now(void)
{
    recomp_cheat_bus bus = {NULL, bus_read, bus_write, NULL, NULL, 0, NULL, 0, bus_ok};
    recomp_cheats_apply(&s_cheats, &bus);
    return recomp_cheats_active(&s_cheats);
}

void xbox_CheatsVblank(void)
{
    ensure_init();
    if (!s_env_done) {
        s_env_done = 1;
        recomp_cheats_load_env(&s_cheats, "xbox");
    } else if (++s_ticks % POLL_EVERY == 0) {
        recomp_cheats_poll(&s_cheats);
    }
    if (s_cheats.n && recomp_cheats_active(&s_cheats))
        apply_now();
}

int xbox_CheatsLoadText(const char *text)
{
    ensure_init();
    s_env_done = 1; /* the text replaces whatever the file had */
    return recomp_cheats_parse_text(&s_cheats, text);
}

size_t xbox_CheatsApply(void)
{
    ensure_init();
    return apply_now();
}

void xbox_CheatsReset(void)
{
    if (s_init)
        recomp_cheats_free(&s_cheats);
    s_init = 0;
    s_env_done = 0;
    s_ticks = 0;
}

unsigned xbox_CheatsRefused(void)
{
    return __atomic_load_n(&s_refused, __ATOMIC_RELAXED);
}

#endif
