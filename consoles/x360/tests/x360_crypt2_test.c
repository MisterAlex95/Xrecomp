/* x360_crypt2_test -- the crypt2 HLE group called directly (XBA-296). Every hash and cipher is checked against a
 * published known answer, the source named beside it. The big-number values come from Python's integers; Parve and the
 * chain-and-sum MAC have no published vectors and are checked against what their definition gives by hand. No expected
 * value comes from this code. */
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
    memcpy(x360_p(X, a), p, n);
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
    memset(x360_p(X, a + size), GUARD, 16);
    return a;
}

static int guard_ok(uint32_t a, uint32_t size)
{
    for (uint32_t i = 0; i < 16; i++)
        if (x360_p(X, a + size + i)[0] != GUARD)
            return 0;
    return 1;
}

static void expect_hex(const char *what, uint32_t a, const char *hex)
{
    static uint8_t want[4096];
    uint32_t n = unhex(hex, want);
    OK(!memcmp(x360_p(X, a), want, n), "%s differs from the published value", what);
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
        memset(x360_p(X, out), 0, h->digest);
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
        memcpy(x360_p(X, st2), x360_p(X, st), h->state); /* the state moves: nothing may be remembered by address */
        memset(x360_p(X, st), 0xEE, h->state);
        uint32_t tmp = st;
        st = st2, st2 = tmp;
    }
    memset(x360_p(X, out), 0, h->digest);
    K(h->final, 3, st, out, h->digest);
    expect_hex(h->name, out, want);
    OK(guard_ok(st, h->state) && guard_ok(st2, h->state), "%s: wrote past the state", h->name);

    /* a short output buffer takes the digest's first bytes only */
    uint32_t sh = gstate(4);
    K(h->one_shot, 8, in, len, 0, 0, 0, 0, sh, 4u);
    OK(guard_ok(sh, 4), "%s: wrote past a short output", h->name);
}

/* a million 'a' from one small buffer, at awkward lengths, the state moving to another address after every call */
static void hash_million(const hash_api *h, const char *want)
{
    static const uint32_t cuts[] = {1, 3, 55, 56, 57, 63, 64, 65, 111, 112, 119, 127, 128, 129, 1000};
    uint32_t in = galloc(1000), out = gstate(h->digest), st = gstate(h->state), st2 = gstate(h->state);
    memset(x360_p(X, in), 'a', 1000);
    K(h->init, 1, st);
    uint32_t left = 1000000;
    for (unsigned k = 0; left; k++) {
        uint32_t take = cuts[k % (sizeof cuts / sizeof cuts[0])];
        if (take > left)
            take = left;
        K(h->update, 3, st, in, take);
        left -= take;
        memcpy(x360_p(X, st2), x360_p(X, st), h->state);
        memset(x360_p(X, st), 0xEE, h->state);
        uint32_t tmp = st;
        st = st2, st2 = tmp;
    }
    K(h->final, 3, st, out, h->digest);
    expect_hex(h->name, out, want);
    OK(guard_ok(st, h->state) && guard_ok(st2, h->state), "%s: wrote past the state", h->name);
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

    /* the incremental SHA-256 of x360_crypt.c and the one-shot here are one hash (this digest: Python's hashlib) */
    hash_vector(&SHA256, ABC896, 112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    /* one million 'a' (the FIPS 180 long message; digests confirmed with Python's hashlib): many blocks compressed,
     * every chaining value going through guest memory */
    hash_million(&SHA256, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    hash_million(&SHA384, "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b"
                          "07b8b3dc38ecc4ebae97ddd87f3d8985");
    hash_million(&SHA512, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                          "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
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

    memset(x360_p(X, out), 0, 16);
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

    memset(x360_p(X, out), 0, 20);
    KX(XeCryptHmacShaInit, 3, st, k, ks);
    KX(XeCryptHmacShaUpdate, 3, st, d, 5u);
    memcpy(x360_p(X, st2), x360_p(X, st), 0xB0); /* a save state in the middle of the MAC */
    memset(x360_p(X, st), 0xEE, 0xB0);
    KX(XeCryptHmacShaUpdate, 3, st2, d + 5, ds - 5);
    KX(XeCryptHmacShaFinal, 3, st2, out, 20u);
    expect_hex("HMAC-SHA1 incremental", out, want);
    OK(guard_ok(st, 0xB0) && guard_ok(st2, 0xB0), "HMAC-SHA1: wrote past the state");

    /* Verify: the whole MAC, a truncated one, a wrong one, one longer than a digest */
    uint32_t mac = gbytes(x360_p(X, out), 20);
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 20u) == 1, "HmacShaVerify refused the right MAC");
    OK(KX(XeCryptHmacShaVerify, 10, k, ks, d, ds, 0, 0, 0, 0, mac, 12u) == 1, "HmacShaVerify refused a truncated MAC");
    x360_p(X, mac + 11)[0] ^= 1;
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

/* ---- ciphers ---- */

/* a mode taking (state, in, size, out, feed, encrypt): both ways, and chained in place through the feed */
static void cbc_both(const char *what, unsigned ord, uint32_t st, const char *iv, const char *pt, const char *ct)
{
    uint32_t ivn, n, m, feed = ghex(iv, &ivn), in = ghex(pt, &n), want = ghex(ct, &m), out = gstate(n);
    OK(n == m, "%s: vector sizes", what);
    K(ord, 6, st, in, n, out, feed, 1u);
    OK(!memcmp(x360_p(X, out), x360_p(X, want), n), "%s: encryption differs", what);
    OK(!memcmp(x360_p(X, feed), x360_p(X, want + n - ivn), ivn), "%s: the feed is not the last ciphertext block", what);
    OK(guard_ok(out, n), "%s: wrote past the output", what);

    feed = ghex(iv, &ivn);
    K(ord, 6, st, want, n, out, feed, 0u);
    OK(!memcmp(x360_p(X, out), x360_p(X, in), n), "%s: decryption differs", what);
    OK(!memcmp(x360_p(X, feed), x360_p(X, want + n - ivn), ivn), "%s: decrypt feed", what);

    feed = ghex(iv, &ivn);
    memcpy(x360_p(X, out), x360_p(X, in), n);
    K(ord, 6, st, out, ivn, out, feed, 1u);
    K(ord, 6, st, out + ivn, n - ivn, out + ivn, feed, 1u);
    OK(!memcmp(x360_p(X, out), x360_p(X, want), n), "%s: chained in-place encryption differs", what);
    feed = ghex(iv, &ivn);
    K(ord, 6, st, out, n, out, feed, 0u);
    OK(!memcmp(x360_p(X, out), x360_p(X, in), n), "%s: in-place decryption differs", what);
}

static void ecb_both(const char *what, unsigned ord, uint32_t st, const char *pt, const char *ct)
{
    uint32_t n, m, in = ghex(pt, &n), want = ghex(ct, &m), out = gstate(n);
    K(ord, 4, st, in, out, 1u);
    OK(!memcmp(x360_p(X, out), x360_p(X, want), n), "%s: encryption differs", what);
    OK(guard_ok(out, n), "%s: wrote past the block", what);
    K(ord, 4, st, want, out, 0u);
    OK(!memcmp(x360_p(X, out), x360_p(X, in), n), "%s: decryption differs", what);
}

#define NIST_KEY128 "2b7e151628aed2a6abf7158809cf4f3c"
#define NIST_IV "000102030405060708090a0b0c0d0e0f"
#define NIST_P1 "6bc1bee22e409f96e93d7e117393172a"
#define NIST_P2 "ae2d8a571e03ac9c9eb76fac45af8e51"
#define NIST_P3 "30c81c46a35ce411e5fbc1191a0a52ef"
#define NIST_P4 "f69f2445df4f9b17ad2b417be66c3710"
#define NIST_PT NIST_P1 NIST_P2 NIST_P3 NIST_P4

static void test_aes(void)
{
    uint32_t n, st = gstate(0x160);

    /* FIPS 197 appendix C.1 */
    KX(XeCryptAesKey, 2, st, ghex("000102030405060708090a0b0c0d0e0f", &n));
    OK(guard_ok(st, 0x160), "AesKey wrote past the state");
    ecb_both("AES-128 C.1", X360_xboxkrnl_XeCryptAesEcb, st, "00112233445566778899aabbccddeeff",
             "69c4e0d86a7b0430d8cdb78070b4c55a");

    /* FIPS 197 appendix A.1: the expansion of 2b7e..., its last round key w[40..43] */
    KX(XeCryptAesKey, 2, st, ghex(NIST_KEY128, &n));
    expect_hex("AES-128 last round key", st + 10 * 16, "d014f9a8c9ee2589e13f0cc8b6630ca6");

    /* SP 800-38A F.1.1 / F.1.2 ECB-AES128 */
    static const char *const ecb[4][2] = {{NIST_P1, "3ad77bb40d7a3660a89ecaf32466ef97"},
                                          {NIST_P2, "f5d3d58503b9699de785895a96fdbaaf"},
                                          {NIST_P3, "43b1cd7f598ece23881b00e3ed030688"},
                                          {NIST_P4, "7b0c785e27e8ad3f8223207104725dd4"}};
    for (int k = 0; k < 4; k++)
        ecb_both("ECB-AES128 F.1", X360_xboxkrnl_XeCryptAesEcb, st, ecb[k][0], ecb[k][1]);

    /* SP 800-38A F.2.1 / F.2.2 CBC-AES128 */
    cbc_both("CBC-AES128 F.2", X360_xboxkrnl_XeCryptAesCbc, st, NIST_IV, NIST_PT,
             "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
             "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");

    /* SP 800-38A F.5.1 / F.5.2 CTR-AES128: one call, then block by block with the counter carried */
    static const char CTR_CT[] = "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                                 "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee";
    uint32_t len, m, pt = ghex(NIST_PT, &len), ct = ghex(CTR_CT, &m), out = gstate(len);
    uint32_t ctr = ghex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", &n);
    KX(XeCryptAesCtr, 5, st, pt, len, out, ctr);
    OK(!memcmp(x360_p(X, out), x360_p(X, ct), len), "CTR-AES128 F.5.1 differs");
    OK(guard_ok(out, len), "CTR wrote past the output");
    expect_hex("CTR counter after four blocks", ctr, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff03");
    ctr = ghex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", &n);
    for (uint32_t k = 0; k < 4; k++)
        KX(XeCryptAesCtr, 5, st, ct + 16 * k, 16u, out + 16 * k, ctr);
    OK(!memcmp(x360_p(X, out), x360_p(X, pt), len), "CTR-AES128 F.5.2 (block by block) differs");
    ctr = ghex("ffffffffffffffffffffffffffffffff", &n); /* the carry runs through all 128 bits */
    KX(XeCryptAesCtr, 5, st, pt, 16u, out, ctr);
    expect_hex("CTR counter wrap", ctr, "00000000000000000000000000000000");

    /* CBC-MAC from a zero IV. One block: F.1.1's answer. Four blocks: F.2.1's last ciphertext block, the first
     * plaintext block xored with F.2.1's IV so that a zero IV takes the same path. */
    uint32_t key = ghex(NIST_KEY128, &n), mac = gstate(16);
    KX(XeCryptAesCbcMac, 4, key, ghex(NIST_P1, &n), 16u, mac);
    expect_hex("AES CBC-MAC, one block", mac, "3ad77bb40d7a3660a89ecaf32466ef97");
    uint32_t iv = ghex(NIST_IV, &n), msg = ghex(NIST_PT, &len);
    for (uint32_t i = 0; i < 16; i++)
        x360_p(X, msg + i)[0] ^= x360_p(X, iv + i)[0];
    KX(XeCryptAesCbcMac, 4, key, msg, len, mac);
    expect_hex("AES CBC-MAC, four blocks", mac, "3ff1caa1681fac09120eca307586e1a7");
    OK(guard_ok(mac, 16), "CBC-MAC wrote past the MAC");

    /* the key schedule of any size: FIPS 197 appendix C.1, C.2, C.3 */
    static const struct {
        const char *key, *ct;
    } fips[] = {{"000102030405060708090a0b0c0d0e0f", "69c4e0d86a7b0430d8cdb78070b4c55a"},
                {"000102030405060708090a0b0c0d0e0f1011121314151617", "dda97ca4864cdfe06eaf70a0ec0d7191"},
                {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                 "8ea2b7ca516745bfeafc49904b496089"}};
    uint32_t sch = gstate(0x1D4), one = gstate(16);
    for (unsigned k = 0; k < 3; k++) {
        uint32_t ks, cs, kk = ghex(fips[k].key, &ks), p = ghex("00112233445566778899aabbccddeeff", &n);
        uint32_t cc = ghex(fips[k].ct, &cs);
        KX(XeCryptAesCreateKeySchedule, 3, kk, ks, sch);
        OK(guard_ok(sch, 0x1D4), "the schedule wrote past its end");
        KX(XeCryptAesEncryptOne, 3, sch, p, one);
        expect_hex("AES EncryptOne", one, fips[k].ct);
        KX(XeCryptAesDecryptOne, 3, sch, cc, one);
        expect_hex("AES DecryptOne", one, "00112233445566778899aabbccddeeff");
    }

    /* SP 800-38A F.2.5 / F.2.6 CBC-AES256 through the schedule */
    static const char CT256[] = "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
                                "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b";
    uint32_t ks, k256 = ghex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", &ks);
    KX(XeCryptAesCreateKeySchedule, 3, k256, ks, sch);
    uint32_t feed = ghex(NIST_IV, &n), c256 = ghex(CT256, &m);
    KX(XeCryptAesCbcEncrypt, 5, sch, pt, len, out, feed);
    OK(!memcmp(x360_p(X, out), x360_p(X, c256), len), "CBC-AES256 F.2.5 differs");
    feed = ghex(NIST_IV, &n);
    KX(XeCryptAesCbcDecrypt, 5, sch, c256, len, out, feed);
    OK(!memcmp(x360_p(X, out), x360_p(X, pt), len), "CBC-AES256 F.2.6 differs");

    /* a size that is no AES key: the schedule stays unusable and the cipher does nothing */
    KX(XeCryptAesCreateKeySchedule, 3, k256, 17u, sch);
    memset(x360_p(X, one), 0x5A, 16);
    KX(XeCryptAesEncryptOne, 3, sch, pt, one);
    OK(x360_p(X, one)[0] == 0x5A && x360_p(X, one + 15)[0] == 0x5A, "a refused schedule still encrypted");
}

static void test_des(void)
{
    uint32_t n, st = gstate(0x80), st3 = gstate(0x180);

    /* FIPS 81 appendix B: "Now is the time for all ", ECB and CBC */
    KX(XeCryptDesKey, 2, st, ghex("0123456789abcdef", &n));
    OK(guard_ok(st, 0x80), "DesKey wrote past the state");
    ecb_both("DES ECB FIPS 81", X360_xboxkrnl_XeCryptDesEcb, st, "4e6f772069732074", "3fa40e8a984d4815");
    ecb_both("DES ECB FIPS 81", X360_xboxkrnl_XeCryptDesEcb, st, "68652074696d6520", "6a271787ab8883f9");
    ecb_both("DES ECB FIPS 81", X360_xboxkrnl_XeCryptDesEcb, st, "666f7220616c6c20", "893d51ec4b563b53");
    cbc_both("DES CBC FIPS 81", X360_xboxkrnl_XeCryptDesCbc, st, "1234567890abcdef",
             "4e6f77206973207468652074696d6520666f7220616c6c20", "e5c7cdde872bf27c43e934008c389c0f683788499a7c05f6");

    /* SP 800-67 three-key TDEA example, ECB */
    KX(XeCryptDes3Key, 2, st3, ghex("0123456789abcdef23456789abcdef01456789abcdef0123", &n));
    OK(guard_ok(st3, 0x180), "Des3Key wrote past the state");
    static const char *const tdea[3][2] = {{"5468652071756663", "a826fd8ce53b855f"},
                                           {"6b2062726f776e20", "cce21c8112256fe6"},
                                           {"666f78206a756d70", "68d5c05dd9b6b900"}};
    for (int k = 0; k < 3; k++)
        ecb_both("TDEA ECB SP 800-67", X360_xboxkrnl_XeCryptDes3Ecb, st3, tdea[k][0], tdea[k][1]);

    /* TDEA CBC from those ECB answers: a zero IV, each plaintext block xored with the previous ciphertext block */
    cbc_both("TDEA CBC", X360_xboxkrnl_XeCryptDes3Cbc, st3, "0000000000000000",
             "5468652071756663c3069ffe8a4ceb7faa8d64a178500296", "a826fd8ce53b855fcce21c8112256fe668d5c05dd9b6b900");

    /* three equal keys: TDEA is single DES (FIPS 81's first block) */
    KX(XeCryptDes3Key, 2, st3, ghex("0123456789abcdef0123456789abcdef0123456789abcdef", &n));
    ecb_both("TDEA with one key", X360_xboxkrnl_XeCryptDes3Ecb, st3, "4e6f772069732074", "3fa40e8a984d4815");

    /* FIPS 46-3: every key byte has odd parity, its low bit adjusting it */
    uint32_t in = ghex("00010203feff807f", &n), out = gstate(8);
    KX(XeCryptDesParity, 3, in, 8u, out);
    expect_hex("DES parity", out, "01010202fefe807f");
    OK(guard_ok(out, 8), "DesParity wrote past the output");
}

/* ---- big numbers ---- */

/* a call whose arguments do not fit K(): 64-bit registers, 64-bit answer */
static uint64_t call64(unsigned ord, const uint64_t *regs, int n)
{
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    for (int k = 0; k < n; k++)
        c->gpr[3 + k] = regs[k];
    x360_thunk th = {0, (uint16_t)ord, X360_LIB_XBOXKRNL};
    X->m.slice = 1000;
    x360_hle_call(X, &th);
    return c->gpr[3];
}

/* digits given least significant first, stored as the guest holds them */
static uint32_t gqw(const uint64_t *v, uint32_t n)
{
    uint32_t a = gstate(8 * n);
    for (uint32_t i = 0; i < n; i++)
        x360_w64(X, a + 8 * i, v[i]);
    return a;
}

static void expect_qw(const char *what, uint32_t a, const uint64_t *v, uint32_t n)
{
    int same = 1;
    for (uint32_t i = 0; i < n; i++)
        same &= x360_r64(X, a + 8 * i) == v[i];
    OK(same, "%s: wrong digits", what);
    OK(guard_ok(a, 8 * n), "%s: wrote past the number", what);
}

static void test_bn_helpers(void)
{
    uint32_t n, src = ghex("0102030405060708 1112131415161718", &n), dst = gstate(16);

    KX(XeCryptBnDw_SwapLeBe, 3, src, dst, 4u);
    expect_hex("BnDw_SwapLeBe", dst, "0403020108070605 1413121118171615");
    KX(XeCryptBnQw_SwapLeBe, 3, src, dst, 2u);
    expect_hex("BnQw_SwapLeBe", dst, "0403020108070605 1413121118171615");
    KX(XeCryptBnQw_SwapDwQw, 3, src, dst, 2u);
    expect_hex("BnQw_SwapDwQw", dst, "0506070801020304 1516171811121314");
    KX(XeCryptBnQw_SwapDwQwLeBe, 3, src, dst, 2u);
    expect_hex("BnQw_SwapDwQwLeBe", dst, "0807060504030201 1817161514131211");
    OK(guard_ok(dst, 16), "a swap wrote past the destination");

    /* in place */
    memcpy(x360_p(X, dst), x360_p(X, src), 16);
    KX(XeCryptBnQw_SwapDwQwLeBe, 3, dst, dst, 2u);
    expect_hex("BnQw_SwapDwQwLeBe in place", dst, "0807060504030201 1817161514131211");
    memcpy(x360_p(X, dst), x360_p(X, src), 16);
    KX(XeCryptBnQw_SwapDwQw, 3, dst, dst, 2u);
    expect_hex("BnQw_SwapDwQw in place", dst, "0506070801020304 1516171811121314");

    memset(x360_p(X, dst), 0, 16);
    KX(XeCryptBnDw_Copy, 3, src, dst, 3u);
    expect_hex("BnDw_Copy", dst, "0102030405060708 1112131400000000");
    KX(XeCryptBnDw_Zero, 2, dst, 1u);
    expect_hex("BnDw_Zero", dst, "0000000005060708 1112131400000000");
    KX(XeCryptBnQw_Copy, 3, src, dst, 2u);
    OK(!memcmp(x360_p(X, dst), x360_p(X, src), 16), "BnQw_Copy");
    KX(XeCryptBnQw_Zero, 2, dst, 2u);
    expect_hex("BnQw_Zero", dst, "0000000000000000 0000000000000000");
    OK(guard_ok(dst, 16), "BnQw_Zero wrote past the number");

    /* the most significant digit is the last one, each compared as an unsigned 64-bit number */
    static const uint64_t lo[2] = {0xFFFFFFFFFFFFFFFFull, 1}, hi[2] = {0, 2}, big[2] = {0, 0x8000000000000000ull};
    uint32_t a = gqw(lo, 2), b = gqw(hi, 2), d = gqw(big, 2);
    OK(KX(XeCryptBnQwNeCompare, 3, a, b, 2u) == 0xFFFFFFFFu, "Compare: below");
    OK(cpu()->gpr[3] == ~0ull, "Compare: -1 is sign-extended");
    OK(KX(XeCryptBnQwNeCompare, 3, b, a, 2u) == 1, "Compare: above");
    OK(KX(XeCryptBnQwNeCompare, 3, a, a, 2u) == 0, "Compare: equal");
    OK(KX(XeCryptBnQwNeCompare, 3, d, b, 2u) == 1, "Compare: digits are unsigned");
    OK(KX(XeCryptBnQwNeCompare, 3, a, b, 0u) == 0, "Compare: no digits");
}

static void test_bn_mod(void)
{
    uint64_t r[6] = {3};
    OK(call64(X360_xboxkrnl_XeCryptBnQwNeModInv, r, 1) == 0xAAAAAAAAAAAAAAABull, "ModInv(3)");
    r[0] = 0xF00DBABE12345679ull;
    OK(call64(X360_xboxkrnl_XeCryptBnQwNeModInv, r, 1) == 0x50D0095EF19E33C9ull, "ModInv of a modulus digit");

    /* a * b / 2^192 mod N (values from Python's integers) */
    static const uint64_t N[3] = {0xF00DBABE12345679ull, 0x0FEDCBA987654321ull, 0xF123456789ABCDEFull};
    static const uint64_t A_[3] = {0xCCCCCCCCCCCCCCCCull, 0xBBBBBBBBBBBBBBBBull, 0xAAAAAAAAAAAAAAAAull};
    static const uint64_t B_[3] = {0x8899AABBCCDDEEFFull, 0x0011223344556677ull, 0x0123456789ABCDEFull};
    static const uint64_t C_[3] = {0x6904137B2230DAEFull, 0xFD6A07B97C0A0AD7ull, 0x31D3EF4A9807F571ull};
    uint32_t ga = gqw(A_, 3), gb = gqw(B_, 3), gn = gqw(N, 3), out = gstate(24);
    uint64_t args[6] = {ga, gb, out, 0x50D0095EF19E33C9ull, gn, 3};
    call64(X360_xboxkrnl_XeCryptBnQwNeModMul, args, 6);
    expect_qw("ModMul", out, C_, 3);
    args[2] = ga; /* the output may be an input */
    call64(X360_xboxkrnl_XeCryptBnQwNeModMul, args, 6);
    for (uint32_t i = 0; i < 3; i++)
        OK(x360_r64(X, ga + 8 * i) == C_[i], "ModMul in place");

    /* RSA: the textbook 3233 = 61 * 53, e = 17, 65 -> 2790 */
    static const uint64_t c2790[1] = {2790};
    uint32_t key = gstate(0x18), in = gstate(8), res = gstate(8);
    x360_w32(X, key, 1), x360_w32(X, key + 4, 17), x360_w64(X, key + 8, 0), x360_w64(X, key + 16, 3233);
    x360_w64(X, in, 65);
    OK(KX(XeCryptBnQwNeRsaPubCrypt, 3, in, res, key) == 1, "RsaPubCrypt refused a good key");
    expect_qw("RsaPubCrypt 65^17 mod 3233", res, c2790, 1);

    /* two digits, e = 65537 (the value from Python's pow) */
    static const uint64_t M127[2] = {0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull};
    static const uint64_t msg[2] = {0x0011223344556677ull, 0x0123456789ABCDEFull};
    static const uint64_t cph[2] = {0x5D4DEF23C39BE5D7ull, 0x392246F7ABDBA6B0ull};
    uint32_t key2 = gstate(0x20), in2 = gqw(msg, 2), res2 = gstate(16);
    x360_w32(X, key2, 2), x360_w32(X, key2 + 4, 65537), x360_w64(X, key2 + 8, 0);
    x360_w64(X, key2 + 16, M127[0]), x360_w64(X, key2 + 24, M127[1]);
    OK(KX(XeCryptBnQwNeRsaPubCrypt, 3, in2, res2, key2) == 1, "RsaPubCrypt refused a 2-digit key");
    expect_qw("RsaPubCrypt, two digits", res2, cph, 2);

    /* keys it cannot use are refused */
    x360_w64(X, key + 16, 3232);
    OK(KX(XeCryptBnQwNeRsaPubCrypt, 3, in, res, key) == 0, "RsaPubCrypt took an even modulus");
    x360_w64(X, key + 16, 3233), x360_w32(X, key, 65);
    OK(KX(XeCryptBnQwNeRsaPubCrypt, 3, in, res, key) == 0, "RsaPubCrypt took 65 digits");
    x360_w32(X, key, 0);
    OK(KX(XeCryptBnQwNeRsaPubCrypt, 3, in, res, key) == 0, "RsaPubCrypt took no digits");
}

static void test_pkcs1(void)
{
    /* RFC 8017 9.2 note 1: the DigestInfo prefix of SHA-1, 30 21 30 09 06 05 2b 0e 03 02 1a 05 00 04 14. The block is
     * 00 01 FF.. 00 prefix digest, here byte-reversed. */
    static const uint8_t prefix[15] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e,
                                       0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
    uint8_t digest[20], want[128];
    for (int i = 0; i < 20; i++)
        digest[i] = (uint8_t)(0xA0 + i);
    memset(want, 0xFF, sizeof want);
    want[0] = 0x00, want[1] = 0x01;
    want[128 - 36] = 0x00;
    memcpy(want + 128 - 35, prefix, 15);
    memcpy(want + 128 - 20, digest, 20);

    uint32_t d = gbytes(digest, 20), sig = gstate(128);
    KX(XeCryptBnDwLePkcs1Format, 4, d, 0u, sig, 128u);
    int same = 1;
    for (int i = 0; i < 128; i++)
        same &= x360_p(X, sig + (uint32_t)i)[0] == want[127 - i];
    OK(same, "Pkcs1Format: not the RFC 8017 block, reversed");
    OK(guard_ok(sig, 128), "Pkcs1Format wrote past the block");
    OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 128u) == 1, "Pkcs1Verify refused its own format");
    x360_p(X, sig + 3)[0] ^= 1;
    OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 128u) == 0, "Pkcs1Verify accepted another digest");
    x360_p(X, sig + 3)[0] ^= 1;
    x360_p(X, sig + 60)[0] ^= 1;
    OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 128u) == 0, "Pkcs1Verify accepted damaged padding");
    OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 38u) == 0, "Pkcs1Verify accepted a block too short");
    OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 513u) == 0, "Pkcs1Verify accepted a block too long");

    for (uint32_t format = 1; format <= 2; format++) {
        KX(XeCryptBnDwLePkcs1Format, 4, d, format, sig, 128u);
        OK(KX(XeCryptBnDwLePkcs1Verify, 3, d, sig, 128u) == 1, "Pkcs1Verify: format %u", format);
    }
}

/* no published vectors exist for these two: what the documented definition gives by hand */
static void test_parve(void)
{
    uint8_t zero[256] = {0}, key[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t n, sbox = gbytes(zero, 256), k = gbytes(key, 8), out = gstate(8);
    uint32_t in = ghex("0123456789abcdef", &n);

    /* an all-zero table leaves only the rotations: eight of one bit give each byte back */
    KX(XeCryptParveEcb, 4, k, sbox, in, out);
    expect_hex("ParveEcb, zero table", out, "0123456789abcdef");
    OK(guard_ok(out, 8), "ParveEcb wrote past the block");
    KX(XeCryptParveEcb, 4, k, sbox, in, in); /* in place */
    expect_hex("ParveEcb in place", in, "0123456789abcdef");

    /* the MAC: nothing but the IV without a whole block; one block is the cipher of block xor IV; the tail is ignored */
    uint32_t iv = ghex("1000000000000002", &n), msg = ghex("0123456789abcdef ffff", &n), mac = gstate(8);
    uint64_t a[6] = {k, sbox, iv, msg, 7, mac};
    call64(X360_xboxkrnl_XeCryptParveCbcMac, a, 6);
    expect_hex("ParveCbcMac, under one block", mac, "1000000000000002");
    a[4] = 10;
    call64(X360_xboxkrnl_XeCryptParveCbcMac, a, 6);
    expect_hex("ParveCbcMac, one block and a tail", mac, "1123456789abcded");
    OK(guard_ok(mac, 8), "ParveCbcMac wrote past the MAC");

    /* key, round counter and table all at work: values from a separate Python transcription of the definition, which
     * shows the code follows the description, not that the description matches a console */
    uint8_t table[256];
    for (int i = 0; i < 256; i++)
        table[i] = (uint8_t)(i * 7 + 3);
    uint32_t tb = gbytes(table, 256), k2 = ghex("0f1e2d3c4b5a6978", &n), p2 = ghex("0123456789abcdef", &n);
    KX(XeCryptParveEcb, 4, k2, tb, p2, out);
    expect_hex("ParveEcb, full table", out, "f45c448fd863d451");
    uint32_t iv2 = ghex("a0a1a2a3a4a5a6a7", &n), m2 = ghex("00112233445566778899aabbccddeeff5a5a", &n);
    uint64_t b[6] = {k2, tb, iv2, m2, n, mac};
    call64(X360_xboxkrnl_XeCryptParveCbcMac, b, 6);
    expect_hex("ParveCbcMac, two blocks and a tail", mac, "f854856882f13301");
}

static void test_chain_and_sum(void)
{
    /* ab = {1, 0}, cd = {1, 0}, one pair {1, 0}. By the definition's steps: out0 = 0xE79A9C1 after the first half and
     * unchanged by the second, out1 = twice that. */
    uint32_t cd = gstate(8), ab = gstate(8), in = gstate(12), out = gstate(8);
    x360_w32(X, cd, 1), x360_w32(X, cd + 4, 0), x360_w32(X, ab, 1), x360_w32(X, ab + 4, 0);
    x360_w32(X, in, 1), x360_w32(X, in + 4, 0), x360_w32(X, in + 8, 0xDEADBEEF);
    KX(XeCryptChainAndSumMac, 5, cd, ab, in, 2u, out);
    OK(x360_r32(X, out) == 0xE79A9C1 && x360_r32(X, out + 4) == 0x1CF35382, "ChainAndSumMac, one pair");
    OK(guard_ok(out, 8), "ChainAndSumMac wrote past the output");
    KX(XeCryptChainAndSumMac, 5, cd, ab, in, 3u, out); /* the odd word is ignored */
    OK(x360_r32(X, out) == 0xE79A9C1 && x360_r32(X, out + 4) == 0x1CF35382, "ChainAndSumMac, odd word");

    /* no input: the two additive constants, reduced modulo 2^31 - 1 */
    x360_w32(X, ab + 4, 0x80000005), x360_w32(X, cd + 4, 7);
    KX(XeCryptChainAndSumMac, 5, cd, ab, in, 0u, out);
    OK(x360_r32(X, out) == 6 && x360_r32(X, out + 4) == 7, "ChainAndSumMac, no input");

    /* real multipliers over three pairs: from a separate Python transcription of the definition */
    static const uint32_t words[6] = {0x00000001, 0xFFFFFFFF, 0x12345678, 0x9ABCDEF0, 0xDEADBEEF, 0x0BADF00D};
    uint32_t in3 = gstate(24);
    for (uint32_t i = 0; i < 6; i++)
        x360_w32(X, in3 + 4 * i, words[i]);
    x360_w32(X, cd, 0x89ABCDEF), x360_w32(X, cd + 4, 0x01234567);
    x360_w32(X, ab, 0xFEDCBA98), x360_w32(X, ab + 4, 0x76543210);
    KX(XeCryptChainAndSumMac, 5, cd, ab, in3, 6u, out);
    OK(x360_r32(X, out) == 0x4E7476CE && x360_r32(X, out + 4) == 0x17289DCA, "ChainAndSumMac, three pairs");
}

/* private-key operations and formats answer an error, and leave the caller's buffers alone */
static void test_refused(void)
{
    static const unsigned refused[] = {X360_xboxkrnl_XeCryptBnQwBeSigCreate, X360_xboxkrnl_XeCryptBnQwNeRsaPrvCrypt,
                                       X360_xboxkrnl_XeCryptBnQwNeRsaKeyGen};
    uint32_t buf = gstate(0x100);
    for (unsigned k = 0; k < sizeof refused / sizeof refused[0]; k++) {
        memset(x360_p(X, buf), 0x77, 0x100);
        OK(K(refused[k], 4, buf, buf, buf, buf) == 0, "export %u answered success", refused[k]);
        OK(x360_p(X, buf)[0] == 0x77 && x360_p(X, buf + 0xFF)[0] == 0x77, "export %u wrote to the buffer", refused[k]);
    }
}

int main(void)
{
    if (hle_util_start())
        return 1;
    test_md5();
    test_sha2();
    test_hmac();
    test_aes();
    test_des();
    test_bn_helpers();
    test_bn_mod();
    test_pkcs1();
    test_parve();
    test_chain_and_sum();
    test_refused();
    printf("x360_crypt2: %d checks\n", checks);
    return hle_util_end("x360_crypt2");
}
