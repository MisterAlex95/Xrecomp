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

/* ---- case: fixed rules, never the host's locale ---- */

/* the kernel's single-byte tables: ASCII and Latin-1 letters; y with diaeresis has no Latin-1 capital and gives '?' */
static uint32_t upper8(uint32_t ch)
{
    ch &= 0xFF;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7))
        return ch - 0x20;
    return ch == 0xFF ? '?' : ch;
}

static uint32_t lower8(uint32_t ch)
{
    ch &= 0xFF;
    return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? ch + 0x20 : ch;
}

/* Latin Extended-A pairs: capital even in these ranges, odd in the two others */
static int ext_a_even(uint32_t ch)
{
    return (ch >= 0x100 && ch <= 0x137 && ch != 0x130 && ch != 0x131) || (ch >= 0x14A && ch <= 0x177);
}

static int ext_a_odd(uint32_t ch)
{
    return (ch >= 0x139 && ch <= 0x148) || (ch >= 0x179 && ch <= 0x17E);
}

/* One-to-one pairs of Latin-1, Latin Extended-A, Greek, Cyrillic and the fullwidth forms; the rest is unchanged. */
static uint32_t upper16(uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z')
        return ch - 0x20;
    if (ch < 0x80)
        return ch;
    if (ch >= 0xE0 && ch <= 0xFE && ch != 0xF7)
        return ch - 0x20;
    if (ch == 0xFF)
        return 0x178;
    if (ext_a_even(ch))
        return ch & 1 ? ch - 1 : ch;
    if (ext_a_odd(ch))
        return ch & 1 ? ch : ch - 1;
    if (ch == 0x3AC)
        return 0x386;
    if (ch >= 0x3AD && ch <= 0x3AF)
        return ch - 0x25;
    if (ch == 0x3CC)
        return 0x38C;
    if (ch == 0x3CD || ch == 0x3CE)
        return ch - 0x3F;
    if ((ch >= 0x3B1 && ch <= 0x3C9 && ch != 0x3C2) || (ch >= 0x430 && ch <= 0x44F) || (ch >= 0xFF41 && ch <= 0xFF5A))
        return ch - 0x20;
    if (ch >= 0x450 && ch <= 0x45F)
        return ch - 0x50;
    return ch;
}

static uint32_t lower16(uint32_t ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch + 0x20;
    if (ch < 0x80)
        return ch;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7)
        return ch + 0x20;
    if (ch == 0x178)
        return 0xFF;
    if (ext_a_even(ch))
        return ch & 1 ? ch : ch + 1;
    if (ext_a_odd(ch))
        return ch & 1 ? ch + 1 : ch;
    if (ch == 0x386)
        return 0x3AC;
    if (ch >= 0x388 && ch <= 0x38A)
        return ch + 0x25;
    if (ch == 0x38C)
        return 0x3CC;
    if (ch == 0x38E || ch == 0x38F)
        return ch + 0x3F;
    if ((ch >= 0x391 && ch <= 0x3A9 && ch != 0x3A2) || (ch >= 0x410 && ch <= 0x42F) || (ch >= 0xFF21 && ch <= 0xFF3A))
        return ch + 0x20;
    if (ch >= 0x400 && ch <= 0x40F)
        return ch + 0x50;
    return ch;
}

/* ---- counted strings: STRING and UNICODE_STRING {Length, MaximumLength, Buffer}, lengths in bytes ---- */

#define X_STATUS_SOME_NOT_MAPPED 0x00000107u
#define X_STATUS_BUFFER_OVERFLOW 0x80000005u
#define X_STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define X_STATUS_INVALID_PARAMETER_2 0xC00000F0u
#define X_STATUS_INVALID_PARAMETER_4 0xC00000F2u
#define X_STATUS_INVALID_PARAMETER_5 0xC00000F3u

typedef struct gs {
    uint32_t len, max, buf;
} gs;

static gs gs_read(x360_machine *x, uint32_t a)
{
    gs s = {x360_r16(x, a), x360_r16(x, a + 2), x360_r32(x, a + 4)};
    return s;
}

/* overlapping ranges are fine, as with RtlMoveMemory */
static void gmove(x360_machine *x, uint32_t dst, uint32_t src, uint32_t n)
{
    if (dst <= src)
        for (uint32_t k = 0; k < n; k++)
            x360_w8(x, dst + k, x360_r8(x, src + k));
    else
        for (uint32_t k = n; k--;)
            x360_w8(x, dst + k, x360_r8(x, src + k));
}

/* characters before the terminator, stopping once past what a UNICODE_STRING can hold */
static uint32_t wcs_len(x360_machine *x, uint32_t a)
{
    uint32_t n = 0;
    while (n < 0x8000 && x360_r16(x, a + 2 * n))
        n++;
    return n;
}

static void k_RtlCompareMemory(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* NT's result: the length of the matching prefix, not the number of equal bytes */
    uint32_t a = A(0), b = A(1), n = A(2), k = 0;
    while (k < n && x360_r8(x, a + k) == x360_r8(x, b + k))
        k++;
    RET(k);
}

static void k_RtlUpperChar(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(upper8(A(0)));
}

static void k_RtlLowerChar(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(lower8(A(0)));
}

static void k_RtlUpcaseUnicodeChar(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(upper16(A(0) & 0xFFFF));
}

static void k_RtlDowncaseUnicodeChar(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(lower16(A(0) & 0xFFFF));
}

static void k_RtlCompareString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    gs a = gs_read(x, A(0)), b = gs_read(x, A(1));
    int ci = (A(2) & 0xFF) != 0;
    uint32_t n = a.len < b.len ? a.len : b.len;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ca = x360_r8(x, a.buf + k), cb = x360_r8(x, b.buf + k);
        if (ci)
            ca = upper8(ca), cb = upper8(cb);
        if (ca != cb) {
            RET((int32_t)ca - (int32_t)cb);
            return;
        }
    }
    RET((int32_t)a.len - (int32_t)b.len);
}

static void k_RtlCompareUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    gs a = gs_read(x, A(0)), b = gs_read(x, A(1));
    int ci = (A(2) & 0xFF) != 0;
    uint32_t n = (a.len < b.len ? a.len : b.len) / 2;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t ca = x360_r16(x, a.buf + 2 * k), cb = x360_r16(x, b.buf + 2 * k);
        if (ci)
            ca = upper16(ca), cb = upper16(cb);
        if (ca != cb) {
            RET((int32_t)ca - (int32_t)cb);
            return;
        }
    }
    RET((int32_t)a.len - (int32_t)b.len);
}

static void k_RtlCopyString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), src = A(1);
    if (!src) {
        x360_w16(x, dst, 0);
        return;
    }
    gs d = gs_read(x, dst), s = gs_read(x, src);
    uint32_t n = s.len < d.max ? s.len : d.max;
    gmove(x, d.buf, s.buf, n);
    x360_w16(x, dst, n);
}

static void k_RtlCopyUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), src = A(1);
    if (!src) {
        x360_w16(x, dst, 0);
        return;
    }
    gs d = gs_read(x, dst), s = gs_read(x, src);
    uint32_t n = s.len < d.max ? s.len : d.max;
    gmove(x, d.buf, s.buf, n);
    x360_w16(x, dst, n);
    if (!(n & 1) && n + 2 <= d.max)
        x360_w16(x, d.buf + n, 0);
}

enum { TERM_NONE, TERM_IF_COPIED, TERM_ALWAYS };

static uint32_t append(x360_machine *x, uint32_t dst, uint32_t src, uint32_t n, int term)
{
    gs d = gs_read(x, dst);
    if (d.len + n > d.max)
        return X_STATUS_BUFFER_TOO_SMALL;
    gmove(x, d.buf + d.len, src, n);
    d.len += n;
    x360_w16(x, dst, d.len);
    if ((term == TERM_ALWAYS || (term == TERM_IF_COPIED && n)) && d.len + 2 <= d.max)
        x360_w16(x, d.buf + d.len, 0);
    return X_STATUS_SUCCESS;
}

static void k_RtlAppendStringToString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    gs s = gs_read(x, A(1));
    RET(append(x, A(0), s.buf, s.len, TERM_NONE));
}

static void k_RtlAppendUnicodeStringToString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    gs s = gs_read(x, A(1));
    RET(append(x, A(0), s.buf, s.len, TERM_IF_COPIED));
}

static void k_RtlAppendUnicodeToString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t src = A(1);
    RET(src ? append(x, A(0), src, 2 * wcs_len(x, src), TERM_ALWAYS) : X_STATUS_SUCCESS);
}

/* The runtime's ANSI is one byte per character (RtlMultiByteToUnicodeN), so the two sizes are a factor of 2. */
static void k_RtlAnsiStringToUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), alloc = A(2) & 0xFF;
    gs s = gs_read(x, A(1));
    uint32_t size = 2 * (s.len + 1), len = size - 2, buf;
    if (size > 0xFFFF) {
        RET(X_STATUS_INVALID_PARAMETER_2);
        return;
    }
    x360_w16(x, dst, len);
    if (alloc) {
        buf = x360_heap_alloc(x, &x->heap_v4k, size, 0);
        if (!buf) {
            x360_w16(x, dst, 0);
            RET(X_STATUS_NO_MEMORY);
            return;
        }
        x360_w16(x, dst + 2, size);
        x360_w32(x, dst + 4, buf);
    } else {
        if (size > x360_r16(x, dst + 2)) { /* the terminator is part of what must fit */
            RET(X_STATUS_BUFFER_OVERFLOW);
            return;
        }
        buf = x360_r32(x, dst + 4);
    }
    for (uint32_t k = s.len; k--;) /* from the end: the source may be the start of the destination */
        x360_w16(x, buf + 2 * k, x360_r8(x, s.buf + k));
    x360_w16(x, buf + len, 0);
    RET(X_STATUS_SUCCESS);
}

static void k_RtlCreateUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t dst = A(0), src = A(1);
    uint32_t size = src ? 2 * (wcs_len(x, src) + 1) : 0;
    uint32_t buf = size && size <= 0xFFFE ? x360_heap_alloc(x, &x->heap_v4k, size, 0) : 0;
    if (!buf) {
        RET(0); /* a BOOLEAN */
        return;
    }
    gmove(x, buf, src, size);
    x360_w16(x, dst, size - 2);
    x360_w16(x, dst + 2, size);
    x360_w32(x, dst + 4, buf);
    RET(1);
}

static void k_RtlFreeUnicodeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t s = A(0), buf = s ? x360_r32(x, s + 4) : 0;
    if (buf) {
        x360_heap_free(x, &x->heap_v4k, buf);
        x360_w32(x, s, 0);
        x360_w32(x, s + 4, 0);
    }
    RET(0);
}

static void k_RtlMultiByteToUnicodeSize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), 2 * A(2));
    RET(X_STATUS_SUCCESS);
}

static void k_RtlUnicodeToMultiByteSize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), A(2) / 2);
    RET(X_STATUS_SUCCESS);
}

/* The code point at unit *k of n; an unpaired surrogate is U+FFFD and sets *bad. */
static uint32_t utf16_next(x360_machine *x, uint32_t src, uint32_t *k, uint32_t n, int *bad)
{
    uint32_t ch = x360_r16(x, src + 2 * (*k)++);
    if (ch < 0xD800 || ch > 0xDFFF)
        return ch;
    if (ch < 0xDC00 && *k < n) {
        uint32_t lo = x360_r16(x, src + 2 * *k);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            ++*k;
            return 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
        }
    }
    *bad = 1;
    return 0xFFFD;
}

static uint32_t utf8_size(uint32_t cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void k_RtlUnicodeToUtf8(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* as RtlUnicodeToUTF8N: (dst, dst bytes, written bytes*, src, src bytes); no dst asks for the size */
    uint32_t dst = A(0), cap = A(1), outp = A(2), src = A(3), bytes = A(4);
    if (!src) {
        RET(X_STATUS_INVALID_PARAMETER_4);
        return;
    }
    if (!outp) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    if (dst && (bytes & 1)) {
        RET(X_STATUS_INVALID_PARAMETER_5);
        return;
    }
    uint32_t n = bytes / 2, o = 0;
    int bad = 0;
    for (uint32_t k = 0; k < n;) {
        uint32_t cp = utf16_next(x, src, &k, n, &bad), size = utf8_size(cp);
        if (dst) {
            if (size > cap - o) { /* only whole characters are written */
                x360_w32(x, outp, o);
                RET(X_STATUS_BUFFER_TOO_SMALL);
                return;
            }
            static const uint8_t lead[5] = {0, 0, 0xC0, 0xE0, 0xF0};
            for (uint32_t i = size; i-- > 1; cp >>= 6)
                x360_w8(x, dst + o + i, 0x80 | (cp & 0x3F));
            x360_w8(x, dst + o, size == 1 ? cp : lead[size] | cp);
        }
        o += size;
    }
    x360_w32(x, outp, o);
    RET(bad ? X_STATUS_SOME_NOT_MAPPED : X_STATUS_SUCCESS);
}

static void k_RtlUnicodeToUtf8Size(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (size*, src, src bytes), the shape of RtlUnicodeToMultiByteSize */
    uint32_t src = A(1), n = A(2) / 2, o = 0;
    int bad = 0;
    for (uint32_t k = 0; k < n;)
        o += utf8_size(utf16_next(x, src, &k, n, &bad));
    if (A(0))
        x360_w32(x, A(0), o);
    RET(X_STATUS_SUCCESS);
}

/* ---- the stack: frames walked with x360_seh.c, from the .pdata of the image ---- */

static void k_RtlLookupFunctionEntry(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_seh_fn f;
    RET(x360_seh_lookup(x, A(0), &f) ? f.entry : 0);
}

/* NT's PowerPC form: (ControlPc, FunctionEntry, ContextRecord, InFunction*, EstablisherFrame*, ContextPointers,
 * LowStackLimit, HighStackLimit), the caller's PC as the result and the caller's state in the record. The optional
 * ContextPointers are not filled in; the limits are not needed, the frames are read from the back chain. */
static void k_RtlVirtualUnwind(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t pc = A(0), entry = A(1), rec = A(2), in_fn = A(3), frame = A(4);
    x360_seh_ctx cur, next;
    x360_seh_fn f;
    memset(&cur, 0, sizeof cur);
    x360_seh_read_context(x, rec, &cur);
    cur.pc = pc;
    int have;
    if (entry) {
        uint32_t info = x360_r32(x, entry + 4);
        f.entry = entry, f.start = x360_r32(x, entry), f.info = info;
        f.prolog = info & 0xFF, f.len = ((info >> 8) & 0x3FFFFF) * 4;
        have = 1;
    } else {
        have = x360_seh_lookup(x, pc, &f);
    }
    x360_seh_virtual_unwind(x, &cur, have ? &f : NULL, &next);
    if (in_fn)
        x360_w8(x, in_fn, have && pc >= f.start + 4 * f.prolog && pc - f.start < f.len);
    if (frame)
        x360_w32(x, frame, (uint32_t)cur.gpr[1]);
    x360_seh_write_context(x, rec, &next);
    RET(next.pc);
}

typedef struct walker {
    x360_seh_ctx cur;
    int done;
} walker;

static int in_image(const x360_machine *x, uint32_t pc)
{
    return x->image_size && pc >= x->image_base && pc - x->image_base < x->image_size;
}

static int on_stack(const x360_thread *t, uint32_t sp)
{
    return sp >= t->stack_limit && sp <= t->stack_base;
}

/* frame 0 is the code the export returns to */
static void walk_start(walker *w, const xe_cpu *c)
{
    memset(w, 0, sizeof *w);
    memcpy(w->cur.gpr, c->gpr, sizeof w->cur.gpr);
    w->cur.lr = c->lr;
    w->cur.pc = c->pc;
}

/* the next frame's PC, 0 at the end of the chain */
static uint32_t walk_next(x360_machine *x, const x360_thread *t, walker *w)
{
    uint32_t pc = w->cur.pc, sp = (uint32_t)w->cur.gpr[1];
    if (w->done || !in_image(x, pc)) {
        w->done = 1;
        return 0;
    }
    x360_seh_fn f;
    x360_seh_ctx next;
    /* the back chain is checked before the unwind reads the slots it points at: a bad one ends the walk, not the run */
    if (!on_stack(t, sp) || !on_stack(t, x360_r32(x, sp)) || !x360_seh_lookup(x, pc - 4, &f)) {
        w->done = 1; /* this frame is known, its caller cannot be read */
        return pc;
    }
    x360_seh_virtual_unwind(x, &w->cur, &f, &next);
    uint32_t up = (uint32_t)next.gpr[1];
    if (up < sp || !on_stack(t, up) || (up == sp && next.pc == pc))
        w->done = 1;
    w->cur = next;
    return pc;
}

static void k_RtlCaptureStackBackTrace(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (frames to skip, frames to capture, array*, hash*): the number captured, the hash is the sum of their PCs */
    uint32_t skip = A(0), want = A(1), out = A(2), hash = A(3), n = 0, sum = 0;
    walker w;
    walk_start(&w, c);
    for (uint32_t k = 0; k < 4096 && n < want; k++) {
        uint32_t pc = walk_next(x, t, &w);
        if (!pc)
            break;
        if (k < skip)
            continue;
        if (out)
            x360_w32(x, out + 4 * n, pc);
        sum += pc;
        n++;
    }
    if (hash)
        x360_w32(x, hash, sum);
    RET(n);
}

static void k_RtlGetCallersAddress(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (caller*, caller's caller*) of the function that asks: the frames after its own */
    uint32_t out1 = A(0), out2 = A(1);
    walker w;
    walk_start(&w, c);
    uint32_t self = walk_next(x, t, &w), a = self ? walk_next(x, t, &w) : 0, b = a ? walk_next(x, t, &w) : 0;
    if (out1)
        x360_w32(x, out1, a);
    if (out2)
        x360_w32(x, out2, b);
    RET(0);
}

static void k_RtlGetStackLimits(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    if (A(0))
        x360_w32(x, A(0), t->stack_limit);
    if (A(1))
        x360_w32(x, A(1), t->stack_base);
    RET(0);
}

static void k_RtlRaiseStatus(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* an EXCEPTION_RECORD (code, flags, record, address, parameter count, 15 parameters) just below the caller's frame,
     * above the area x360_seh.c dispatches from */
    uint32_t rec = ((uint32_t)c->gpr[1] - 0x60) & ~7u;
    for (uint32_t k = 0; k < 0x50; k += 4)
        x360_w32(x, rec + k, 0);
    x360_w32(x, rec, A(0));
    x360_w32(x, rec + 4, 1); /* EXCEPTION_NONCONTINUABLE */
    x360_w32(x, rec + 12, c->pc);
    x360_seh_raise(x, t, rec);
}

/* ---- the image: PE headers are little-endian in guest memory ---- */

static uint32_t le16(x360_machine *x, uint32_t a)
{
    return x360_r8(x, a) | (uint32_t)x360_r8(x, a + 1) << 8;
}

static uint32_t le32(x360_machine *x, uint32_t a)
{
    return le16(x, a) | le16(x, a + 2) << 16;
}

/* a module handle stands for its image */
static uint32_t image_of(const x360_machine *x, uint32_t base)
{
    return base && base == x->module ? x->image_base : base;
}

/* IMAGE_NT_HEADERS of the image at base, 0 when it has none; the headers are in its first page */
static uint32_t nt_headers(x360_machine *x, uint32_t base)
{
    if (!base || x360_r8(x, base) != 'M' || x360_r8(x, base + 1) != 'Z')
        return 0;
    uint32_t lfanew = le32(x, base + 0x3C);
    if (lfanew > 0x1000 - 0x18 || le32(x, base + lfanew) != 0x00004550u)
        return 0;
    return base + lfanew;
}

static void k_RtlImageNtHeader(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(nt_headers(x, image_of(x, A(0))));
}

/* (base, mapped as image, directory, size*): bit 0 of the base marks an image mapped as a data file */
static void k_RtlImageDirectoryEntryToData(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t base = image_of(x, A(0)), mapped = A(1) & 0xFF, dir = A(2) & 0xFFFF, size = A(3);
    if (base & 1) {
        base &= ~1u;
        mapped = 0;
    }
    uint32_t nt = nt_headers(x, base);
    uint32_t opt = nt + 24, magic = nt ? le16(x, opt) : 0;
    if (magic != 0x10B && magic != 0x20B) { /* PE32, PE32+ */
        RET(0);
        return;
    }
    uint32_t count = le32(x, opt + (magic == 0x10B ? 92 : 108)), dirs = opt + (magic == 0x10B ? 96 : 112);
    uint32_t rva = dir < count ? le32(x, dirs + 8 * dir) : 0;
    if (!rva) {
        RET(0);
        return;
    }
    if (size)
        x360_w32(x, size, le32(x, dirs + 8 * dir + 4));
    if (mapped || rva < le32(x, opt + 60)) { /* SizeOfHeaders: the same offset in either view */
        RET(base + rva);
        return;
    }
    uint32_t sec = opt + le16(x, nt + 20), nsec = le16(x, nt + 6);
    for (uint32_t k = 0; k < nsec && k < 96; k++, sec += 40) {
        uint32_t va = le32(x, sec + 12), raw = le32(x, sec + 16), ptr = le32(x, sec + 20);
        if (rva >= va && rva - va < raw) {
            RET(base + ptr + (rva - va));
            return;
        }
    }
    RET(0);
}

static void k_XexPcToFileHeader(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (pc, loader entry*): the image holding pc */
    int hit = in_image(x, A(0));
    if (A(1))
        x360_w32(x, A(1), hit ? x->module : 0);
    RET(hit ? x->image_base : 0);
}

/* The title is the only module this runtime has: nothing else can be loaded. */
static void k_XexLoadImage(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(3))
        x360_w32(x, A(3), 0);
    RET(0xC0000034u); /* STATUS_OBJECT_NAME_NOT_FOUND */
}

static void k_XexUnloadImage(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(A(0) && A(0) == x->module ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE);
}

/* CRC-32 (IEEE, reflected), four bits at a time */
static void k_RtlComputeCrc32(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    static const uint32_t nibble[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
                                        0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint32_t crc = ~A(0), p = A(1), n = A(2);
    for (uint32_t k = 0; k < n; k++) {
        crc ^= x360_r8(x, p + k);
        crc = crc >> 4 ^ nibble[crc & 15];
        crc = crc >> 4 ^ nibble[crc & 15];
    }
    RET(~crc);
}

const x360_hle_row x360_tab_rtl[] = {
    X360_HK(RtlLookupFunctionEntry, k_RtlLookupFunctionEntry),
    X360_HK(RtlVirtualUnwind, k_RtlVirtualUnwind),
    X360_HK(RtlCaptureStackBackTrace, k_RtlCaptureStackBackTrace),
    X360_HK(RtlGetCallersAddress, k_RtlGetCallersAddress),
    X360_HK(RtlGetStackLimits, k_RtlGetStackLimits),
    X360_HK(RtlRaiseStatus, k_RtlRaiseStatus),
    X360_HK(RtlImageNtHeader, k_RtlImageNtHeader),
    X360_HK(RtlImageDirectoryEntryToData, k_RtlImageDirectoryEntryToData),
    X360_HK(XexPcToFileHeader, k_XexPcToFileHeader),
    X360_HK(XexLoadImage, k_XexLoadImage),
    X360_HK(XexUnloadImage, k_XexUnloadImage),
    X360_HK(RtlComputeCrc32, k_RtlComputeCrc32),
    X360_HK(RtlCompareMemory, k_RtlCompareMemory),
    X360_HK(RtlUpperChar, k_RtlUpperChar),
    X360_HK(RtlLowerChar, k_RtlLowerChar),
    X360_HK(RtlUpcaseUnicodeChar, k_RtlUpcaseUnicodeChar),
    X360_HK(RtlDowncaseUnicodeChar, k_RtlDowncaseUnicodeChar),
    X360_HK(RtlCompareString, k_RtlCompareString),
    X360_HK(RtlCompareUnicodeString, k_RtlCompareUnicodeString),
    X360_HK(RtlCopyString, k_RtlCopyString),
    X360_HK(RtlCopyUnicodeString, k_RtlCopyUnicodeString),
    X360_HK(RtlAppendStringToString, k_RtlAppendStringToString),
    X360_HK(RtlAppendUnicodeStringToString, k_RtlAppendUnicodeStringToString),
    X360_HK(RtlAppendUnicodeToString, k_RtlAppendUnicodeToString),
    X360_HK(RtlAnsiStringToUnicodeString, k_RtlAnsiStringToUnicodeString),
    X360_HK(RtlCreateUnicodeString, k_RtlCreateUnicodeString),
    X360_HK(RtlFreeUnicodeString, k_RtlFreeUnicodeString),
    X360_HK(RtlMultiByteToUnicodeSize, k_RtlMultiByteToUnicodeSize),
    X360_HK(RtlUnicodeToMultiByteSize, k_RtlUnicodeToMultiByteSize),
    X360_HK(RtlUnicodeToUtf8, k_RtlUnicodeToUtf8),
    X360_HK(RtlUnicodeToUtf8Size, k_RtlUnicodeToUtf8Size),
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
