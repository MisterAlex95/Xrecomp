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

int main(void)
{
    if (hle_util_start())
        return 1;
    test_conversions();
    test_truncation();
    test_long_output();
    printf("x360_rtl: %d checks\n", checks);
    return hle_util_end("x360_rtl");
}
