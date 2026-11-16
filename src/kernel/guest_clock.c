/* guest_clock.c - see guest_clock.h (XBA-240). */
#include "guest_clock.h"

#include "platform/host_time.h"   /* xbox_HostNowNs */

/* ---- Pure ---------------------------------------------------------------- */

void xbox_GuestClockInit(XboxGuestClock *c)
{
    c->host0 = 0;
    c->extra0 = 0;
    c->speed = 1;
}

uint64_t xbox_GuestClockExtraAt(const XboxGuestClock *c, uint64_t host_ns)
{
    if (c->speed <= 1 || host_ns <= c->host0)
        return c->extra0;
    return c->extra0 + (uint64_t)(c->speed - 1) * (host_ns - c->host0);
}

void xbox_GuestClockRetime(XboxGuestClock *c, uint64_t host_ns, uint32_t speed)
{
    if (speed < 1)
        speed = 1;
    if (speed > XBOX_GUEST_CLOCK_MAX_SPEED)
        speed = XBOX_GUEST_CLOCK_MAX_SPEED;
    if (host_ns < c->host0)       /* a reader's late clock: never back in time */
        host_ns = c->host0;
    c->extra0 = xbox_GuestClockExtraAt(c, host_ns);
    c->host0 = host_ns;
    c->speed = speed;
}

uint64_t xbox_GuestClockUnits(uint64_t ns, uint64_t per_second)
{
    /* Whole seconds and the remainder apart: ns * 733333333 (the TSC) would
     * wrap after 25 s. */
    return (ns / 1000000000u) * per_second + (ns % 1000000000u) * per_second / 1000000000u;
}

uint64_t xbox_GuestClockHostSpan(uint64_t span, uint32_t speed)
{
    if (speed <= 1)
        return span;
    return span / speed + (span % speed != 0);
}

uint32_t xbox_GuestClockHostMsAt(uint32_t ms, uint32_t speed)
{
    if (ms == 0xFFFFFFFFu)
        return ms;
    return (uint32_t)xbox_GuestClockHostSpan(ms, speed);
}

int xbox_GuestClockShowNth(uint32_t *n, uint32_t *last_speed, uint32_t speed)
{
    if (speed != *last_speed) {
        *last_speed = speed;
        *n = 0;
    }
    if (speed <= 1)
        return 1;
    return (*n)++ % speed == 0;
}

/* ---- The process's guest clock -------------------------------------------- */

/* A sequence lock: the writer (one at a time, s_writer) makes s_seq odd while
 * it changes the three fields; a reader retries while it is odd or moved.
 * s_active is set before the first change and never cleared (but by tests):
 * while it is 0 the readers return at once. */
static volatile uint32_t s_seq, s_writer, s_active;
static volatile uint64_t s_host0, s_extra0;
static volatile uint32_t s_speed = 1;

static void clock_read(XboxGuestClock *c)
{
    for (;;) {
        uint32_t q = __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
        if (q & 1u)
            continue;
        c->host0 = __atomic_load_n(&s_host0, __ATOMIC_RELAXED);
        c->extra0 = __atomic_load_n(&s_extra0, __ATOMIC_RELAXED);
        c->speed = __atomic_load_n(&s_speed, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_seq, __ATOMIC_RELAXED) == q)
            return;
    }
}

void xbox_GuestClockSetSpeed(uint32_t speed)
{
    XboxGuestClock c;
    uint32_t q;

    if (speed < 1)
        speed = 1;
    if (speed > XBOX_GUEST_CLOCK_MAX_SPEED)
        speed = XBOX_GUEST_CLOCK_MAX_SPEED;
    while (__atomic_exchange_n(&s_writer, 1u, __ATOMIC_ACQUIRE))
        ;
    if (speed == __atomic_load_n(&s_speed, __ATOMIC_RELAXED)) {
        __atomic_store_n(&s_writer, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (speed > 1)
        __atomic_store_n(&s_active, 1u, __ATOMIC_RELEASE);
    c.host0 = s_host0;
    c.extra0 = s_extra0;
    c.speed = s_speed;
    xbox_GuestClockRetime(&c, xbox_HostNowNs(), speed);
    q = __atomic_load_n(&s_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_seq, q + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&s_host0, c.host0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_extra0, c.extra0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_speed, c.speed, __ATOMIC_RELAXED);
    __atomic_store_n(&s_seq, q + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&s_writer, 0u, __ATOMIC_RELEASE);
}

uint32_t xbox_GuestClockSpeed(void)
{
    return __atomic_load_n(&s_speed, __ATOMIC_RELAXED);
}

int xbox_GuestClockActive(void)
{
    return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE) != 0;
}

uint64_t xbox_GuestClockExtraNs(void)
{
    XboxGuestClock c;
    uint64_t now;

    if (!xbox_GuestClockActive())
        return 0;
    /* The clock before the fields: a change made in between has host0 past
     * now, which reads as the change's own extra -- never less than a reader
     * before it saw, so the guest's time does not step back. */
    now = xbox_HostNowNs();
    clock_read(&c);
    return xbox_GuestClockExtraAt(&c, now);
}

uint64_t xbox_GuestClockExtraUnits(uint64_t per_second)
{
    if (!xbox_GuestClockActive())
        return 0;
    return xbox_GuestClockUnits(xbox_GuestClockExtraNs(), per_second);
}

uint64_t xbox_GuestNowNs(void)
{
    uint64_t now = xbox_HostNowNs();   /* before the fields (xbox_GuestClockExtraNs) */
    XboxGuestClock c;

    if (!xbox_GuestClockActive())
        return now;
    clock_read(&c);
    return now + xbox_GuestClockExtraAt(&c, now);
}

uint64_t xbox_GuestClockHostNs(uint64_t guest_ns)
{
    return xbox_GuestClockHostSpan(guest_ns, xbox_GuestClockSpeed());
}

uint32_t xbox_GuestClockHostMs(uint32_t guest_ms)
{
    return xbox_GuestClockHostMsAt(guest_ms, xbox_GuestClockSpeed());
}

int xbox_GuestClockShowFrame(void)
{
    /* One caller, the executor's FLIP_STALL (the pushbuffer thread). */
    static uint32_t n, last_speed = 1;
    return xbox_GuestClockShowNth(&n, &last_speed, xbox_GuestClockSpeed());
}

void xbox_GuestClockReset(void)
{
    while (__atomic_exchange_n(&s_writer, 1u, __ATOMIC_ACQUIRE))
        ;
    __atomic_store_n(&s_seq, __atomic_load_n(&s_seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_host0 = 0;
    s_extra0 = 0;
    s_speed = 1;
    __atomic_store_n(&s_seq, __atomic_load_n(&s_seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_active, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_writer, 0u, __ATOMIC_RELEASE);
}
