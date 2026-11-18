/* x360_crypt2_test -- the crypt2 HLE group called directly (XBA-296). Every expected value below is a published
 * known answer, the source named beside it; none comes from this code. */
#include "x360_hle_util.h"

#define GUARD 0xA5
static int checks;

#define OK(cond, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        CHECK(cond, __VA_ARGS__);                                                                                      \
    } while (0)

/* hex text to bytes; returns the length */
static uint32_t unhex(const char *s, uint8_t *out)
{
    uint32_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0)
            continue;
        if (hi < 0)
            hi = v;
        else
            out[n++] = (uint8_t)(hi << 4 | v), hi = -1;
    }
    return n;
}

/* guest copy of a host buffer */
static uint32_t gbytes(const void *p, uint32_t n)
{
    uint32_t a = galloc(n ? n : 1);
    memcpy(X->base + a, p, n);
    return a;
}

static uint32_t ghex(const char *s, uint32_t *n)
{
    static uint8_t tmp[4096];
    *n = unhex(s, tmp);
    return gbytes(tmp, *n);
}

/* a guest block of `size` bytes followed by 16 guard bytes */
static uint32_t gstate(uint32_t size)
{
    uint32_t a = galloc(size + 16);
    memset(X->base + a + size, GUARD, 16);
    return a;
}

static int guard_ok(uint32_t a, uint32_t size)
{
    for (uint32_t i = 0; i < 16; i++)
        if (X->base[a + size + i] != GUARD)
            return 0;
    return 1;
}

static void expect_hex(const char *what, uint32_t a, const char *hex)
{
    static uint8_t want[4096];
    uint32_t n = unhex(hex, want);
    OK(!memcmp(X->base + a, want, n), "%s differs from the published value", what);
}

#define KX(name, n, ...) K(X360_xboxkrnl_##name, n, __VA_ARGS__)

/* ---- hashes ---- */

typedef struct {
    unsigned init, update, final, one_shot;
    uint32_t state, digest;
    const char *name;
} hash_api;

#define API(N, st, dg) {X360_xboxkrnl_##N##Init, X360_xboxkrnl_##N##Update, X360_xboxkrnl_##N##Final, X360_xboxkrnl_##N, st, dg, #N}
static const hash_api MD5 = API(XeCryptMd5, 0x54, 16);
static const hash_api SHA256 = API(XeCryptSha256, 0x64, 32);
static const hash_api SHA384 = API(XeCryptSha384, 0xC8, 48);
static const hash_api SHA512 = API(XeCryptSha512, 0xC8, 64);

/* the message whole, then split over Update calls at awkward lengths, with the state moved to another guest address
 * between two calls: only what is in guest memory can survive that, as in a save state */
static void hash_vector(const hash_api *h, const char *msg, uint32_t len, const char *want)
{
    uint32_t in = gbytes(msg, len), out = gstate(h->digest);

    K(h->one_shot, 8, in, len, 0, 0, 0, 0, out, h->digest);
    expect_hex(h->name, out, want);
    OK(guard_ok(out, h->digest), "%s: wrote past the digest", h->name);

    if (len > 3) { /* the three inputs of the one-shot call are one message */
        memset(X->base + out, 0, h->digest);
        K(h->one_shot, 8, in, 1u, in + 1, 2u, in + 3, len - 3, out, h->digest);
        expect_hex(h->name, out, want);
    }

    static const uint32_t cuts[] = {1, 3, 55, 56, 57, 63, 64, 65, 111, 112, 119, 127, 128, 129};
    uint32_t st = gstate(h->state), st2 = gstate(h->state);
    K(h->init, 1, st);
    uint32_t at = 0;
    for (unsigned k = 0; at < len; k++) {
        uint32_t take = cuts[k % (sizeof cuts / sizeof cuts[0])];
        if (take > len - at)
            take = len - at;
        K(h->update, 3, st, in + at, take);
        at += take;
        memcpy(X->base + st2, X->base + st, h->state); /* the state moves: nothing may be remembered by address */
        memset(X->base + st, 0xEE, h->state);
        uint32_t tmp = st;
        st = st2, st2 = tmp;
    }
    memset(X->base + out, 0, h->digest);
    K(h->final, 3, st, out, h->digest);
    expect_hex(h->name, out, want);
    OK(guard_ok(st, h->state) && guard_ok(st2, h->state), "%s: wrote past the state", h->name);

    /* a short output buffer takes the digest's first bytes only */
    uint32_t sh = gstate(4);
    K(h->one_shot, 8, in, len, 0, 0, 0, 0, sh, 4u);
    OK(guard_ok(sh, 4), "%s: wrote past a short output", h->name);
}

static void test_md5(void)
{
    /* RFC 1321 appendix A.5 */
    hash_vector(&MD5, "", 0, "d41d8cd98f00b204e9800998ecf8427e");
    hash_vector(&MD5, "a", 1, "0cc175b9c0f1b6a831c399e269772661");
    hash_vector(&MD5, "abc", 3, "900150983cd24fb0d6963f7d28e17f72");
    hash_vector(&MD5, "message digest", 14, "f96b697d7cb7938d525a2f31aaf161d0");
    hash_vector(&MD5, "abcdefghijklmnopqrstuvwxyz", 26, "c3fcd3d76192e4007dfb496cca67e13b");
    hash_vector(&MD5, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62,
                "d174ab98d277d9f5a5611c2c9f419d9f");
    hash_vector(&MD5, "12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80,
                "57edf4a22be3c955ac49da2e2107b67a");
}

static const char ABC448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char ABC896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                             "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";

static void test_sha2(void)
{
    /* the FIPS 180-4 example messages, digests as printed in RFC 6234 section 8.5 (TEST1, TEST2_1, TEST2_2) */
    hash_vector(&SHA256, "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hash_vector(&SHA256, ABC448, 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    hash_vector(&SHA384, "abc", 3,
                "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    hash_vector(&SHA384, ABC896, 112,
                "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746039");
    hash_vector(&SHA512, "abc", 3,
                "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    hash_vector(&SHA512, ABC896, 112,
                "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");

    /* SHA-224: the kernel's Init, then the SHA-256 Update and Final x360_crypt.c answers */
    static const struct {
        const char *msg, *want;
    } v224[] = {{"abc", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7"},
                {ABC448, "75388b16512776cc5dba5da1fd890150b0c6455cb4f58b1952522525"}};
    for (unsigned k = 0; k < 2; k++) {
        uint32_t st = gstate(0x64), out = gstate(28), n = (uint32_t)strlen(v224[k].msg), in = gbytes(v224[k].msg, n);
        KX(XeCryptSha224Init, 1, st);
        KX(XeCryptSha256Update, 3, st, in, n);
        KX(XeCryptSha256Final, 3, st, out, 28u);
        expect_hex("SHA-224", out, v224[k].want);
        OK(guard_ok(out, 28) && guard_ok(st, 0x64), "SHA-224: wrote past a buffer");
    }

    /* the incremental SHA-256 of x360_crypt.c and the one-shot here are one hash */
    hash_vector(&SHA256, ABC896, 112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

/* ---- HMAC ---- */

typedef struct {
    const char *key, *data, *md5, *sha1;
} hmac_vec;

/* RFC 2202 sections 2 and 3. Keys and data given as hex; the two test sets differ in the key of cases 1, 3 and 5. */
#define AA20 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define AA80 AA20 AA20 AA20 AA20
#define DD50 "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define CD50 "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"
#define HI "4869205468657265"                                                  /* "Hi There" */
#define JEFE "4a656665"                                                        /* "Jefe" */
#define WHAT "7768617420646f2079612077616e7420666f72206e6f7468696e673f"        /* "what do ya want for nothing?" */
#define TRUNC "546573742057697468205472756e636174696f6e"                       /* "Test With Truncation" */
#define K0102 "0102030405060708090a0b0c0d0e0f10111213141516171819"
#define LONG1                                                                                                          \
    "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374"
#define LONG2                                                                                                          \
    "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b657920616e64204c6172676572205468616e20"     \
    "4f6e6520426c6f636b2d53697a652044617461"

static void hmac_md5_case(const char *key, const char *data, const char *want)
{
    uint32_t ks, ds, k = ghex(key, &ks), d = ghex(data, &ds), out = gstate(16), st = gstate(0xA8);

    KX(XeCryptHmacMd5, 10, k, ks, d, ds, 0, 0, 0, 0, out, 16u);
    expect_hex("HMAC-MD5 one shot", out, want);
    OK(guard_ok(out, 16), "HMAC-MD5: wrote past the MAC");

    memset(X->base + out, 0, 16);
    KX(XeCryptHmacMd5Init, 3, st, k, ks);
    KX(XeCryptHmacMd5Update, 3, st, d, 5u);
    KX(XeCryptHmacMd5Update, 3, st, d + 5, ds - 5);
    KX(XeCryptHmacMd5Final, 3, st, out, 16u);
    expect_hex("HMAC-MD5 incremental", out, want);
    OK(guard_ok(st, 0xA8), "HMAC-MD5: wrote past the state");
}

static void hmac_sha_case(const char *key, const char *data, const char *want)
{
    uint32_t ks, ds, k = ghex(key, &ks), d = ghex(data, &ds), out = gstate(20), st = gstate(0xB0), st2 = gstate(0xB0);

    KX(XeCryptHmacSha, 10, k, ks, d, ds, 0, 0, 0, 0, out, 20u);
    expect_hex("HMAC-SHA1 one shot (x360_crypt.c)", out, want);

    memset(X->base + out, 0, 20);
    KX(XeCryptHmacShaInit, 3, st, k, ks);
    KX(XeCryptHmacShaUpdate, 3, st, d, 5u);
    memcpy(X->base + st2, X->base + st, 0xB0); /* a save state in the middle of the MAC */
    memset(X->base + st, 0xEE, 0xB0);
    KX(XeCryptHmacShaUpdate, 3, st2, d + 5, ds - 5);
    KX(XeCryptHmacShaFinal, 3, st2, out, 20u);
    expect_hex("HMAC-SHA1 incremental", out, want);
    OK(guard_ok(st, 0xB0) && guard_ok(st2, 0xB0), "HMAC-SHA1: wrote past the state");

    /* Verify: the whole MAC, a truncated one, a wrong one, one longer than a digest */
    uint32_t mac = gbytes(X->base + out, 20);
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 20u) == 1, "HmacShaVerify refused the right MAC");
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 12u) == 1, "HmacShaVerify refused a truncated MAC");
    X->base[mac + 11] ^= 1;
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 12u) == 0, "HmacShaVerify accepted a wrong MAC");
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 21u) == 0, "HmacShaVerify accepted 21 bytes");
}

static void test_hmac(void)
{
    /* RFC 2202 section 2 */
    hmac_md5_case("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", HI, "9294727a3638bb1c13f48ef8158bfc9d");
    hmac_md5_case(JEFE, WHAT, "750c783e6ab0b503eaa86e310a5db738");
    hmac_md5_case("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", DD50, "56be34521d144c88dbb8c733f0e8b3f6");
    hmac_md5_case(K0102, CD50, "697eaf0aca3a3aea3a75164746ffaa79");
    hmac_md5_case("0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c", TRUNC, "56461ef2342edc00f9bab995690efd4c");
    hmac_md5_case(AA80, LONG1, "6b1ab7fe4bd7bf8f0b62e6ce61b9d0cd");
    hmac_md5_case(AA80, LONG2, "6f630fad67cda0ee1fb1f562db3aa53e");
    /* RFC 2202 section 3 */
    hmac_sha_case("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", HI, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_sha_case(JEFE, WHAT, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    hmac_sha_case("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", DD50, "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
    hmac_sha_case(K0102, CD50, "4c9007f4026250c6bc8414f9bf50c86c2d7235da");
    hmac_sha_case("0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c", TRUNC, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");
    hmac_sha_case(AA80, LONG1, "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    hmac_sha_case(AA80, LONG2, "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
}

int main(void)
{
    if (hle_util_start())
        return 1;
    test_md5();
    test_sha2();
    test_hmac();
    printf("x360_crypt2: %d checks\n", checks);
    return hle_util_end("x360_crypt2");
}
