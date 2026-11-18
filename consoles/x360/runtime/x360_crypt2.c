/* x360_crypt2.c -- the hash, cipher and big-number exports beyond x360_crypt.c (XBA-296). Our own implementations of
 * the published algorithms (RFC 1321, RFC 2104, FIPS 180-4, FIPS 197, SP 800-38A, FIPS 46-3). Names, signatures and
 * state layouts as documented by
 * Xenia's xboxkrnl_crypt.cc and ExCrypt's headers (both BSD).
 *
 * Every multi-call state lives in guest memory, big-endian, and nothing is kept on the host: a save state carries a
 * hash in progress. The layouts follow x360_crypt.c (count is the total number of bytes hashed):
 *   XECRYPT_MD5_STATE     0x54  {be32 count, be32 state[4], 64-byte buffer}
 *   XECRYPT_SHA_STATE     0x58  {be32 count, be32 state[5], 64-byte buffer}
 *   XECRYPT_SHA256_STATE  0x64  {be32 count, be32 state[8], 64-byte buffer}
 *   XECRYPT_SHA512_STATE  0xC8  {be64 count, be64 state[8], 128-byte buffer}, SHA-384 too
 *   XECRYPT_HMACMD5_STATE 0xA8, XECRYPT_HMACSHA_STATE 0xB0: two hash states, the inner one first
 *
 * A key longer than the block is hashed, as RFC 2104 says and as x360_crypt.c's XeCryptHmacSha does. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define A9(n) ((uint32_t)x360_stack_arg(x, c, (n)))
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

void x360_crypt_sha_block(int words, uint32_t *h, const uint8_t *p); /* x360_crypt.c */

static uint32_t rol32(uint32_t v, int n) { return v << n | v >> (32 - n); }
static uint64_t ror64(uint64_t v, int n) { return v >> n | v << (64 - n); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

/* ---- MD5 (RFC 1321) ---- */

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
    static const uint8_t shift[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g, round = i / 16;
        if (round == 0)
            f = (b & c) | (~b & d), g = i;
        else if (round == 1)
            f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (round == 2)
            f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else
            f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = a + f + MD5_K[i] + le32(p + 4 * g);
        a = d, d = c, c = b;
        b += rol32(t, shift[round][i % 4]);
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

/* ---- SHA-512 (FIPS 180-4) ---- */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

static void sha512_block(uint64_t h[8], const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

/* ---- one engine for the five hashes, its state loaded from and stored to the guest's ---- */

typedef enum { H_MD5, H_SHA1, H_SHA256, H_SHA384, H_SHA512 } hash_id;

typedef struct {
    uint32_t block, word, words, digest, count; /* sizes in bytes; words: the chaining words the state holds */
} hash_alg;

static const hash_alg ALG[] = {
    [H_MD5] = {64, 4, 4, 16, 4},     [H_SHA1] = {64, 4, 5, 20, 4},    [H_SHA256] = {64, 4, 8, 32, 4},
    [H_SHA384] = {128, 8, 8, 48, 8}, [H_SHA512] = {128, 8, 8, 64, 8},
};

typedef struct {
    hash_id id;
    uint64_t h[8];
    uint64_t count;
    uint8_t buf[128];
} hash;

static uint32_t hash_state_size(hash_id id)
{
    const hash_alg *a = &ALG[id];
    return a->count + a->word * a->words + a->block;
}

static void hash_init(hash *s, hash_id id)
{
    static const uint64_t iv[][8] = {
        [H_MD5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476},
        [H_SHA1] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0},
        [H_SHA256] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
        [H_SHA384] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
                      0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull},
        [H_SHA512] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                      0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull},
    };
    memset(s, 0, sizeof *s);
    s->id = id;
    memcpy(s->h, iv[id], sizeof s->h);
}

static void hash_block(hash *s)
{
    if (ALG[s->id].word == 8) {
        sha512_block(s->h, s->buf);
        return;
    }
    uint32_t h[8];
    for (int i = 0; i < 8; i++)
        h[i] = (uint32_t)s->h[i];
    if (s->id == H_MD5)
        md5_block(h, s->buf);
    else
        x360_crypt_sha_block((int)ALG[s->id].words, h, s->buf);
    for (int i = 0; i < 8; i++)
        s->h[i] = h[i];
}

static void hash_update(hash *s, const uint8_t *p, size_t n)
{
    uint32_t block = ALG[s->id].block;
    while (n) {
        size_t at = s->count % block, take = block - at < n ? block - at : n;
        memcpy(s->buf + at, p, take);
        s->count += take;
        p += take;
        n -= take;
        if (s->count % block == 0)
            hash_block(s);
    }
}

/* pads and writes the digest (at most 64 bytes) */
static void hash_final(hash *s, uint8_t *out)
{
    const hash_alg *a = &ALG[s->id];
    uint32_t len_size = a->block == 128 ? 16 : 8;
    uint64_t bytes = s->count;
    uint8_t len[16] = {0}, pad = 0x80, zero = 0;
    for (int i = 0; i < 8; i++) {
        if (s->id == H_MD5)
            len[i] = (uint8_t)((bytes << 3) >> (8 * i));
        else
            len[len_size - 1 - (uint32_t)i] = (uint8_t)((bytes << 3) >> (8 * i));
    }
    if (len_size == 16)
        len[7] = (uint8_t)(bytes >> 61);
    hash_update(s, &pad, 1);
    while (s->count % a->block != a->block - len_size)
        hash_update(s, &zero, 1);
    hash_update(s, len, len_size);
    for (uint32_t i = 0; i < a->digest; i++) {
        uint32_t w = i / a->word, b = i % a->word;
        out[i] = (uint8_t)(s->h[w] >> (s->id == H_MD5 ? 8 * b : 8 * (a->word - 1 - b)));
    }
}

static void hash_load(x360_machine *x, uint32_t g, hash *s, hash_id id)
{
    const hash_alg *a = &ALG[id];
    s->id = id;
    s->count = a->count == 8 ? x360_r64(x, g) : x360_r32(x, g);
    for (uint32_t i = 0; i < 8; i++)
        s->h[i] = i >= a->words ? 0 : a->word == 8 ? x360_r64(x, g + a->count + 8 * i) : x360_r32(x, g + a->count + 4 * i);
    memset(s->buf, 0, sizeof s->buf);
    memcpy(s->buf, x->base + g + a->count + a->word * a->words, a->block);
}

static void hash_store(x360_machine *x, uint32_t g, const hash *s)
{
    const hash_alg *a = &ALG[s->id];
    if (a->count == 8)
        x360_w64(x, g, s->count);
    else
        x360_w32(x, g, (uint32_t)s->count);
    for (uint32_t i = 0; i < a->words; i++) {
        if (a->word == 8)
            x360_w64(x, g + a->count + 8 * i, s->h[i]);
        else
            x360_w32(x, g + a->count + 4 * i, (uint32_t)s->h[i]);
    }
    memcpy(x->base + g + a->count + a->word * a->words, s->buf, a->block);
}

static void put_digest(x360_machine *x, uint32_t out, uint32_t out_size, const uint8_t *d, uint32_t n)
{
    if (out)
        memcpy(x->base + out, d, out_size < n ? out_size : n);
}

static void guest_init(x360_machine *x, uint32_t g, hash_id id)
{
    hash s;
    hash_init(&s, id);
    hash_store(x, g, &s);
}

static void guest_update(x360_machine *x, uint32_t g, hash_id id, uint32_t p, uint32_t n)
{
    hash s;
    hash_load(x, g, &s, id);
    if (p && n)
        hash_update(&s, x->base + p, n);
    hash_store(x, g, &s);
}

static void guest_final(x360_machine *x, uint32_t g, hash_id id, uint32_t out, uint32_t out_size)
{
    hash s;
    uint8_t d[64];
    hash_load(x, g, &s, id);
    hash_final(&s, d);
    hash_store(x, g, &s);
    put_digest(x, out, out_size, d, ALG[id].digest);
}

/* (input 1, size, input 2, size, input 3, size, output, output size) */
static void one_shot(x360_machine *x, xe_cpu *c, hash_id id)
{
    hash s;
    uint8_t d[64];
    hash_init(&s, id);
    for (int k = 0; k < 3; k++)
        if (A(2 * k) && A(2 * k + 1))
            hash_update(&s, x->base + A(2 * k), A(2 * k + 1));
    hash_final(&s, d);
    put_digest(x, A(6), A(7), d, ALG[id].digest);
}

#define HASH_INCREMENTAL(Name, id)                                                                                     \
    static void k_##Name##Init(x360_machine *x, x360_thread *t, xe_cpu *c)                                             \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        guest_init(x, A(0), id);                                                                                       \
    }                                                                                                                  \
    static void k_##Name##Update(x360_machine *x, x360_thread *t, xe_cpu *c)                                           \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        guest_update(x, A(0), id, A(1), A(2));                                                                         \
    }                                                                                                                  \
    static void k_##Name##Final(x360_machine *x, x360_thread *t, xe_cpu *c)                                            \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        guest_final(x, A(0), id, A(1), A(2));                                                                          \
    }

#define HASH_ONE_SHOT(Name, id)                                                                                        \
    static void k_##Name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        one_shot(x, c, id);                                                                                            \
    }

HASH_INCREMENTAL(XeCryptMd5, H_MD5)
HASH_ONE_SHOT(XeCryptMd5, H_MD5)
HASH_ONE_SHOT(XeCryptSha256, H_SHA256)
HASH_INCREMENTAL(XeCryptSha384, H_SHA384)
HASH_ONE_SHOT(XeCryptSha384, H_SHA384)
HASH_INCREMENTAL(XeCryptSha512, H_SHA512)
HASH_ONE_SHOT(XeCryptSha512, H_SHA512)

/* SHA-224 is SHA-256 from another start (FIPS 180-4 5.3.2): the kernel exports only this Init, a title goes on with
 * XeCryptSha256Update and XeCryptSha256Final and keeps the first 28 bytes */
static void k_XeCryptSha224Init(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    static const uint32_t iv[8] = {0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
                                   0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4};
    hash s;
    hash_init(&s, H_SHA256);
    for (int i = 0; i < 8; i++)
        s.h[i] = iv[i];
    hash_store(x, A(0), &s);
}

/* ---- HMAC (RFC 2104): the guest's state is the inner hash followed by the outer one ---- */

/* the two hashes keyed: each has absorbed its pad */
static void hmac_key(x360_machine *x, hash_id id, uint32_t key, uint32_t key_size, hash *inner, hash *outer)
{
    uint32_t block = ALG[id].block;
    uint8_t k[128] = {0}, pad[128];
    if (key_size > block) {
        hash s;
        hash_init(&s, id);
        hash_update(&s, x->base + key, key_size);
        hash_final(&s, k);
    } else if (key && key_size)
        memcpy(k, x->base + key, key_size);
    hash_init(inner, id);
    hash_init(outer, id);
    for (uint32_t i = 0; i < block; i++)
        pad[i] = k[i] ^ 0x36;
    hash_update(inner, pad, block);
    for (uint32_t i = 0; i < block; i++)
        pad[i] = k[i] ^ 0x5C;
    hash_update(outer, pad, block);
}

/* the MAC from the two hashes */
static void hmac_finish(hash *inner, hash *outer, uint8_t *d)
{
    uint8_t in[64];
    hash_final(inner, in);
    hash_update(outer, in, ALG[inner->id].digest);
    hash_final(outer, d);
}

static void hmac_init(x360_machine *x, uint32_t g, hash_id id, uint32_t key, uint32_t key_size)
{
    hash inner, outer;
    hmac_key(x, id, key, key_size, &inner, &outer);
    hash_store(x, g, &inner);
    hash_store(x, g + hash_state_size(id), &outer);
}

static void hmac_final(x360_machine *x, uint32_t g, hash_id id, uint32_t out, uint32_t out_size)
{
    hash inner, outer;
    uint8_t d[64];
    hash_load(x, g, &inner, id);
    hash_load(x, g + hash_state_size(id), &outer, id);
    hmac_finish(&inner, &outer, d);
    hash_store(x, g, &inner);
    hash_store(x, g + hash_state_size(id), &outer);
    put_digest(x, out, out_size, d, ALG[id].digest);
}

/* (key, key size, input 1, size, input 2, size, input 3, size, ...): the MAC of the three inputs */
static void hmac_one_shot(x360_machine *x, xe_cpu *c, hash_id id, uint8_t *d)
{
    hash inner, outer;
    hmac_key(x, id, A(0), A(1), &inner, &outer);
    for (int k = 0; k < 3; k++)
        if (A(2 + 2 * k) && A(3 + 2 * k))
            hash_update(&inner, x->base + A(2 + 2 * k), A(3 + 2 * k));
    hmac_finish(&inner, &outer, d);
}

#define HMAC_INCREMENTAL(Name, id)                                                                                     \
    static void k_##Name##Init(x360_machine *x, x360_thread *t, xe_cpu *c)                                             \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        hmac_init(x, A(0), id, A(1), A(2));                                                                            \
    }                                                                                                                  \
    static void k_##Name##Update(x360_machine *x, x360_thread *t, xe_cpu *c)                                           \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        guest_update(x, A(0), id, A(1), A(2));                                                                         \
    }                                                                                                                  \
    static void k_##Name##Final(x360_machine *x, x360_thread *t, xe_cpu *c)                                            \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        hmac_final(x, A(0), id, A(1), A(2));                                                                           \
    }

HMAC_INCREMENTAL(XeCryptHmacMd5, H_MD5)
HMAC_INCREMENTAL(XeCryptHmacSha, H_SHA1)

static void k_XeCryptHmacMd5(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint8_t d[64];
    hmac_one_shot(x, c, H_MD5, d);
    put_digest(x, A9(8), A9(9), d, 16);
}

/* (key, key size, three inputs, the MAC to compare, its size): TRUE when the MAC starts with those bytes */
static void k_XeCryptHmacShaVerify(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint8_t d[64];
    uint32_t cmp = A9(8), size = A9(9);
    hmac_one_shot(x, c, H_SHA1, d);
    RET(size <= 20 && (!size || (cmp && !memcmp(x->base + cmp, d, size))));
}

/* ---- block modes: CBC over either cipher; a size is whole blocks, what is left over is not touched ---- */

typedef void (*block_fn)(const void *key, const uint8_t *in, uint8_t *out, int encrypt);

static void cbc_run(block_fn fn, const void *key, uint32_t bs, const uint8_t *in, uint32_t size, uint8_t *out,
                    uint8_t *feed, int encrypt)
{
    uint8_t t[16], ct[16];
    for (uint32_t n = size / bs; n; n--, in += bs, out += bs) {
        memcpy(ct, in, bs); /* the input and the output may be one buffer */
        if (encrypt) {
            for (uint32_t i = 0; i < bs; i++)
                t[i] = ct[i] ^ feed[i];
            fn(key, t, out, 1);
            memcpy(feed, out, bs);
        } else {
            fn(key, ct, t, 0);
            for (uint32_t i = 0; i < bs; i++)
                out[i] = t[i] ^ feed[i];
            memcpy(feed, ct, bs);
        }
    }
}

/* ---- AES (FIPS 197) ----
 * XECRYPT_AES_STATE 0x160: 11 round keys for encryption, then the 11 of the equivalent inverse cipher (5.3.5).
 * XECRYPT_AES_SCHEDULE 0x1D4 (any key size): 29 round-key slots, be32 rounds - 1 at 0x1D0: the Nr + 1 encryption keys,
 * then the Nr - 1 inverse-mixed keys of the equivalent inverse cipher. */

static const uint8_t AES_SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t AES_INV[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

typedef struct {
    uint32_t nr;
    uint8_t enc[15 * 16], dec[15 * 16];
} aes_key;

static uint8_t xt(uint8_t a) { return (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0)); }

static void inv_mix(uint8_t *s) /* InvMixColumns on 16 bytes */
{
    for (int i = 0; i < 16; i += 4) {
        uint8_t a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];
        uint8_t u = xt(xt(a ^ c)), v = xt(xt(b ^ d)), w = xt(u ^ v), e = a ^ b ^ c ^ d;
        s[i] = a ^ e ^ xt(a ^ b) ^ w ^ u;
        s[i + 1] = b ^ e ^ xt(b ^ c) ^ w ^ v;
        s[i + 2] = c ^ e ^ xt(c ^ d) ^ w ^ u;
        s[i + 3] = d ^ e ^ xt(d ^ a) ^ w ^ v;
    }
}

/* KeyExpansion for a key of nk 32-bit words (4, 6, 8); rk holds Nr + 1 round keys. Returns Nr. */
static uint32_t aes_expand(const uint8_t *key, uint32_t nk, uint8_t *rk)
{
    uint32_t nr = nk + 6;
    uint8_t rcon = 1;
    memcpy(rk, key, 4 * nk);
    for (uint32_t i = nk; i < 4 * (nr + 1); i++) {
        uint8_t t[4];
        memcpy(t, rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t first = t[0];
            t[0] = AES_SBOX[t[1]] ^ rcon, t[1] = AES_SBOX[t[2]], t[2] = AES_SBOX[t[3]], t[3] = AES_SBOX[first];
            rcon = xt(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++)
                t[j] = AES_SBOX[t[j]];
        }
        for (uint32_t j = 0; j < 4; j++)
            rk[4 * i + j] = rk[4 * (i - nk) + j] ^ t[j];
    }
    return nr;
}

/* the round keys of the equivalent inverse cipher */
static void aes_invert(const uint8_t *rk, uint32_t nr, uint8_t *dk)
{
    memcpy(dk, rk + 16 * nr, 16);
    for (uint32_t i = 1; i < nr; i++) {
        memcpy(dk + 16 * i, rk + 16 * (nr - i), 16);
        inv_mix(dk + 16 * i);
    }
    memcpy(dk + 16 * nr, rk, 16);
}

static void aes_block(const void *key, const uint8_t *in, uint8_t *out, int encrypt)
{
    const aes_key *k = key;
    const uint8_t *rk = encrypt ? k->enc : k->dec;
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[i];
    for (uint32_t r = 1; r <= k->nr; r++) {
        if (encrypt) {
            for (int i = 0; i < 16; i++) /* SubBytes, ShiftRows */
                t[i] = AES_SBOX[s[(i * 5) & 15]];
            if (r < k->nr)
                for (int i = 0; i < 16; i += 4) { /* MixColumns */
                    uint8_t a = t[i], b = t[i + 1], c = t[i + 2], d = t[i + 3], e = a ^ b ^ c ^ d;
                    t[i] = a ^ e ^ xt(a ^ b), t[i + 1] = b ^ e ^ xt(b ^ c);
                    t[i + 2] = c ^ e ^ xt(c ^ d), t[i + 3] = d ^ e ^ xt(d ^ a);
                }
        } else {
            for (int i = 0; i < 16; i++) /* InvSubBytes, InvShiftRows */
                t[i] = AES_INV[s[(i * 13) & 15]];
            if (r < k->nr)
                inv_mix(t);
        }
        for (uint32_t i = 0; i < 16; i++)
            s[i] = t[i] ^ rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* the guest's XECRYPT_AES_STATE */
static void aes_load(x360_machine *x, uint32_t g, aes_key *k)
{
    k->nr = 10;
    memcpy(k->enc, x->base + g, 11 * 16);
    memcpy(k->dec, x->base + g + 11 * 16, 11 * 16);
}

/* the guest's XECRYPT_AES_SCHEDULE; 0 when no valid key was set in it */
static int aes_load_schedule(x360_machine *x, uint32_t g, aes_key *k)
{
    k->nr = x360_r32(x, g + 0x1D0) + 1;
    if (k->nr != 10 && k->nr != 12 && k->nr != 14)
        return 0;
    memcpy(k->enc, x->base + g, 16 * (k->nr + 1));
    memcpy(k->dec, k->enc + 16 * k->nr, 16);
    memcpy(k->dec + 16, x->base + g + 16 * (k->nr + 1), 16 * (k->nr - 1));
    memcpy(k->dec + 16 * k->nr, k->enc, 16);
    return 1;
}

static void k_XeCryptAesKey(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint8_t rk[11 * 16], dk[11 * 16];
    aes_expand(x->base + A(1), 4, rk);
    aes_invert(rk, 10, dk);
    memcpy(x->base + A(0), rk, sizeof rk);
    memcpy(x->base + A(0) + sizeof rk, dk, sizeof dk);
}

static void k_XeCryptAesEcb(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    aes_key k;
    aes_load(x, A(0), &k);
    aes_block(&k, x->base + A(1), x->base + A(2), A(3) != 0);
}

static void k_XeCryptAesCbc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    aes_key k;
    aes_load(x, A(0), &k);
    cbc_run(aes_block, &k, 16, x->base + A(1), A(2), x->base + A(3), x->base + A(4), A(5) != 0);
}

/* (state, input, size, output, counter): the counter is one big-endian 128-bit number (SP 800-38A B.1), left at the
 * next unused value; a last partial block uses up one counter */
static void k_XeCryptAesCtr(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    aes_key k;
    const uint8_t *in = x->base + A(1);
    uint8_t *out = x->base + A(3), *ctr = x->base + A(4), ks[16];
    aes_load(x, A(0), &k);
    for (uint32_t size = A(2); size;) {
        uint32_t n = size < 16 ? size : 16;
        aes_block(&k, ctr, ks, 1);
        for (uint32_t i = 0; i < n; i++)
            out[i] = in[i] ^ ks[i];
        for (int i = 15; i >= 0 && !++ctr[i]; i--) {
        }
        in += n, out += n, size -= n;
    }
}

/* (AES-128 key, input, size, MAC): CBC from a zero IV, the last block is the MAC; a partial last block is ignored */
static void k_XeCryptAesCbcMac(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    aes_key k;
    uint8_t mac[16] = {0}, b[16];
    const uint8_t *in = x->base + A(1);
    k.nr = aes_expand(x->base + A(0), 4, k.enc);
    for (uint32_t n = A(2) / 16; n; n--, in += 16) {
        for (int i = 0; i < 16; i++)
            b[i] = mac[i] ^ in[i];
        aes_block(&k, b, mac, 1);
    }
    memcpy(x->base + A(3), mac, 16);
}

/* (key, key size in bytes, schedule) */
static void k_XeCryptAesCreateKeySchedule(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t size = A(1), g = A(2);
    uint8_t rk[15 * 16], dk[15 * 16];
    memset(x->base + g, 0, 0x1D4); /* a size that is no AES key leaves a schedule the ciphers refuse */
    if (size != 16 && size != 24 && size != 32)
        return;
    uint32_t nr = aes_expand(x->base + A(0), size / 4, rk);
    aes_invert(rk, nr, dk);
    memcpy(x->base + g, rk, 16 * (nr + 1));
    memcpy(x->base + g + 16 * (nr + 1), dk + 16, 16 * (nr - 1));
    x360_w32(x, g + 0x1D0, nr - 1);
}

static void aes_one(x360_machine *x, xe_cpu *c, int encrypt)
{
    aes_key k;
    if (aes_load_schedule(x, A(0), &k))
        aes_block(&k, x->base + A(1), x->base + A(2), encrypt);
}

static void aes_schedule_cbc(x360_machine *x, xe_cpu *c, int encrypt)
{
    aes_key k;
    if (aes_load_schedule(x, A(0), &k))
        cbc_run(aes_block, &k, 16, x->base + A(1), A(2), x->base + A(3), x->base + A(4), encrypt);
}

static void k_XeCryptAesEncryptOne(x360_machine *x, x360_thread *t, xe_cpu *c) { (void)t, aes_one(x, c, 1); }
static void k_XeCryptAesDecryptOne(x360_machine *x, x360_thread *t, xe_cpu *c) { (void)t, aes_one(x, c, 0); }
static void k_XeCryptAesCbcEncrypt(x360_machine *x, x360_thread *t, xe_cpu *c) { (void)t, aes_schedule_cbc(x, c, 1); }
static void k_XeCryptAesCbcDecrypt(x360_machine *x, x360_thread *t, xe_cpu *c) { (void)t, aes_schedule_cbc(x, c, 0); }

/* ---- DES and triple DES (FIPS 46-3) ----
 * XECRYPT_DES_STATE 0x80: the 16 round keys, each a be64 holding 48 bits. XECRYPT_DES3_STATE 0x180: three of them. */

static const uint8_t DES_IP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                   62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                   57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                   61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t DES_FP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                   38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                   36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                   34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};
static const uint8_t DES_E[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                  12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                  22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
static const uint8_t DES_P[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                  2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};
static const uint8_t DES_PC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
                                    35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
                                    46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
static const uint8_t DES_PC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t DES_SHIFT[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t DES_S[8][4][16] = {
    {{14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7},
     {0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8},
     {4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0},
     {15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13}},
    {{15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10},
     {3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5},
     {0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15},
     {13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9}},
    {{10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8},
     {13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1},
     {13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7},
     {1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12}},
    {{7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15},
     {13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9},
     {10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4},
     {3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14}},
    {{2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9},
     {14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6},
     {4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14},
     {11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3}},
    {{12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11},
     {10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8},
     {9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6},
     {4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13}},
    {{4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1},
     {13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6},
     {1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2},
     {6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12}},
    {{13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7},
     {1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2},
     {7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8},
     {2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}}};

/* the standard's bit numbering: bit 1 is the most significant of the `bits` wide input */
static uint64_t des_permute(uint64_t v, int bits, const uint8_t *table, int n)
{
    uint64_t r = 0;
    for (int i = 0; i < n; i++)
        r = r << 1 | ((v >> (bits - table[i])) & 1);
    return r;
}

static void des_schedule(const uint8_t *key, uint64_t sub[16])
{
    uint64_t cd = des_permute(be64(key), 64, DES_PC1, 56);
    uint32_t l = (uint32_t)(cd >> 28), r = (uint32_t)cd & 0xFFFFFFF;
    for (int i = 0; i < 16; i++) {
        int s = DES_SHIFT[i];
        l = (l << s | l >> (28 - s)) & 0xFFFFFFF;
        r = (r << s | r >> (28 - s)) & 0xFFFFFFF;
        sub[i] = des_permute((uint64_t)l << 28 | r, 56, DES_PC2, 48);
    }
}

static uint64_t des_crypt(const uint64_t sub[16], uint64_t block, int encrypt)
{
    uint64_t v = des_permute(block, 64, DES_IP, 64);
    uint32_t l = (uint32_t)(v >> 32), r = (uint32_t)v;
    for (int i = 0; i < 16; i++) {
        uint64_t e = des_permute(r, 32, DES_E, 48) ^ sub[encrypt ? i : 15 - i];
        uint32_t f = 0;
        for (int b = 0; b < 8; b++) {
            uint32_t six = (uint32_t)(e >> (42 - 6 * b)) & 63;
            f = f << 4 | DES_S[b][(six >> 4 & 2) | (six & 1)][six >> 1 & 15];
        }
        uint32_t next = l ^ (uint32_t)des_permute(f, 32, DES_P, 32);
        l = r, r = next;
    }
    return des_permute((uint64_t)r << 32 | l, 64, DES_FP, 64);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static void des_block(const void *key, const uint8_t *in, uint8_t *out, int encrypt)
{
    put_be64(out, des_crypt(key, be64(in), encrypt));
}

/* EDE (FIPS 46-3): encrypt with the first key, decrypt with the second, encrypt with the third */
static void des3_block(const void *key, const uint8_t *in, uint8_t *out, int encrypt)
{
    const uint64_t(*sub)[16] = key;
    uint64_t v = be64(in);
    for (int i = 0; i < 3; i++)
        v = encrypt ? des_crypt(sub[i], v, i != 1) : des_crypt(sub[2 - i], v, i == 1);
    put_be64(out, v);
}

static void des_load(x360_machine *x, uint32_t g, uint64_t *sub, int keys)
{
    for (int i = 0; i < 16 * keys; i++)
        sub[i] = x360_r64(x, g + 8 * (uint32_t)i);
}

static void k_XeCryptDesKey(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[16];
    des_schedule(x->base + A(1), sub);
    for (uint32_t i = 0; i < 16; i++)
        x360_w64(x, A(0) + 8 * i, sub[i]);
}

static void k_XeCryptDesEcb(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[16];
    des_load(x, A(0), sub, 1);
    des_block(sub, x->base + A(1), x->base + A(2), A(3) != 0);
}

static void k_XeCryptDesCbc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[16];
    des_load(x, A(0), sub, 1);
    cbc_run(des_block, sub, 8, x->base + A(1), A(2), x->base + A(3), x->base + A(4), A(5) != 0);
}

/* (state, three 8-byte keys one after the other) */
static void k_XeCryptDes3Key(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[16];
    for (uint32_t k = 0; k < 3; k++) {
        des_schedule(x->base + A(1) + 8 * k, sub);
        for (uint32_t i = 0; i < 16; i++)
            x360_w64(x, A(0) + 0x80 * k + 8 * i, sub[i]);
    }
}

static void k_XeCryptDes3Ecb(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[3][16];
    des_load(x, A(0), sub[0], 3);
    des3_block(sub, x->base + A(1), x->base + A(2), A(3) != 0);
}

static void k_XeCryptDes3Cbc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint64_t sub[3][16];
    des_load(x, A(0), sub[0], 3);
    cbc_run(des3_block, sub, 8, x->base + A(1), A(2), x->base + A(3), x->base + A(4), A(5) != 0);
}

/* (input, size, output): every byte with odd parity, its lowest bit being the parity bit */
static void k_XeCryptDesParity(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    const uint8_t *in = x->base + A(0);
    uint8_t *out = x->base + A(2);
    for (uint32_t i = 0; i < A(1); i++) {
        uint8_t v = in[i] & 0xFE, p = v;
        p ^= p >> 4, p ^= p >> 2, p ^= p >> 1;
        out[i] = v | (~p & 1);
    }
}

const x360_hle_row x360_tab_crypt2[] = {
    X360_HK(XeCryptMd5Init, k_XeCryptMd5Init),
    X360_HK(XeCryptMd5Update, k_XeCryptMd5Update),
    X360_HK(XeCryptMd5Final, k_XeCryptMd5Final),
    X360_HK(XeCryptMd5, k_XeCryptMd5),
    X360_HK(XeCryptSha224Init, k_XeCryptSha224Init),
    X360_HK(XeCryptSha256, k_XeCryptSha256),
    X360_HK(XeCryptSha384Init, k_XeCryptSha384Init),
    X360_HK(XeCryptSha384Update, k_XeCryptSha384Update),
    X360_HK(XeCryptSha384Final, k_XeCryptSha384Final),
    X360_HK(XeCryptSha384, k_XeCryptSha384),
    X360_HK(XeCryptSha512Init, k_XeCryptSha512Init),
    X360_HK(XeCryptSha512Update, k_XeCryptSha512Update),
    X360_HK(XeCryptSha512Final, k_XeCryptSha512Final),
    X360_HK(XeCryptSha512, k_XeCryptSha512),
    X360_HK(XeCryptHmacMd5Init, k_XeCryptHmacMd5Init),
    X360_HK(XeCryptHmacMd5Update, k_XeCryptHmacMd5Update),
    X360_HK(XeCryptHmacMd5Final, k_XeCryptHmacMd5Final),
    X360_HK(XeCryptHmacMd5, k_XeCryptHmacMd5),
    X360_HK(XeCryptHmacShaInit, k_XeCryptHmacShaInit),
    X360_HK(XeCryptHmacShaUpdate, k_XeCryptHmacShaUpdate),
    X360_HK(XeCryptHmacShaFinal, k_XeCryptHmacShaFinal),
    X360_HK(XeCryptHmacShaVerify, k_XeCryptHmacShaVerify),
    X360_HK(XeCryptAesKey, k_XeCryptAesKey),
    X360_HK(XeCryptAesEcb, k_XeCryptAesEcb),
    X360_HK(XeCryptAesCbc, k_XeCryptAesCbc),
    X360_HK(XeCryptAesCtr, k_XeCryptAesCtr),
    X360_HK(XeCryptAesCbcMac, k_XeCryptAesCbcMac),
    X360_HK(XeCryptAesCreateKeySchedule, k_XeCryptAesCreateKeySchedule),
    X360_HK(XeCryptAesEncryptOne, k_XeCryptAesEncryptOne),
    X360_HK(XeCryptAesDecryptOne, k_XeCryptAesDecryptOne),
    X360_HK(XeCryptAesCbcEncrypt, k_XeCryptAesCbcEncrypt),
    X360_HK(XeCryptAesCbcDecrypt, k_XeCryptAesCbcDecrypt),
    X360_HK(XeCryptDesKey, k_XeCryptDesKey),
    X360_HK(XeCryptDesEcb, k_XeCryptDesEcb),
    X360_HK(XeCryptDesCbc, k_XeCryptDesCbc),
    X360_HK(XeCryptDesParity, k_XeCryptDesParity),
    X360_HK(XeCryptDes3Key, k_XeCryptDes3Key),
    X360_HK(XeCryptDes3Ecb, k_XeCryptDes3Ecb),
    X360_HK(XeCryptDes3Cbc, k_XeCryptDes3Cbc),
};
const unsigned x360_tab_crypt2_n = sizeof x360_tab_crypt2 / sizeof x360_tab_crypt2[0];
