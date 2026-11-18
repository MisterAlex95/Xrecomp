/* x360_rtl.c -- the Rtl string family, the wide and counting printf family, image and stack helpers (XBA-296).
 *
 * The printf family runs on one formatter with no limit on its output: a sink counts every character and writes only
 * those within its capacity, so _vscprintf and the vsprintf that follows it agree on any length. The console's
 * conventions: arguments in 64-bit slots (r3..r10, then the caller's stack), variadic doubles in GPRs, a va_list in
 * guest memory as 8-byte slots, guest wide strings UTF-16 big-endian. Conversions follow the Microsoft CRT: %s means
 * the format's own width and %S the other one, h / l / w force narrow / wide, I64 and ll are 64 bits, and l is 32.
 * A wide character on narrow output is its low byte below 256 and '?' above (the runtime's ANSI is Latin-1, as in
 * RtlUnicodeToMultiByteN). Nothing here depends on the host's locale. */
#include <math.h>
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

/* ---- the formatter ---- */

#define FMT_MAX_FIELD 0x7FFFFFFFll
#define FMT_MAX_FLOAT_PRECISION 4096 /* a double has no significant digit past 1074 decimals */

enum { SZ_NONE, SZ_HH, SZ_H, SZ_L, SZ_LL, SZ_W };

typedef struct fmt_sink {
    x360_machine *x;
    uint32_t buf;    /* guest buffer, 0: only count */
    uint64_t cap, n; /* characters that may be written, characters produced */
    int wide;        /* UTF-16 big-endian output */
} fmt_sink;

typedef struct fmt_args {
    x360_machine *x;
    xe_cpu *c;
    int gpr; /* the next register, then the caller's stack */
    int use_va;
    uint32_t va; /* a va_list in guest memory */
} fmt_args;

typedef struct fmt_spec {
    int left, plus, space, alt, zero, size;
    int64_t width;
    int64_t prec; /* -1: none */
} fmt_spec;

static uint64_t arg_next(fmt_args *g)
{
    if (g->use_va) {
        uint64_t v = x360_r64(g->x, g->va);
        g->va += 8;
        return v;
    }
    if (g->gpr <= 10)
        return g->c->gpr[g->gpr++];
    return x360_stack_arg(g->x, g->c, g->gpr++ - 3);
}

static void sink_put(fmt_sink *s, uint32_t ch)
{
    if (s->buf && s->n < s->cap) {
        if (s->wide)
            x360_w16(s->x, s->buf + 2 * (uint32_t)s->n, ch);
        else
            x360_w8(s->x, s->buf + (uint32_t)s->n, ch < 256 ? ch : '?');
    }
    s->n++;
}

/* characters past the capacity are only counted, however many a width asks for */
static uint64_t sink_room(const fmt_sink *s)
{
    return s->buf && s->n < s->cap ? s->cap - s->n : 0;
}

static void sink_fill(fmt_sink *s, uint32_t ch, uint64_t count)
{
    uint64_t room = sink_room(s), w = count < room ? count : room;
    for (uint64_t k = 0; k < w; k++)
        sink_put(s, ch);
    s->n += count - w;
}

static void sink_text(fmt_sink *s, const char *p, size_t n)
{
    for (size_t k = 0; k < n; k++)
        sink_put(s, (uint8_t)p[k]);
}

/* sign or radix prefix, padding, the precision's zeros, the digits */
static void emit_number(fmt_sink *s, const fmt_spec *f, const char *prefix, size_t plen, const char *body, size_t blen,
                        uint64_t zeros, int zero_pad)
{
    uint64_t total = plen + zeros + blen;
    uint64_t pad = (uint64_t)f->width > total ? (uint64_t)f->width - total : 0;
    if (!f->left && !zero_pad)
        sink_fill(s, ' ', pad);
    sink_text(s, prefix, plen);
    if (!f->left && zero_pad)
        sink_fill(s, '0', pad);
    sink_fill(s, '0', zeros);
    sink_text(s, body, blen);
    if (f->left)
        sink_fill(s, ' ', pad);
}

static void emit_int(fmt_sink *s, const fmt_spec *f, uint64_t v, int is_signed, int neg, unsigned base, int upper)
{
    char tmp[24], prefix[3] = {0, 0, 0};
    size_t n = 0, plen = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (uint64_t u = v; u; u /= base)
        tmp[sizeof tmp - ++n] = dig[u % base];
    if (!v && f->prec != 0)
        tmp[sizeof tmp - ++n] = '0';
    uint64_t zeros = f->prec > (int64_t)n ? (uint64_t)f->prec - n : 0;
    if (is_signed && neg)
        prefix[plen++] = '-';
    else if (is_signed && f->plus)
        prefix[plen++] = '+';
    else if (is_signed && f->space)
        prefix[plen++] = ' ';
    if (f->alt && base == 16 && v) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    }
    if (f->alt && base == 8 && !zeros && (!n || tmp[sizeof tmp - n] != '0'))
        zeros = 1;
    emit_number(s, f, prefix, plen, tmp + sizeof tmp - n, n, zeros, f->zero && !f->left && f->prec < 0);
}

static void emit_float(fmt_sink *s, const fmt_spec *f, char conv, uint64_t bits)
{
    double d;
    memcpy(&d, &bits, 8);
    char spec[12], num[FMT_MAX_FLOAT_PRECISION + 416];
    size_t k = 0;
    spec[k++] = '%';
    if (f->plus)
        spec[k++] = '+';
    if (f->space)
        spec[k++] = ' ';
    if (f->alt)
        spec[k++] = '#';
    if (f->prec >= 0) {
        spec[k++] = '.';
        spec[k++] = '*';
    }
    spec[k++] = conv;
    spec[k] = 0;
    int prec = f->prec > FMT_MAX_FLOAT_PRECISION ? FMT_MAX_FLOAT_PRECISION : (int)f->prec;
    int r = f->prec >= 0 ? snprintf(num, sizeof num, spec, prec, d) : snprintf(num, sizeof num, spec, d);
    size_t n = r < 0 ? 0 : (size_t)r >= sizeof num ? sizeof num - 1 : (size_t)r;
    for (size_t i = 0; i < n; i++)
        if (num[i] == ',') /* a host locale's decimal point */
            num[i] = '.';
    size_t plen = n && (num[0] == '-' || num[0] == '+' || num[0] == ' ');
    if ((conv == 'a' || conv == 'A') && n >= plen + 2 && num[plen] == '0')
        plen += 2;
    emit_number(s, f, num, plen, num + plen, n - plen, 0, f->zero && !f->left && isfinite(d));
}

static uint32_t str_at(x360_machine *x, uint32_t a, uint64_t k, int wide)
{
    return wide ? x360_r16(x, a + 2 * (uint32_t)k) : x360_r8(x, a + (uint32_t)k);
}

/* a guest string: never read past the precision (the buffer need not be terminated) */
static void emit_str(x360_machine *x, fmt_sink *s, const fmt_spec *f, uint32_t a, int wide)
{
    static const char null_text[] = "(null)";
    uint64_t n = 0;
    if (!a)
        n = f->prec >= 0 && f->prec < 6 ? (uint64_t)f->prec : 6;
    else
        while ((f->prec < 0 || (int64_t)n < f->prec) && str_at(x, a, n, wide))
            n++;
    uint64_t pad = (uint64_t)f->width > n ? (uint64_t)f->width - n : 0;
    if (!f->left)
        sink_fill(s, ' ', pad);
    uint64_t room = sink_room(s), w = n < room ? n : room;
    for (uint64_t k = 0; k < w; k++)
        sink_put(s, a ? str_at(x, a, k, wide) : (uint8_t)null_text[k]);
    s->n += n - w;
    if (f->left)
        sink_fill(s, ' ', pad);
}

static uint32_t fmt_at(x360_machine *x, uint32_t a, int wide)
{
    return wide ? x360_r16(x, a) : x360_r8(x, a);
}

static int64_t fmt_number(x360_machine *x, uint32_t *p, int wide)
{
    int64_t v = 0;
    for (uint32_t ch; (ch = fmt_at(x, *p, wide)) >= '0' && ch <= '9'; *p += wide ? 2 : 1)
        v = v * 10 + (ch - '0') > FMT_MAX_FIELD ? FMT_MAX_FIELD : v * 10 + (ch - '0');
    return v;
}

/* The characters the format produces (all of them, whatever the sink's capacity). */
static uint64_t fmt_core(x360_machine *x, fmt_args *g, uint32_t fmt, int wide, fmt_sink *s)
{
    const uint32_t unit = wide ? 2 : 1;
    uint32_t p = fmt;
    for (;;) {
        uint32_t ch = fmt_at(x, p, wide);
        p += unit;
        if (!ch)
            break;
        if (ch != '%') {
            sink_put(s, ch);
            continue;
        }
        fmt_spec f;
        memset(&f, 0, sizeof f);
        f.prec = -1;
        for (;; p += unit) {
            ch = fmt_at(x, p, wide);
            if (ch == '-')
                f.left = 1;
            else if (ch == '+')
                f.plus = 1;
            else if (ch == ' ')
                f.space = 1;
            else if (ch == '#')
                f.alt = 1;
            else if (ch == '0')
                f.zero = 1;
            else
                break;
        }
        if (ch == '*') {
            int32_t v = (int32_t)arg_next(g);
            p += unit;
            f.left |= v < 0;
            f.width = v < 0 ? -(int64_t)v : v;
        } else {
            f.width = fmt_number(x, &p, wide);
        }
        if (fmt_at(x, p, wide) == '.') {
            p += unit;
            if (fmt_at(x, p, wide) == '*') {
                int32_t v = (int32_t)arg_next(g);
                p += unit;
                f.prec = v < 0 ? -1 : v;
            } else {
                f.prec = fmt_number(x, &p, wide);
            }
        }
        for (;; p += unit) {
            ch = fmt_at(x, p, wide);
            if (ch == 'h') {
                f.size = f.size == SZ_H ? SZ_HH : SZ_H;
            } else if (ch == 'l') {
                f.size = f.size == SZ_L ? SZ_LL : SZ_L;
            } else if (ch == 'w') {
                f.size = SZ_W;
            } else if (ch == 'j') {
                f.size = SZ_LL;
            } else if (ch == 'L' || ch == 'z' || ch == 't') {
                continue; /* long double is a double, size_t and ptrdiff_t are 32 bits */
            } else if (ch == 'I') {
                uint32_t a = fmt_at(x, p + unit, wide), b = a ? fmt_at(x, p + 2 * unit, wide) : 0;
                if (a == '6' && b == '4') {
                    f.size = SZ_LL;
                    p += 2 * unit;
                } else if (a == '3' && b == '2') {
                    f.size = SZ_NONE;
                    p += 2 * unit;
                }
            } else {
                break;
            }
        }
        if (!ch)
            break;
        p += unit;
        switch (ch) {
        case 'd':
        case 'i': {
            uint64_t v = arg_next(g);
            int64_t sv = f.size == SZ_LL   ? (int64_t)v
                         : f.size == SZ_H  ? (int16_t)v
                         : f.size == SZ_HH ? (int8_t)v
                                           : (int32_t)v;
            emit_int(s, &f, sv < 0 ? 0 - (uint64_t)sv : (uint64_t)sv, 1, sv < 0, 10, 0);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t v = arg_next(g);
            v = f.size == SZ_LL   ? v
                : f.size == SZ_H  ? (uint16_t)v
                : f.size == SZ_HH ? (uint8_t)v
                                  : (uint32_t)v;
            emit_int(s, &f, v, 0, 0, ch == 'u' ? 10 : ch == 'o' ? 8 : 16, ch == 'X');
            break;
        }
        case 'p': {
            fmt_spec q = f;
            q.prec = 8;
            q.alt = 0;
            emit_int(s, &q, (uint32_t)arg_next(g), 0, 0, 16, 1);
            break;
        }
        case 'c':
        case 'C':
        case 's':
        case 'S': {
            int upper = ch == 'C' || ch == 'S';
            int arg_wide = f.size == SZ_H || f.size == SZ_HH                     ? 0
                           : f.size == SZ_L || f.size == SZ_LL || f.size == SZ_W ? 1
                                                                                 : (upper ? !wide : wide);
            uint64_t v = arg_next(g);
            if (ch == 's' || ch == 'S') {
                emit_str(x, s, &f, (uint32_t)v, arg_wide);
            } else {
                uint64_t pad = f.width > 1 ? (uint64_t)f.width - 1 : 0;
                if (!f.left)
                    sink_fill(s, ' ', pad);
                sink_put(s, (uint32_t)v & (arg_wide ? 0xFFFFu : 0xFFu));
                if (f.left)
                    sink_fill(s, ' ', pad);
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            emit_float(s, &f, (char)ch, arg_next(g));
            break;
        case 'n':
            arg_next(g); /* disabled in the CRT: its argument is consumed, nothing is stored */
            break;
        case '%':
            sink_put(s, '%');
            break;
        default:
            sink_put(s, '%');
            sink_put(s, ch);
            break;
        }
    }
    return s->n;
}

static uint32_t fmt_result(uint64_t n)
{
    return n > 0x7FFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static fmt_args regs_from(x360_machine *x, xe_cpu *c, int first)
{
    fmt_args g = {x, c, first, 0, 0};
    return g;
}

static fmt_args va_from(x360_machine *x, xe_cpu *c, uint32_t va)
{
    fmt_args g = {x, c, 0, 1, va};
    return g;
}

/* sprintf, swprintf, vsprintf, vswprintf: no limit but the guest's buffer */
static void print_unbounded(x360_machine *x, xe_cpu *c, uint32_t buf, uint32_t fmt, int wide, fmt_args g)
{
    if (!buf || !fmt) {
        RET(0xFFFFFFFFu);
        return;
    }
    fmt_sink s = {x, buf, UINT64_MAX, 0, wide};
    uint64_t n = fmt_core(x, &g, fmt, wide, &s);
    sink_put(&s, 0);
    RET(fmt_result(n));
}

/* _snprintf and friends, as the CRT documents them: the length when it fits (terminated only when there is room for
 * the terminator: an exact fit has none), -1 after writing `count` characters when it does not; a null buffer with a
 * count of 0 asks for the length. */
static void print_bounded(x360_machine *x, xe_cpu *c, uint32_t buf, uint32_t count, uint32_t fmt, int wide, fmt_args g)
{
    if (!fmt || (!buf && count)) {
        RET(0xFFFFFFFFu);
        return;
    }
    fmt_sink s = {x, buf, count, 0, wide};
    uint64_t n = fmt_core(x, &g, fmt, wide, &s);
    if (buf && n < count)
        sink_put(&s, 0);
    RET(!buf || n <= count ? fmt_result(n) : 0xFFFFFFFFu);
}

static void print_count(x360_machine *x, xe_cpu *c, uint32_t fmt, int wide, fmt_args g)
{
    if (!fmt) {
        RET(0xFFFFFFFFu);
        return;
    }
    fmt_sink s = {x, 0, 0, 0, wide};
    RET(fmt_result(fmt_core(x, &g, fmt, wide, &s)));
}

#define PRINT_FN(name, call)                                                                                           \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        call;                                                                                                          \
    }

PRINT_FN(sprintf, print_unbounded(x, c, A(0), A(1), 0, regs_from(x, c, 5)))
PRINT_FN(swprintf, print_unbounded(x, c, A(0), A(1), 1, regs_from(x, c, 5)))
PRINT_FN(vsprintf, print_unbounded(x, c, A(0), A(1), 0, va_from(x, c, A(2))))
PRINT_FN(vswprintf, print_unbounded(x, c, A(0), A(1), 1, va_from(x, c, A(2))))
PRINT_FN(_snprintf, print_bounded(x, c, A(0), A(1), A(2), 0, regs_from(x, c, 6)))
PRINT_FN(_snwprintf, print_bounded(x, c, A(0), A(1), A(2), 1, regs_from(x, c, 6)))
PRINT_FN(_vsnprintf, print_bounded(x, c, A(0), A(1), A(2), 0, va_from(x, c, A(3))))
PRINT_FN(_vsnwprintf, print_bounded(x, c, A(0), A(1), A(2), 1, va_from(x, c, A(3))))
PRINT_FN(_scprintf, print_count(x, c, A(0), 0, regs_from(x, c, 4)))
PRINT_FN(_scwprintf, print_count(x, c, A(0), 1, regs_from(x, c, 4)))
PRINT_FN(_vscprintf, print_count(x, c, A(0), 0, va_from(x, c, A(1))))
PRINT_FN(_vscwprintf, print_count(x, c, A(0), 1, va_from(x, c, A(1))))

const x360_hle_row x360_tab_rtl[] = {
    X360_HK(sprintf, k_sprintf),
    X360_HK(swprintf, k_swprintf),
    X360_HK(vsprintf, k_vsprintf),
    X360_HK(vswprintf, k_vswprintf),
    X360_HK(_snprintf, k__snprintf),
    X360_HK(_snwprintf, k__snwprintf),
    X360_HK(_vsnprintf, k__vsnprintf),
    X360_HK(_vsnwprintf, k__vsnwprintf),
    X360_HK(_scprintf, k__scprintf),
    X360_HK(_scwprintf, k__scwprintf),
    X360_HK(_vscprintf, k__vscprintf),
    X360_HK(_vscwprintf, k__vscwprintf),
};
const unsigned x360_tab_rtl_n = sizeof x360_tab_rtl / sizeof x360_tab_rtl[0];
