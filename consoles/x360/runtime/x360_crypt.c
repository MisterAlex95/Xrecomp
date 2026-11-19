/* x360_crypt.c -- the kernel's general-purpose crypto exports titles call (XBA-270): SHA-1 (one shot, incremental,
 * HMAC), SHA-256, RC4, random bytes. Our own implementations of the public algorithms (FIPS 180-4, RFC 2104); the
 * guest state layouts are Xenia's (xboxkrnl_crypt.cc, BSD): XECRYPT_SHA_STATE {be32 count, be32 state[5], 64-byte
 * buffer}, XECRYPT_SHA256_STATE {be32 count, be32 state[8], 64-byte buffer}, XECRYPT_RC4_STATE {S[256], i, j}.
 * Nothing here involves a console key (the XeKeys* exports stay unimplemented). XeCryptRandom is deterministic (a
 * fixed-seed generator): runs stay reproducible. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define A9(n) ((uint32_t)x360_stack_arg(x, c, (n)))

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
static uint32_t ror(uint32_t v, int n) { return v >> n | v << (32 - n); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ---- SHA-1 / SHA-256 on host buffers ---- */

typedef struct {
    uint32_t h[8];
    uint64_t count;
    uint8_t buf[64];
    int words; /* 5: SHA-1, 8: SHA-256 */
} sha;

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

/* one compression of SHA-1 (words 5) or SHA-256 (words 8): x360_crypt2.c's hashes share these */
void x360_crypt_sha_block(int words, uint32_t *h, const uint8_t *p);
void x360_crypt_sha_block(int words, uint32_t *h, const uint8_t *p)
{
    if (words == 5)
        sha1_block(h, p);
    else
        sha256_block(h, p);
}

static void sha_init(sha *s, int words)
{
    static const uint32_t i1[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    static const uint32_t i256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(s, 0, sizeof *s);
    s->words = words;
    memcpy(s->h, words == 5 ? i1 : i256, 4u * (unsigned)words);
}

static void sha_update(sha *s, const uint8_t *p, size_t n)
{
    while (n) {
        size_t at = s->count % 64, take = 64 - at < n ? 64 - at : n;
        memcpy(s->buf + at, p, take);
        s->count += take;
        p += take;
        n -= take;
        if (s->count % 64 == 0) {
            if (s->words == 5)
                sha1_block(s->h, s->buf);
            else
                sha256_block(s->h, s->buf);
        }
    }
}

static void sha_final(sha *s, uint8_t *out)
{
    uint64_t bits = s->count * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha_update(s, &pad, 1);
    while (s->count % 64 != 56)
        sha_update(s, &zero, 1);
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < s->words; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

/* the guest's state <-> ours (count is the total byte count, 32 bits; the buffer holds count % 64 bytes) */
static void state_in(x360_machine *x, uint32_t g, sha *s, int words)
{
    s->words = words;
    s->count = x360_r32(x, g);
    for (int i = 0; i < words; i++)
        s->h[i] = x360_r32(x, g + 4 + 4 * (uint32_t)i);
    memcpy(s->buf, x360_p(x, g + 4 + 4 * (uint32_t)words), 64);
}

static void state_out(x360_machine *x, uint32_t g, const sha *s)
{
    x360_w32(x, g, (uint32_t)s->count);
    for (int i = 0; i < s->words; i++)
        x360_w32(x, g + 4 + 4 * (uint32_t)i, s->h[i]);
    memcpy(x360_p(x, g + 4 + 4 * (uint32_t)s->words), s->buf, 64);
}

static void out_digest(x360_machine *x, uint32_t out, uint32_t out_size, const uint8_t *d, uint32_t n)
{
    if (out)
        memcpy(x360_p(x, out), d, out_size < n ? out_size : n);
}

/* ---- exports ---- */

void x360_k_XeCryptShaInit(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    sha_init(&s, 5);
    state_out(x, A(0), &s);
}

void x360_k_XeCryptShaUpdate(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    state_in(x, A(0), &s, 5);
    sha_update(&s, x360_p(x, A(1)), A(2));
    state_out(x, A(0), &s);
}

void x360_k_XeCryptShaFinal(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    uint8_t d[20];
    state_in(x, A(0), &s, 5);
    sha_final(&s, d);
    out_digest(x, A(1), A(2), d, 20);
    for (int i = 0; i < 5; i++)
        x360_w32(x, A(0) + 4 + 4 * (uint32_t)i, s.h[i]);
}

void x360_k_XeCryptSha(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (input 1, size, input 2, size, input 3, size, output, output size) */
    sha s;
    uint8_t d[20];
    sha_init(&s, 5);
    for (int k = 0; k < 3; k++)
        if (A(2 * k) && A(2 * k + 1))
            sha_update(&s, x360_p(x, A(2 * k)), A(2 * k + 1));
    sha_final(&s, d);
    out_digest(x, A(6), A(7), d, 20);
}

void x360_k_XeCryptSha256Init(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    sha_init(&s, 8);
    state_out(x, A(0), &s);
}

void x360_k_XeCryptSha256Update(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    state_in(x, A(0), &s, 8);
    sha_update(&s, x360_p(x, A(1)), A(2));
    state_out(x, A(0), &s);
}

void x360_k_XeCryptSha256Final(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    sha s;
    uint8_t d[32];
    state_in(x, A(0), &s, 8);
    sha_final(&s, d);
    out_digest(x, A(1), A(2), d, 32);
}

void x360_k_XeCryptHmacSha(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (key, key size, input 1, size, input 2, size, input 3, size, output, output size: the last two on the stack) */
    uint8_t key[64] = {0}, pad[64], d[20];
    uint32_t ks = A(1);
    sha s;
    if (ks > 64) {
        sha_init(&s, 5);
        sha_update(&s, x360_p(x, A(0)), ks);
        sha_final(&s, key);
    } else if (ks)
        memcpy(key, x360_p(x, A(0)), ks);
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x36;
    sha_init(&s, 5);
    sha_update(&s, pad, 64);
    for (int k = 0; k < 3; k++)
        if (A(2 + 2 * k) && A(3 + 2 * k))
            sha_update(&s, x360_p(x, A(2 + 2 * k)), A(3 + 2 * k));
    sha_final(&s, d);
    for (int i = 0; i < 64; i++)
        pad[i] = key[i] ^ 0x5C;
    sha_init(&s, 5);
    sha_update(&s, pad, 64);
    sha_update(&s, d, 20);
    sha_final(&s, d);
    out_digest(x, A9(8), A9(9), d, 20);
}

/* RC4: XECRYPT_RC4_STATE {S[256], i, j} */
static void rc4_key(uint8_t *st, const uint8_t *key, uint32_t n)
{
    for (int i = 0; i < 256; i++)
        st[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256 && n; i++) {
        j = (j + st[i] + key[i % n]) & 0xFF;
        uint8_t tmp = st[i];
        st[i] = st[j];
        st[j] = tmp;
    }
    st[256] = st[257] = 0;
}

static void rc4_run(uint8_t *st, uint8_t *p, uint32_t n)
{
    uint8_t i = st[256], j = st[257];
    for (uint32_t k = 0; k < n; k++) {
        i = (uint8_t)(i + 1);
        j = (uint8_t)(j + st[i]);
        uint8_t tmp = st[i];
        st[i] = st[j];
        st[j] = tmp;
        p[k] ^= st[(uint8_t)(st[i] + st[j])];
    }
    st[256] = i, st[257] = j;
}

void x360_k_XeCryptRc4Key(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    rc4_key(x360_p(x, A(0)), x360_p(x, A(1)), A(2));
}

void x360_k_XeCryptRc4Ecb(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    rc4_run(x360_p(x, A(0)), x360_p(x, A(1)), A(2));
}

void x360_k_XeCryptRc4(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint8_t st[258];
    rc4_key(st, x360_p(x, A(0)), A(1));
    rc4_run(st, x360_p(x, A(2)), A(3));
}

void x360_k_XeCryptRandom(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint8_t *p = x360_p(x, A(0));
    if (!x->rng)
        x->rng = 0x9E3779B97F4A7C15ull;
    for (uint32_t k = 0; k < A(1); k++) {
        x->rng ^= x->rng << 13, x->rng ^= x->rng >> 7, x->rng ^= x->rng << 17;
        p[k] = (uint8_t)(x->rng >> 24);
    }
    c->gpr[3] = 0;
}
