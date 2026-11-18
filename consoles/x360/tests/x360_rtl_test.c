/* x360_rtl_test -- the rtl HLE group called directly (XBA-296). */
#include "x360_hle_util.h"

static int checks;
#define T(cond, ...)                                                                                                   \
    do {                                                                                                               \
        checks++;                                                                                                      \
        CHECK(cond, __VA_ARGS__);                                                                                      \
    } while (0)

/* an export with 64-bit arguments (K() passes 32 bits): arguments 9 and up in the caller's stack slots */
static uint32_t K64(unsigned ord, int n, const uint64_t *args)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    for (int k = 0; k < n; k++) {
        if (k < 8)
            c->gpr[3 + k] = args[k];
        else
            x360_w64(X, stack_top + 0x50 + 8 * (uint32_t)(k - 8), args[k]);
    }
    x360_thunk th = {0, (uint16_t)ord, X360_LIB_XBOXKRNL};
    X->m.slice = 1000;
    x360_hle_call(X, &th);
    return (uint32_t)c->gpr[3];
}

/* a va_list in guest memory: 8-byte slots */
static uint32_t gva(int n, const uint64_t *args)
{
    uint32_t a = galloc(8 * (uint32_t)n + 8);
    for (int k = 0; k < n; k++)
        x360_w64(X, a + 8 * (uint32_t)k, args[k]);
    return a;
}

static uint64_t dbl(double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    return v;
}

/* a buffer of `n` bytes filled with 0x7E, to see what was written */
static uint32_t gbuf(uint32_t n)
{
    uint32_t a = x360_heap_alloc(X, &X->heap_v4k, n, 0);
    memset(X->base + a, 0x7E, n);
    return a;
}

static int weq(uint32_t a, const char *s)
{
    uint32_t k = 0;
    for (; s[k]; k++)
        if (x360_r16(X, a + 2 * k) != (uint8_t)s[k])
            return 0;
    return x360_r16(X, a + 2 * k) == 0;
}

/* the first n units only: no terminator expected */
static int wneq(uint32_t a, const char *s, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++)
        if (x360_r16(X, a + 2 * k) != (uint8_t)s[k])
            return 0;
    return 1;
}

static const char *nstr(uint32_t a) { return (const char *)X->base + a; }

/* ---- printf ---- */

/* one narrow format through the register and the va_list exports, and the count */
static void fmt_case(const char *fmt, const char *want, int n, const uint64_t *args)
{
    uint32_t f = gstr(fmt), va = gva(n, args), buf = gbuf(0x2000);
    uint64_t a[12] = {buf, f};
    for (int k = 0; k < n; k++)
        a[2 + k] = args[k];
    uint32_t r = K64(X360_xboxkrnl_sprintf, 2 + n, a);
    T(r == strlen(want) && !strcmp(nstr(buf), want), "sprintf(\"%s\"): %d \"%s\", want \"%s\"", fmt, (int)r, nstr(buf),
      want);
    r = K(X360_xboxkrnl_vsprintf, 3, buf, f, va);
    T(r == strlen(want) && !strcmp(nstr(buf), want), "vsprintf(\"%s\"): %d \"%s\"", fmt, (int)r, nstr(buf));
    r = K(X360_xboxkrnl__vscprintf, 2, f, va);
    T(r == strlen(want), "_vscprintf(\"%s\") = %d", fmt, (int)r);
    a[0] = f;
    for (int k = 0; k < n; k++)
        a[1 + k] = args[k];
    r = K64(X360_xboxkrnl__scprintf, 1 + n, a);
    T(r == strlen(want), "_scprintf(\"%s\") = %d", fmt, (int)r);
    x360_heap_free(X, &X->heap_v4k, buf);
}

/* the same through the wide exports */
static void wfmt_case(const char *fmt, const char *want, int n, const uint64_t *args)
{
    uint32_t f = gwstr(fmt), va = gva(n, args), buf = gbuf(0x4000);
    uint64_t a[12] = {buf, f};
    for (int k = 0; k < n; k++)
        a[2 + k] = args[k];
    uint32_t r = K64(X360_xboxkrnl_swprintf, 2 + n, a);
    T(r == strlen(want) && weq(buf, want), "swprintf(\"%s\") = %d, want \"%s\"", fmt, (int)r, want);
    r = K(X360_xboxkrnl_vswprintf, 3, buf, f, va);
    T(r == strlen(want) && weq(buf, want), "vswprintf(\"%s\") = %d", fmt, (int)r);
    r = K(X360_xboxkrnl__vscwprintf, 2, f, va);
    T(r == strlen(want), "_vscwprintf(\"%s\") = %d", fmt, (int)r);
    a[0] = f;
    for (int k = 0; k < n; k++)
        a[1 + k] = args[k];
    r = K64(X360_xboxkrnl__scwprintf, 1 + n, a);
    T(r == strlen(want), "_scwprintf(\"%s\") = %d", fmt, (int)r);
    a[0] = buf, a[1] = 0x1000, a[2] = f;
    for (int k = 0; k < n; k++)
        a[3 + k] = args[k];
    r = K64(X360_xboxkrnl__snwprintf, 3 + n, a);
    T(r == strlen(want) && weq(buf, want), "_snwprintf(\"%s\") = %d", fmt, (int)r);
    r = K(X360_xboxkrnl__vsnwprintf, 4, buf, 0x1000, f, va);
    T(r == strlen(want) && weq(buf, want), "_vsnwprintf(\"%s\") = %d", fmt, (int)r);
    x360_heap_free(X, &X->heap_v4k, buf);
}

#define ARGS(...) (sizeof((uint64_t[]){__VA_ARGS__}) / 8), (uint64_t[]) { __VA_ARGS__ }
#define NF(fmt, want, ...) fmt_case(fmt, want, (int)ARGS(__VA_ARGS__))
#define WF(fmt, want, ...) wfmt_case(fmt, want, (int)ARGS(__VA_ARGS__))

static void test_conversions(void)
{
    uint32_t ab = gstr("ab"), wab = gwstr("wide");
    NF("plain %%", "plain %", 0);
    NF("%d|%i|%u", "-5|7|4294967291", (uint32_t)-5, 7, (uint32_t)-5);
    NF("%5d|%-5d|%05d|%+d|% d", "   42|42   |00042|+42| 42", 42, 42, 42, 42, 42);
    NF("%.3d|%.0d|%5.3d", "007||  007", 7, 0, 7);
    NF("%x|%X|%#x|%#X|%o|%#o", "ff|FF|0xff|0XFF|10|010", 255, 255, 255, 255, 8, 8);
    NF("%#x|%#o", "0|0", 0, 0);
    NF("%hd|%hu|%hhd", "-1|65535|-1", 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFu);
    NF("%ld|%lu", "-1|4294967295", 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
    NF("%I64d|%lld|%I64u|%I64x", "-2|-2|18446744073709551614|fffffffffffffffe", (uint64_t)-2, (uint64_t)-2,
       (uint64_t)-2, (uint64_t)-2);
    NF("%I32d|%I32x", "-1|ffffffff", 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull);
    NF("%*d|%-*d|%*d", "   7|7   |7   ", 4, 7, 4, 7, (uint32_t)-4, 7);
    NF("%.*d|%.*d", "007|7", 3, 7, (uint32_t)-1, 7);
    NF("%p", "0012ABCD", 0x12ABCD);
    NF("%c%c", "xy", 'x', 'y');
    NF("%3c|%-3c|", "  x|x  |", 'x', 'x');
    NF("%s|%5s|%-5s|%.1s|%.*s", "ab|   ab|ab   |a|a", ab, ab, ab, ab, 1, ab);
    NF("%s", "(null)", 0);
    NF("%.3s", "(nu", 0);
    NF("%S|%ls|%ws|%hs", "wide|wide|wide|ab", wab, wab, wab, ab);
    NF("%C|%lc|%hc", "W|W|n", 'W', 'W', 'n');
    NF("%f|%.2f|%8.3f|%-8.1f|%08.2f", "1.500000|1.50|   1.500|1.5     |00001.50", dbl(1.5), dbl(1.5), dbl(1.5), dbl(1.5),
       dbl(1.5));
    NF("%+.1f|% .1f|%.1f", "+2.5| 2.5|-2.5", dbl(2.5), dbl(2.5), dbl(-2.5));
    NF("%08.2f", "-0001.50", dbl(-1.5));
    NF("%e|%G|%g", "1.500000e+00|1E+20|0.25", dbl(1.5), dbl(1e20), dbl(0.25));
    NF("%d %f %s", "1 2.000000 ab", 1, dbl(2.0), ab);
    NF("%y|%", "%y|", 0);
    NF("%n%d", "9", 0, 9);

    WF("%s|%S|%ls|%ws|%hs", "wide|ab|wide|wide|ab", wab, ab, wab, wab, ab);
    WF("%c|%C|%lc|%hc", "W|n|W|n", 'W', 'n', 'W', 'n');
    WF("%5s|%-5s|%.2s", " wide|wide |wi", wab, wab, wab);
    WF("%s", "(null)", 0);
    WF("%d|%I64d|%05.1f|%*d", "3|-9|002.5|  1", 3, (uint64_t)-9, dbl(2.5), 3, 1);

    /* a wide argument with no Latin-1 form on narrow output, a narrow one widened by value */
    uint32_t w = galloc(8), buf = gbuf(64);
    x360_w16(X, w, 0x00E9), x360_w16(X, w + 2, 0x3042), x360_w16(X, w + 4, 0);
    T(K(X360_xboxkrnl_sprintf, 3, buf, gstr("%S"), w) == 2 && !strcmp(nstr(buf), "\xE9?"), "wide to narrow: \"%s\"",
      nstr(buf));
    uint32_t n = gstr("\xE9");
    K(X360_xboxkrnl_swprintf, 3, buf, gwstr("%S"), n);
    T(x360_r16(X, buf) == 0xE9 && !x360_r16(X, buf + 2), "narrow to wide: %04X", x360_r16(X, buf));
    x360_heap_free(X, &X->heap_v4k, buf);

    /* the precision bounds the read: this string has no terminator in its 3 characters */
    uint32_t raw = galloc(4);
    memcpy(X->base + raw, "abcd", 4);
    buf = gbuf(16);
    K(X360_xboxkrnl_sprintf, 3, buf, gstr("%.3s"), raw);
    T(!strcmp(nstr(buf), "abc"), "precision: \"%s\"", nstr(buf));
    x360_heap_free(X, &X->heap_v4k, buf);

    /* arguments past the eighth register come from the caller's stack */
    uint64_t many[12] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    buf = gbuf(64);
    many[0] = buf, many[1] = gstr("%d %d %d %d %d %d %d %d %d %d");
    K64(X360_xboxkrnl_sprintf, 12, many);
    T(!strcmp(nstr(buf), "1 2 3 4 5 6 7 8 9 10"), "stack arguments: \"%s\"", nstr(buf));
    x360_heap_free(X, &X->heap_v4k, buf);
}

static void test_truncation(void)
{
    uint32_t buf = gbuf(64), f = gwstr("%d");
    /* fits with room: terminated */
    T(K(X360_xboxkrnl__snwprintf, 4, buf, 5, f, 1234) == 4 && weq(buf, "1234"), "room");
    /* exact fit: the length, no terminator */
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snwprintf, 4, buf, 4, f, 1234) == 4 && wneq(buf, "1234", 4) && x360_r16(X, buf + 8) == 0x7E7E,
      "exact fit");
    /* one short: -1, the characters that fit, nothing after them */
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snwprintf, 4, buf, 3, f, 1234) == 0xFFFFFFFFu && wneq(buf, "123", 3) &&
          x360_r16(X, buf + 6) == 0x7E7E,
      "one short");
    /* a count of 0 writes nothing */
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snwprintf, 4, buf, 0, f, 1) == 0xFFFFFFFFu && x360_r16(X, buf) == 0x7E7E, "count 0");
    T(K(X360_xboxkrnl__snwprintf, 3, buf, 0, gwstr("")) == 0, "count 0, empty");
    /* a null buffer: the length with a count of 0, an error otherwise */
    T(K(X360_xboxkrnl__snwprintf, 4, 0, 0, f, 1234) == 4, "null buffer asks for the length");
    T(K(X360_xboxkrnl__snwprintf, 4, 0, 4, f, 1234) == 0xFFFFFFFFu, "null buffer with a count");
    T(K(X360_xboxkrnl__snwprintf, 3, buf, 4, 0) == 0xFFFFFFFFu, "null format");
    T(K(X360_xboxkrnl__scprintf, 1, 0) == 0xFFFFFFFFu, "_scprintf null format");
    T(K(X360_xboxkrnl__scwprintf, 1, 0) == 0xFFFFFFFFu, "_scwprintf null format");

    uint32_t va = gva(1, (uint64_t[]){1234});
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__vsnwprintf, 4, buf, 4, f, va) == 4 && x360_r16(X, buf + 8) == 0x7E7E, "_vsnwprintf exact");
    T(K(X360_xboxkrnl__vsnwprintf, 4, buf, 3, f, va) == 0xFFFFFFFFu && wneq(buf, "123", 3), "_vsnwprintf short");

    /* the narrow pair */
    uint32_t nf = gstr("%d");
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snprintf, 4, buf, 4, nf, 1234) == 4 && !memcmp(X->base + buf, "1234~", 5), "_snprintf exact");
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snprintf, 4, buf, 3, nf, 1234) == 0xFFFFFFFFu && !memcmp(X->base + buf, "123~", 4),
      "_snprintf short");
    T(K(X360_xboxkrnl__vsnprintf, 4, buf, 5, nf, va) == 4 && !strcmp(nstr(buf), "1234"), "_vsnprintf room");
    /* padding past the capacity is counted, not written */
    memset(X->base + buf, 0x7E, 64);
    T(K(X360_xboxkrnl__snprintf, 4, buf, 2, gstr("%100d"), 1) == 0xFFFFFFFFu && !memcmp(X->base + buf, "  ~", 3),
      "padding past the capacity");
    x360_heap_free(X, &X->heap_v4k, buf);
}

static void test_long_output(void)
{
    /* longer than gformat's 2048 bytes and 255 characters of one %s */
    enum { LONG = 6000 };
    uint32_t s = x360_heap_alloc(X, &X->heap_v4k, LONG + 1, 0), w = x360_heap_alloc(X, &X->heap_v4k, 2 * LONG + 2, 0);
    for (uint32_t k = 0; k < LONG; k++) {
        x360_w8(X, s + k, 'a' + k % 26);
        x360_w16(X, w + 2 * k, 'a' + k % 26);
    }
    uint32_t nf = gstr("[%s]"), wf = gwstr("[%s]"), nwf = gstr("[%S]");
    uint32_t va = gva(1, (uint64_t[]){s}), wva = gva(1, (uint64_t[]){w});

    T(K(X360_xboxkrnl__scprintf, 2, nf, s) == LONG + 2, "_scprintf long");
    T(K(X360_xboxkrnl__vscprintf, 2, nf, va) == LONG + 2, "_vscprintf long");
    T(K(X360_xboxkrnl__scwprintf, 2, wf, w) == LONG + 2, "_scwprintf long");
    T(K(X360_xboxkrnl__vscwprintf, 2, wf, wva) == LONG + 2, "_vscwprintf long");
    T(K(X360_xboxkrnl__scprintf, 2, gstr("%9000d"), 1) == 9000, "_scprintf wide field");

    /* the title's pattern: the count, then vsprintf into a buffer of exactly that size plus the terminator */
    uint32_t n = K(X360_xboxkrnl__vscprintf, 2, nf, va);
    uint32_t buf = gbuf(n + 2);
    T(K(X360_xboxkrnl_vsprintf, 3, buf, nf, va) == n, "vsprintf long");
    T(strlen(nstr(buf)) == n && nstr(buf)[0] == '[' && nstr(buf)[n - 1] == ']' &&
          !memcmp(nstr(buf) + 1, X->base + s, LONG),
      "vsprintf wrote %u bytes", (unsigned)strlen(nstr(buf)));
    T(x360_r8(X, buf + n + 1) == 0x7E, "vsprintf stayed in its buffer");
    T(K(X360_xboxkrnl_sprintf, 3, buf, nf, s) == n && !memcmp(nstr(buf) + 1, X->base + s, LONG), "sprintf long");
    T(K(X360_xboxkrnl_sprintf, 3, buf, nwf, w) == n && !memcmp(nstr(buf) + 1, X->base + s, LONG), "sprintf %%S long");
    x360_heap_free(X, &X->heap_v4k, buf);

    buf = gbuf(2 * (n + 2));
    T(K(X360_xboxkrnl_vswprintf, 3, buf, wf, wva) == n && x360_r16(X, buf + 2 * n) == 0 &&
          !memcmp(X->base + buf + 2, X->base + w, 2 * LONG),
      "vswprintf long");
    T(K(X360_xboxkrnl__snwprintf, 4, buf, n, wf, w) == n && x360_r16(X, buf + 2 * (n - 1)) == ']', "_snwprintf long");
    T(K(X360_xboxkrnl__snwprintf, 4, buf, n - 1, wf, w) == 0xFFFFFFFFu, "_snwprintf one short of long");
    x360_heap_free(X, &X->heap_v4k, buf);

    /* a %s just past gformat's limit, and a float wider than its work buffer */
    uint32_t cut = gbuf(300);
    memset(X->base + cut, 'q', 299);
    x360_w8(X, cut + 299, 0);
    buf = gbuf(512);
    T(K(X360_xboxkrnl_sprintf, 3, buf, gstr("%s"), cut) == 299 && strlen(nstr(buf)) == 299, "%%s of 299 characters");
    x360_heap_free(X, &X->heap_v4k, buf);
    uint64_t big[2] = {gstr("%f"), dbl(1e300)};
    T(K64(X360_xboxkrnl__scprintf, 2, big) == 308, "%%f of 1e300 is %u characters", (uint32_t)cpu()->gpr[3]);
    x360_heap_free(X, &X->heap_v4k, cut);
    x360_heap_free(X, &X->heap_v4k, s);
    x360_heap_free(X, &X->heap_v4k, w);
}

/* ---- strings ---- */

#define ST_OVERFLOW 0x80000005u
#define ST_TOO_SMALL 0xC0000023u

/* a STRING / UNICODE_STRING in guest memory */
static uint32_t gs_make(uint32_t len, uint32_t max, uint32_t buf)
{
    uint32_t a = galloc(8);
    x360_w16(X, a, len);
    x360_w16(X, a + 2, max);
    x360_w32(X, a + 4, buf);
    return a;
}

static uint32_t gs_len(uint32_t a) { return x360_r16(X, a); }
static uint32_t gs_max(uint32_t a) { return x360_r16(X, a + 2); }
static uint32_t gs_buf(uint32_t a) { return x360_r32(X, a + 4); }

/* counted strings over text that goes on past their length: nothing may read it */
static uint32_t ns(const char *s) { return gs_make((uint32_t)strlen(s), (uint32_t)strlen(s), gstr(s)); }
static uint32_t us(const char *s) { return gs_make(2 * (uint32_t)strlen(s), 2 * (uint32_t)strlen(s), gwstr(s)); }

static uint32_t gu16(const uint16_t *s, uint32_t n)
{
    uint32_t a = galloc(2 * n + 2);
    for (uint32_t k = 0; k < n; k++)
        x360_w16(X, a + 2 * k, s[k]);
    return a;
}

static int sign(uint32_t r) { return (int32_t)r < 0 ? -1 : (int32_t)r > 0; }

static void test_memory_and_case(void)
{
    uint32_t a = gstr("abcdef"), b = gstr("abcXef");
    T(K(X360_xboxkrnl_RtlCompareMemory, 3, a, b, 6) == 3, "stops at the first difference");
    T(K(X360_xboxkrnl_RtlCompareMemory, 3, a, a, 6) == 6, "all equal");
    T(K(X360_xboxkrnl_RtlCompareMemory, 3, a, b, 0) == 0, "no bytes");
    T(K(X360_xboxkrnl_RtlCompareMemory, 3, a, b, 3) == 3, "equal up to the length");

    T(K(X360_xboxkrnl_RtlUpperChar, 1, 'q') == 'Q' && K(X360_xboxkrnl_RtlUpperChar, 1, 'Q') == 'Q', "upper ASCII");
    T(K(X360_xboxkrnl_RtlUpperChar, 1, 0xE9) == 0xC9, "upper Latin-1");
    T(K(X360_xboxkrnl_RtlUpperChar, 1, 0xF7) == 0xF7, "the division sign has no case");
    T(K(X360_xboxkrnl_RtlUpperChar, 1, '{') == '{' && K(X360_xboxkrnl_RtlUpperChar, 1, '1') == '1', "upper others");
    T(K(X360_xboxkrnl_RtlLowerChar, 1, 'Q') == 'q' && K(X360_xboxkrnl_RtlLowerChar, 1, 'q') == 'q', "lower ASCII");
    T(K(X360_xboxkrnl_RtlLowerChar, 1, 0xC9) == 0xE9, "lower Latin-1");
    T(K(X360_xboxkrnl_RtlLowerChar, 1, 0xD7) == 0xD7, "the multiplication sign has no case");

    static const uint16_t pairs[][2] = {{'A', 'a'},       {0xC9, 0xE9},     {0x178, 0xFF},    {0x100, 0x101},
                                        {0x139, 0x13A},   {0x14A, 0x14B},   {0x179, 0x17A},   {0x391, 0x3B1},
                                        {0x386, 0x3AC},   {0x389, 0x3AE},   {0x38C, 0x3CC},   {0x38F, 0x3CE},
                                        {0x410, 0x430},   {0x401, 0x451},   {0xFF21, 0xFF41}};
    for (unsigned k = 0; k < sizeof pairs / sizeof pairs[0]; k++) {
        T(K(X360_xboxkrnl_RtlDowncaseUnicodeChar, 1, pairs[k][0]) == pairs[k][1], "downcase %04X", pairs[k][0]);
        T(K(X360_xboxkrnl_RtlDowncaseUnicodeChar, 1, pairs[k][1]) == pairs[k][1], "downcase keeps %04X", pairs[k][1]);
        T(K(X360_xboxkrnl_RtlUpcaseUnicodeChar, 1, pairs[k][1]) == pairs[k][0], "upcase %04X", pairs[k][1]);
        T(K(X360_xboxkrnl_RtlUpcaseUnicodeChar, 1, pairs[k][0]) == pairs[k][0], "upcase keeps %04X", pairs[k][0]);
    }
    static const uint16_t caseless[] = {'1', 0xD7, 0xF7, 0xDF, 0x130, 0x131, 0x3C2, 0x3042, 0xD834};
    for (unsigned k = 0; k < sizeof caseless / sizeof caseless[0]; k++) {
        T(K(X360_xboxkrnl_RtlDowncaseUnicodeChar, 1, caseless[k]) == caseless[k], "no lower case: %04X", caseless[k]);
        T(K(X360_xboxkrnl_RtlUpcaseUnicodeChar, 1, caseless[k]) == caseless[k], "no upper case: %04X", caseless[k]);
    }
}

static void test_compare(void)
{
#define CMP8(x, y, ci) sign(K(X360_xboxkrnl_RtlCompareString, 3, ns(x), ns(y), ci))
#define CMP16(x, y, ci) sign(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, us(x), us(y), ci))
    T(CMP8("", "", 0) == 0 && CMP16("", "", 0) == 0, "empty strings are equal");
    T(CMP8("", "a", 0) < 0 && CMP8("a", "", 0) > 0, "empty sorts first");
    T(CMP16("", "a", 0) < 0 && CMP16("a", "", 0) > 0, "empty sorts first (wide)");
    T(CMP8("abc", "abc", 0) == 0 && CMP16("abc", "abc", 0) == 0, "equal");
    T(CMP8("abc", "abd", 0) < 0 && CMP8("abd", "abc", 0) > 0, "order");
    T(CMP16("abc", "abd", 0) < 0 && CMP16("abd", "abc", 0) > 0, "order (wide)");
    T(CMP8("abc", "ab", 0) > 0 && CMP8("ab", "abc", 0) < 0, "a prefix sorts first");
    T(CMP16("abc", "ab", 0) > 0 && CMP16("ab", "abc", 0) < 0, "a prefix sorts first (wide)");
    T(CMP8("ABC", "abc", 0) < 0 && CMP8("ABC", "abc", 1) == 0, "case");
    T(CMP16("ABC", "abc", 0) < 0 && CMP16("ABC", "abc", 1) == 0, "case (wide)");
    /* 'a' folds to 'A', below '_': folding down would put it above */
    T(CMP8("a", "_", 0) > 0 && CMP8("a", "_", 1) < 0, "folds by upcasing");
    T(CMP16("a", "_", 0) > 0 && CMP16("a", "_", 1) < 0, "folds by upcasing (wide)");
    /* only the low byte of the flag counts: it is a BOOLEAN */
    T(CMP8("ABC", "abc", 0x100) < 0, "the flag is a BOOLEAN");
    T(sign(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, us("ABC"), us("abc"), 0x100)) < 0, "the flag is a BOOLEAN (wide)");
    /* bytes above 0x7F sort above ASCII */
    T(sign(K(X360_xboxkrnl_RtlCompareString, 3, ns("\xE9"), ns("z"), 0)) > 0, "unsigned bytes");
    T(sign(K(X360_xboxkrnl_RtlCompareString, 3, ns("\xE9"), ns("\xC9"), 1)) == 0, "Latin-1 folds");

    /* lengths, not terminators */
    uint32_t p = gstr("abcX"), q = gstr("abcY");
    T(K(X360_xboxkrnl_RtlCompareString, 3, gs_make(3, 4, p), gs_make(3, 4, q), 0) == 0, "the length bounds the compare");
    uint32_t wp = gwstr("abcX"), wq = gwstr("abcY");
    T(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, gs_make(6, 8, wp), gs_make(6, 8, wq), 0) == 0, "the same (wide)");

    /* characters compare as numbers, read big-endian: U+0100 is above U+00FF */
    uint16_t hi[] = {0x0100}, lo[] = {0x00FF}, ec[] = {0xC9}, es[] = {0xE9};
    T(sign(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, gs_make(2, 2, gu16(hi, 1)), gs_make(2, 2, gu16(lo, 1)), 0)) > 0,
      "big-endian characters");
    T(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, gs_make(2, 2, gu16(ec, 1)), gs_make(2, 2, gu16(es, 1)), 1) == 0,
      "non-ASCII folds");
    /* an odd byte length: the whole characters compare, then the lengths */
    T(sign(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, gs_make(3, 4, wp), gs_make(2, 4, wp), 0)) > 0, "odd length");
    T(K(X360_xboxkrnl_RtlCompareUnicodeString, 3, gs_make(3, 4, wp), gs_make(3, 4, wp), 0) == 0, "odd lengths equal");
}

static void test_copy(void)
{
    uint32_t buf = gbuf(32);
    /* room to spare: STRING gets no terminator */
    uint32_t d = gs_make(9, 16, buf);
    K(X360_xboxkrnl_RtlCopyString, 2, d, ns("abc"));
    T(gs_len(d) == 3 && !memcmp(X->base + buf, "abc~", 4), "RtlCopyString");
    /* truncated to the destination's maximum */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 2, buf);
    K(X360_xboxkrnl_RtlCopyString, 2, d, ns("abc"));
    T(gs_len(d) == 2 && !memcmp(X->base + buf, "ab~", 3), "RtlCopyString truncates");
    /* exact fit, empty, no source */
    d = gs_make(0, 3, buf);
    K(X360_xboxkrnl_RtlCopyString, 2, d, ns("xyz"));
    T(gs_len(d) == 3 && !memcmp(X->base + buf, "xyz", 3), "RtlCopyString exact fit");
    K(X360_xboxkrnl_RtlCopyString, 2, d, ns(""));
    T(gs_len(d) == 0, "RtlCopyString empty");
    d = gs_make(5, 16, buf);
    K(X360_xboxkrnl_RtlCopyString, 2, d, 0);
    T(gs_len(d) == 0, "RtlCopyString without a source");

    /* room for the terminator */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 16, buf);
    K(X360_xboxkrnl_RtlCopyUnicodeString, 2, d, us("abc"));
    T(gs_len(d) == 6 && weq(buf, "abc"), "RtlCopyUnicodeString");
    /* exact fit: no terminator past the maximum */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 6, buf);
    K(X360_xboxkrnl_RtlCopyUnicodeString, 2, d, us("abc"));
    T(gs_len(d) == 6 && wneq(buf, "abc", 3) && x360_r16(X, buf + 6) == 0x7E7E, "exact fit has no terminator");
    /* one byte of room is not room for a terminator */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 7, buf);
    K(X360_xboxkrnl_RtlCopyUnicodeString, 2, d, us("abc"));
    T(gs_len(d) == 6 && x360_r8(X, buf + 6) == 0x7E, "a spare byte stays untouched");
    /* truncated, in bytes: the odd maximum is the length */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 3, buf);
    K(X360_xboxkrnl_RtlCopyUnicodeString, 2, d, us("abc"));
    T(gs_len(d) == 3 && x360_r8(X, buf + 3) == 0x7E, "truncated to an odd maximum");
    d = gs_make(5, 16, buf);
    K(X360_xboxkrnl_RtlCopyUnicodeString, 2, d, 0);
    T(gs_len(d) == 0, "RtlCopyUnicodeString without a source");
    /* overlapping: the string moves up inside its own buffer */
    memcpy(X->base + buf, "abcdef", 6);
    K(X360_xboxkrnl_RtlCopyString, 2, gs_make(0, 6, buf + 2), gs_make(4, 4, buf));
    T(!memcmp(X->base + buf, "ababcd", 6), "overlap");
    x360_heap_free(X, &X->heap_v4k, buf);
}

static void test_append(void)
{
    uint32_t buf = gbuf(32);
    memcpy(X->base + buf, "ab", 2);
    uint32_t d = gs_make(2, 5, buf);
    T(K(X360_xboxkrnl_RtlAppendStringToString, 2, d, ns("cd")) == 0 && gs_len(d) == 4, "append");
    T(!memcmp(X->base + buf, "abcd~", 5), "append has no terminator");
    /* one byte short changes nothing */
    T(K(X360_xboxkrnl_RtlAppendStringToString, 2, d, ns("ef")) == ST_TOO_SMALL && gs_len(d) == 4, "one byte short");
    T(x360_r8(X, buf + 4) == 0x7E, "nothing written");
    /* exact fit */
    T(K(X360_xboxkrnl_RtlAppendStringToString, 2, d, ns("e")) == 0 && gs_len(d) == 5, "exact fit");
    T(K(X360_xboxkrnl_RtlAppendStringToString, 2, d, ns("")) == 0 && gs_len(d) == 5, "empty on a full string");

    /* UNICODE_STRING: terminated when there is room */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 8, buf);
    T(K(X360_xboxkrnl_RtlAppendUnicodeStringToString, 2, d, us("ab")) == 0 && gs_len(d) == 4 && weq(buf, "ab"),
      "wide append");
    T(K(X360_xboxkrnl_RtlAppendUnicodeStringToString, 2, d, us("cd")) == 0 && gs_len(d) == 8, "wide exact fit");
    T(wneq(buf, "abcd", 4) && x360_r16(X, buf + 8) == 0x7E7E, "no terminator past the maximum");
    T(K(X360_xboxkrnl_RtlAppendUnicodeStringToString, 2, d, us("e")) == ST_TOO_SMALL && gs_len(d) == 8, "wide too small");
    /* an empty source writes nothing, not even a terminator */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 8, buf);
    T(K(X360_xboxkrnl_RtlAppendUnicodeStringToString, 2, d, us("")) == 0 && x360_r16(X, buf) == 0x7E7E, "empty source");

    /* from a C string: an empty one still terminates, a null one does nothing */
    T(K(X360_xboxkrnl_RtlAppendUnicodeToString, 2, d, gwstr("")) == 0 && x360_r16(X, buf) == 0, "empty C string");
    memset(X->base + buf, 0x7E, 32);
    T(K(X360_xboxkrnl_RtlAppendUnicodeToString, 2, d, 0) == 0 && x360_r16(X, buf) == 0x7E7E, "null C string");
    T(K(X360_xboxkrnl_RtlAppendUnicodeToString, 2, d, gwstr("abc")) == 0 && gs_len(d) == 6 && weq(buf, "abc"),
      "C string");
    T(K(X360_xboxkrnl_RtlAppendUnicodeToString, 2, d, gwstr("d")) == 0 && gs_len(d) == 8, "C string exact fit");
    T(K(X360_xboxkrnl_RtlAppendUnicodeToString, 2, d, gwstr("e")) == ST_TOO_SMALL && gs_len(d) == 8, "C string too small");
    /* appended at the byte length, even an odd one */
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(3, 16, buf);
    T(K(X360_xboxkrnl_RtlAppendUnicodeStringToString, 2, d, us("a")) == 0 && gs_len(d) == 5, "odd destination length");
    T(x360_r16(X, buf + 3) == 'a', "written at byte 3");
    x360_heap_free(X, &X->heap_v4k, buf);
}

static void test_convert(void)
{
    /* allocated, terminated, freed through the pool RtlFreeAnsiString uses */
    uint32_t d = gs_make(0, 0, 0);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns("h\xE9llo"), 1) == 0, "allocate");
    T(gs_len(d) == 10 && gs_max(d) == 12 && x360_r16(X, gs_buf(d) + 2) == 0xE9 && x360_r16(X, gs_buf(d) + 10) == 0,
      "converted %u/%u", gs_len(d), gs_max(d));
    uint32_t b = gs_buf(d);
    T(x360_heap_size_of(&X->heap_v4k, b) != 0, "from the pool");
    K(X360_xboxkrnl_RtlFreeUnicodeString, 1, d);
    T(x360_heap_size_of(&X->heap_v4k, b) == 0 && !gs_len(d) && !gs_max(d) && !gs_buf(d), "freed and cleared");
    K(X360_xboxkrnl_RtlFreeUnicodeString, 1, d); /* no buffer: nothing happens */
    T(!gs_buf(d), "freeing twice");

    /* into the caller's buffer: the terminator must fit */
    uint32_t buf = gbuf(32);
    d = gs_make(0, 8, buf);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns("abc"), 0) == 0 && gs_len(d) == 6 && weq(buf, "abc"),
      "exact fit with the terminator");
    memset(X->base + buf, 0x7E, 32);
    d = gs_make(0, 6, buf);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns("abc"), 0) == ST_OVERFLOW, "no room for the terminator");
    T(x360_r16(X, buf) == 0x7E7E, "nothing written on overflow");
    d = gs_make(0, 7, buf);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns("abc"), 0) == ST_OVERFLOW, "half a terminator");
    d = gs_make(0, 2, buf);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns(""), 0) == 0 && !gs_len(d) && !x360_r16(X, buf), "empty");
    d = gs_make(0, 0, buf);
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, ns(""), 0) == ST_OVERFLOW, "empty with no room");
    /* too long for a UNICODE_STRING */
    T(K(X360_xboxkrnl_RtlAnsiStringToUnicodeString, 3, d, gs_make(0x8000, 0x8000, buf), 1) == 0xC00000F0u, "too long");
    x360_heap_free(X, &X->heap_v4k, buf);

    /* RtlCreateUnicodeString: a BOOLEAN */
    d = gs_make(0, 0, 0);
    T(K(X360_xboxkrnl_RtlCreateUnicodeString, 2, d, gwstr("abc")) == 1, "create");
    T(gs_len(d) == 6 && gs_max(d) == 8 && weq(gs_buf(d), "abc"), "created");
    b = gs_buf(d);
    K(X360_xboxkrnl_RtlFreeUnicodeString, 1, d);
    T(x360_heap_size_of(&X->heap_v4k, b) == 0, "created from the pool");
    T(K(X360_xboxkrnl_RtlCreateUnicodeString, 2, d, gwstr("")) == 1 && gs_len(d) == 0 && gs_max(d) == 2, "create empty");
    K(X360_xboxkrnl_RtlFreeUnicodeString, 1, d);
    T(K(X360_xboxkrnl_RtlCreateUnicodeString, 2, d, 0) == 0, "create from nothing");
    uint32_t huge = x360_heap_alloc(X, &X->heap_v4k, 0x10002, 0);
    for (uint32_t k = 0; k < 0x8000; k++)
        x360_w16(X, huge + 2 * k, 'x');
    T(K(X360_xboxkrnl_RtlCreateUnicodeString, 2, d, huge) == 0, "too long to create");
    x360_heap_free(X, &X->heap_v4k, huge);

    /* the sizes, one byte per ANSI character */
    uint32_t out = galloc(4);
    T(K(X360_xboxkrnl_RtlMultiByteToUnicodeSize, 3, out, gstr("abc"), 3) == 0 && x360_r32(X, out) == 6, "to Unicode");
    T(K(X360_xboxkrnl_RtlMultiByteToUnicodeSize, 3, out, gstr(""), 0) == 0 && x360_r32(X, out) == 0, "to Unicode, none");
    T(K(X360_xboxkrnl_RtlUnicodeToMultiByteSize, 3, out, gwstr("abc"), 6) == 0 && x360_r32(X, out) == 3, "to ANSI");
    T(K(X360_xboxkrnl_RtlUnicodeToMultiByteSize, 3, out, gwstr("abc"), 5) == 0 && x360_r32(X, out) == 2, "odd bytes");
}

static void test_utf8(void)
{
    /* 1, 2, 3 bytes and a surrogate pair (4 bytes) */
    static const uint16_t mix[] = {'A', 0xE9, 0x3042, 0xD834, 0xDD1E};
    static const uint8_t want[] = {'A', 0xC3, 0xA9, 0xE3, 0x81, 0x82, 0xF0, 0x9D, 0x84, 0x9E};
    uint32_t src = gu16(mix, 5), out = galloc(4), buf = gbuf(32);

    T(K(X360_xboxkrnl_RtlUnicodeToUtf8Size, 3, out, src, 10) == 0 && x360_r32(X, out) == 10, "size");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8Size, 3, out, src, 0) == 0 && x360_r32(X, out) == 0, "size of nothing");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8Size, 3, out, src, 11) == 0 && x360_r32(X, out) == 10, "an odd byte is not counted");

    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, src, 10) == 0 && x360_r32(X, out) == 10, "convert");
    T(!memcmp(X->base + buf, want, 10) && x360_r8(X, buf + 10) == 0x7E, "the bytes, no terminator");
    /* exact fit */
    memset(X->base + buf, 0x7E, 32);
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 10, out, src, 10) == 0 && x360_r32(X, out) == 10, "exact fit");
    /* one byte short: the pair is not split */
    memset(X->base + buf, 0x7E, 32);
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 9, out, src, 10) == ST_TOO_SMALL && x360_r32(X, out) == 6, "one short");
    T(!memcmp(X->base + buf, want, 6) && x360_r8(X, buf + 6) == 0x7E, "whole characters only");
    /* two bytes of a 3-byte sequence do not fit */
    memset(X->base + buf, 0x7E, 32);
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 5, out, src, 10) == ST_TOO_SMALL && x360_r32(X, out) == 3, "3-byte cut");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 0, out, src, 10) == ST_TOO_SMALL && x360_r32(X, out) == 0, "no room");
    /* no destination asks for the size */
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, 0, 0, out, src, 10) == 0 && x360_r32(X, out) == 10, "size query");
    /* empty */
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, src, 0) == 0 && x360_r32(X, out) == 0, "empty");
    /* bad arguments */
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, 0, 10) == 0xC00000F2u, "no source");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, 0, src, 10) == 0xC000000Du, "no count");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, src, 9) == 0xC00000F3u, "odd source");

    /* unpaired surrogates become U+FFFD and say so */
    static const uint16_t lone_hi[] = {0xD834, 'A'}, lone_lo[] = {0xDD1E}, end_hi[] = {'A', 0xD834};
    memset(X->base + buf, 0x7E, 32);
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, gu16(lone_hi, 2), 4) == 0x107 && x360_r32(X, out) == 4, "lone high");
    T(!memcmp(X->base + buf, "\xEF\xBF\xBD" "A", 4), "replaced");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, gu16(lone_lo, 1), 2) == 0x107 && x360_r32(X, out) == 3, "lone low");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, gu16(end_hi, 2), 4) == 0x107 && x360_r32(X, out) == 4, "high at the end");
    /* a pair cut by the byte count is unpaired */
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, gu16(mix + 3, 2), 2) == 0x107 && x360_r32(X, out) == 3, "cut pair");
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8Size, 3, out, gu16(lone_lo, 1), 2) == 0 && x360_r32(X, out) == 3, "lone size");
    /* the last code point of each length */
    static const uint16_t edges[] = {0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0xDBFF, 0xDFFF};
    static const uint8_t edge_bytes[] = {0x7F, 0xC2, 0x80, 0xDF, 0xBF, 0xE0, 0xA0, 0x80,
                                         0xEF, 0xBF, 0xBF, 0xF4, 0x8F, 0xBF, 0xBF};
    T(K(X360_xboxkrnl_RtlUnicodeToUtf8, 5, buf, 32, out, gu16(edges, 7), 14) == 0 && x360_r32(X, out) == 15, "edges");
    T(!memcmp(X->base + buf, edge_bytes, 15), "edge bytes");
    x360_heap_free(X, &X->heap_v4k, buf);
}

/* ---- image and stack: a small PE in guest memory, three functions A -> B -> C with real prologs ---- */

enum { IMG_SIZE = 0x4000, RVA_A = 0x1000, RVA_B = 0x1100, RVA_C = 0x1200, RVA_PDATA = 0x2000, FN_WORDS = 16 };
enum { OPT = 0x98, SECTIONS = OPT + 0xE0 };

static uint32_t img;

static void le32_put(uint32_t a, uint32_t v)
{
    for (int k = 0; k < 4; k++)
        x360_w8(X, a + (uint32_t)k, (v >> (8 * k)) & 0xFF);
}

static void le16_put(uint32_t a, uint32_t v)
{
    x360_w8(X, a, v & 0xFF);
    x360_w8(X, a + 1, v >> 8);
}

static void section(unsigned k, const char *name, uint32_t va, uint32_t raw_size, uint32_t raw_ptr)
{
    uint32_t s = img + SECTIONS + 40 * k;
    memcpy(X->base + s, name, strlen(name));
    le32_put(s + 8, raw_size);
    le32_put(s + 12, va);
    le32_put(s + 16, raw_size);
    le32_put(s + 20, raw_ptr);
}

static void function(uint32_t rva, unsigned index)
{
    static const uint32_t prolog[4] = {
        0x7D8802A6u, /* mflr r12 */
        0x9181FFF8u, /* stw r12, -8(r1) */
        0xFBE1FFF0u, /* std r31, -16(r1) */
        0x9421FFA0u, /* stwu r1, -0x60(r1) */
    };
    for (uint32_t k = 0; k < FN_WORDS; k++)
        x360_w32(X, img + rva + 4 * k, k < 4 ? prolog[k] : 0x60000000u);
    x360_w32(X, img + RVA_PDATA + 8 * index, img + rva);
    x360_w32(X, img + RVA_PDATA + 8 * index + 4, 4u | FN_WORDS << 8);
}

static void image_build(void)
{
    img = x360_heap_alloc(X, &X->heap_v4k, IMG_SIZE, 0);
    x360_w8(X, img, 'M'), x360_w8(X, img + 1, 'Z');
    le32_put(img + 0x3C, 0x80);
    memcpy(X->base + img + 0x80, "PE\0\0", 4);
    le16_put(img + 0x86, 2);    /* sections */
    le16_put(img + 0x94, 0xE0); /* optional header size */
    le16_put(img + OPT, 0x10B);
    le32_put(img + OPT + 60, 0x400); /* SizeOfHeaders */
    le32_put(img + OPT + 92, 16);    /* directories */
    le32_put(img + OPT + 96 + 8 * 1, 0x300), le32_put(img + OPT + 96 + 8 * 1 + 4, 8);        /* inside the headers */
    le32_put(img + OPT + 96 + 8 * 3, RVA_PDATA), le32_put(img + OPT + 96 + 8 * 3 + 4, 24);   /* .pdata */
    le32_put(img + OPT + 96 + 8 * 4, 0x3800), le32_put(img + OPT + 96 + 8 * 4 + 4, 4);       /* in no section */
    section(0, ".text", 0x1000, 0x1000, 0x400);
    section(1, ".pdata", RVA_PDATA, 0x200, 0x1400);
    function(RVA_A, 0);
    function(RVA_B, 1);
    function(RVA_C, 2);
    X->image_base = img;
    X->image_size = IMG_SIZE;
    X->pdata_scanned = 0;
}

static void test_image(void)
{
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, img) == img + 0x80, "NT headers");
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, X->module) == img + 0x80, "NT headers from the module handle");
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, 0) == 0, "no image");
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, gstr("not an image")) == 0, "no MZ");
    le32_put(img + 0x3C, 0x7FFFFFFFu);
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, img) == 0, "headers out of reach");
    le32_put(img + 0x3C, 0x90);
    T(K(X360_xboxkrnl_RtlImageNtHeader, 1, img) == 0, "no PE signature");
    le32_put(img + 0x3C, 0x80);

#define DIR(base, mapped, entry) K(X360_xboxkrnl_RtlImageDirectoryEntryToData, 4, base, mapped, entry, size)
    uint32_t size = galloc(4);
    T(DIR(img, 1, 3) == img + RVA_PDATA && x360_r32(X, size) == 24, "mapped");
    T(DIR(X->module, 1, 3) == img + RVA_PDATA, "by module handle");
    T(DIR(img, 0, 3) == img + 0x1400, "as a file: through the section table");
    T(DIR(img | 1, 1, 3) == img + 0x1400, "bit 0 of the base means a file");
    T(DIR(img, 0, 1) == img + 0x300 && x360_r32(X, size) == 8, "inside the headers");
    x360_w32(X, size, 0x55);
    T(DIR(img, 1, 2) == 0 && x360_r32(X, size) == 0x55, "an empty directory");
    T(DIR(img, 1, 16) == 0 && x360_r32(X, size) == 0x55, "past the directories");
    T(DIR(img, 0, 4) == 0, "an address in no section");
    T(DIR(0, 1, 3) == 0, "no image");
    T(K(X360_xboxkrnl_RtlImageDirectoryEntryToData, 4, img, 1, 3, 0) == img + RVA_PDATA, "no size wanted");
    le16_put(img + OPT, 0x20B); /* PE32+: the directories sit 16 bytes further */
    le32_put(img + OPT + 108, 16);
    le32_put(img + OPT + 112 + 8 * 3, 0x2100);
    T(DIR(img, 1, 3) == img + 0x2100, "PE32+");
    le16_put(img + OPT, 0x999);
    T(DIR(img, 1, 3) == 0, "an unknown optional header");
    le16_put(img + OPT, 0x10B);
    le32_put(img + OPT + 92, 16);
    le32_put(img + OPT + 96 + 8 * 3, RVA_PDATA);

    uint32_t out = galloc(4);
    T(K(X360_xboxkrnl_XexPcToFileHeader, 2, img + RVA_B, out) == img && x360_r32(X, out) == X->module, "pc in the image");
    T(K(X360_xboxkrnl_XexPcToFileHeader, 2, img, out) == img, "its first byte");
    T(K(X360_xboxkrnl_XexPcToFileHeader, 2, img + IMG_SIZE - 1, 0) == img, "its last byte, no out-pointer");
    T(K(X360_xboxkrnl_XexPcToFileHeader, 2, img + IMG_SIZE, out) == 0 && x360_r32(X, out) == 0, "just past it");
    T(K(X360_xboxkrnl_XexPcToFileHeader, 2, img - 1, out) == 0, "just before it");

    x360_w32(X, out, 0x1234);
    T(K(X360_xboxkrnl_XexLoadImage, 4, gstr("other.xex"), 8, 0, out) == 0xC0000034u && x360_r32(X, out) == 0, "XexLoadImage");
    T(K(X360_xboxkrnl_XexLoadImage, 4, gstr("other.xex"), 8, 0, 0) == 0xC0000034u, "XexLoadImage without an out-pointer");
    T(K(X360_xboxkrnl_XexUnloadImage, 1, X->module) == 0, "the title's own handle");
    T(K(X360_xboxkrnl_XexUnloadImage, 1, 0x1234) == 0xC0000008u, "an unknown handle");
    T(K(X360_xboxkrnl_XexUnloadImage, 1, 0) == 0xC0000008u, "no handle");
}

static void test_crc(void)
{
    uint32_t s = gstr("123456789");
    T(K(X360_xboxkrnl_RtlComputeCrc32, 3, 0, s, 9) == 0xCBF43926u, "the check value");
    T(K(X360_xboxkrnl_RtlComputeCrc32, 3, 0x1234, s, 0) == 0x1234, "no bytes keep the seed");
    uint32_t part = K(X360_xboxkrnl_RtlComputeCrc32, 3, 0, s, 4);
    T(K(X360_xboxkrnl_RtlComputeCrc32, 3, part, s + 4, 5) == 0xCBF43926u, "chained");
    T(x360_crc32(X->base + s, 9) == 0xCBF43926u, "agrees with the loader's CRC");
}

/* frames C (at stack_top), B and A above it; a return address where each prolog stored it */
#define RET_B (img + RVA_B + 0x20)
#define RET_A (img + RVA_A + 0x20)
#define PC_C (img + RVA_C + 0x20)

static void frames_build(void)
{
    uint32_t c = stack_top, b = c + 0x60, a = b + 0x60, top = a + 0x60;
    x360_w32(X, c, b), x360_w32(X, b, a), x360_w32(X, a, top), x360_w32(X, top, 0);
    x360_w32(X, b - 8, RET_B), x360_w32(X, a - 8, RET_A), x360_w32(X, top - 8, X360_THREAD_EXIT);
    x360_w64(X, b - 16, 0x1111222233334444ull); /* B's r31, saved by C */
    cpu()->pc = PC_C;
}

static void test_unwind(void)
{
    x360_thread *th = x360_cur(X);
    uint32_t low = th->stack_limit, high = th->stack_base;
    th->stack_limit = scratch, th->stack_base = scratch + 0x10000;
    frames_build();

    /* .pdata */
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, PC_C) == img + RVA_PDATA + 16, "the function of a pc");
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, img + RVA_A) == img + RVA_PDATA, "its first instruction");
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, img + RVA_C + 4 * FN_WORDS - 4) == img + RVA_PDATA + 16, "its last");
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, img + RVA_C + 4 * FN_WORDS) == 0, "just past it");
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, img + RVA_A - 4) == 0, "before the first function");
    T(K(X360_xboxkrnl_RtlLookupFunctionEntry, 1, 0x10) == 0, "outside the image");

    /* one frame up, in the body of C */
    uint32_t rec = galloc(0x400), in_fn = galloc(4), est = galloc(4), entry = img + RVA_PDATA + 16;
    x360_seh_ctx k;
    memset(&k, 0, sizeof k);
    k.gpr[1] = stack_top, k.gpr[31] = 0x99, k.gpr[14] = 0x77, k.pc = PC_C, k.lr = 0x1;
    x360_seh_write_context(X, rec, &k);
    T(K(X360_xboxkrnl_RtlVirtualUnwind, 8, PC_C, entry, rec, in_fn, est, 0, 0, 0) == RET_B, "the caller's pc");
    x360_seh_ctx up;
    x360_seh_read_context(X, rec, &up);
    T(up.pc == RET_B && up.lr == RET_B, "its pc and lr in the record");
    T(up.gpr[1] == stack_top + 0x60, "its stack pointer: %08X", (uint32_t)up.gpr[1]);
    T(up.gpr[31] == 0x1111222233334444ull, "the register the prolog saved");
    T(up.gpr[14] == 0x77, "a register it did not save");
    T(x360_r32(X, est) == stack_top, "the establisher frame");
    T(x360_r8(X, in_fn) == 1, "past the prolog");

    /* the same with the entry looked up, and no out-pointers */
    x360_seh_write_context(X, rec, &k);
    T(K(X360_xboxkrnl_RtlVirtualUnwind, 8, PC_C, 0, rec, 0, 0, 0, 0, 0) == RET_B, "the entry found from the pc");

    /* at the function's first instruction nothing is saved yet: the caller is in LR */
    k.pc = img + RVA_C, k.lr = RET_B;
    x360_seh_write_context(X, rec, &k);
    T(K(X360_xboxkrnl_RtlVirtualUnwind, 8, img + RVA_C, entry, rec, in_fn, est, 0, 0, 0) == RET_B, "at the entry");
    x360_seh_read_context(X, rec, &up);
    T(up.gpr[1] == stack_top && up.gpr[31] == 0x99, "the frame is not allocated yet");
    T(x360_r8(X, in_fn) == 0, "inside the prolog");

    /* no function: a leaf, the caller in LR */
    k.pc = 0x10, k.lr = RET_A;
    x360_seh_write_context(X, rec, &k);
    T(K(X360_xboxkrnl_RtlVirtualUnwind, 8, 0x10, 0, rec, in_fn, est, 0, 0, 0) == RET_A, "a leaf");
    T(x360_r8(X, in_fn) == 0, "a leaf is in no function");

    /* the whole chain */
    uint32_t arr = galloc(32), hash = galloc(4);
#define TRACE(skip, want, h) K(X360_xboxkrnl_RtlCaptureStackBackTrace, 4, skip, want, arr, h)
    T(TRACE(0, 8, hash) == 3, "three frames");
    T(x360_r32(X, arr) == PC_C && x360_r32(X, arr + 4) == RET_B && x360_r32(X, arr + 8) == RET_A, "their addresses");
    T(x360_r32(X, hash) == PC_C + RET_B + RET_A, "the hash");
    T(TRACE(1, 8, hash) == 2 && x360_r32(X, arr) == RET_B && x360_r32(X, hash) == RET_B + RET_A, "one skipped");
    x360_w32(X, arr + 4, 0x55);
    T(TRACE(0, 1, 0) == 1 && x360_r32(X, arr) == PC_C && x360_r32(X, arr + 4) == 0x55, "the array's size is kept");
    T(TRACE(3, 8, hash) == 0 && x360_r32(X, hash) == 0, "all skipped");
    T(TRACE(0, 0, hash) == 0, "none wanted");

    uint32_t a = galloc(4), b = galloc(4);
    K(X360_xboxkrnl_RtlGetCallersAddress, 2, a, b);
    T(x360_r32(X, a) == RET_B && x360_r32(X, b) == RET_A, "the callers: %08X %08X", x360_r32(X, a), x360_r32(X, b));
    K(X360_xboxkrnl_RtlGetCallersAddress, 2, 0, b);
    T(x360_r32(X, b) == RET_A, "only the second");

    /* a chain that leaves the stack ends there */
    x360_w32(X, stack_top + 0x60, 0x10);
    T(TRACE(0, 8, 0) == 2, "a back chain off the stack");
    x360_w32(X, stack_top + 0x60, stack_top + 0xC0);
    /* a pc outside the image has no frames */
    cpu()->pc = 0x10;
    T(TRACE(0, 8, hash) == 0 && x360_r32(X, hash) == 0, "no frames outside the image");
    K(X360_xboxkrnl_RtlGetCallersAddress, 2, a, b);
    T(x360_r32(X, a) == 0 && x360_r32(X, b) == 0, "no callers outside the image");

    th->stack_limit = low, th->stack_base = high;
    K(X360_xboxkrnl_RtlGetStackLimits, 2, a, b);
    T(x360_r32(X, a) == low && x360_r32(X, b) == high && low < high, "the stack limits");
    K(X360_xboxkrnl_RtlGetStackLimits, 2, 0, 0);
}

/* last: with no handler in this bare machine the raise ends the run */
static void test_raise(void)
{
    uint64_t raised = X->st.seh_raised;
    cpu()->pc = PC_C;
    K(X360_xboxkrnl_RtlRaiseStatus, 1, 0xC0000123u);
    uint32_t rec = (stack_top - 0x60) & ~7u;
    T(X->st.seh_raised == raised + 1, "dispatched");
    T(x360_r32(X, rec) == 0xC0000123u, "the code in the record");
    T(x360_r32(X, rec + 4) == 1, "noncontinuable");
    T(x360_r32(X, rec + 8) == 0 && x360_r32(X, rec + 16) == 0, "no chained record, no parameters");
    T(x360_r32(X, rec + 12) == PC_C, "the address");
    T(X->status == X360_ERROR && strstr(X->error, "C0000123"), "unhandled: \"%s\"", X->error);
    T(x360_cur(X)->seh.depth == 0, "the dispatch ended");
    X->status = 0;
}

int main(void)
{
    if (hle_util_start())
        return 1;
    image_build();
    test_image();
    test_crc();
    test_unwind();
    test_conversions();
    test_truncation();
    test_long_output();
    test_memory_and_case();
    test_compare();
    test_copy();
    test_append();
    test_convert();
    test_utf8();
    test_raise();
    printf("x360_rtl: %d checks\n", checks);
    return hle_util_end("x360_rtl");
}
