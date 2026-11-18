/* x360_crypt2.c -- the hash, cipher and big-number exports beyond x360_crypt.c (XBA-296). Our own implementations of
 * the published algorithms (RFC 1321, RFC 2104, FIPS 180-4). Names, signatures and state layouts as documented by
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
};
const unsigned x360_tab_crypt2_n = sizeof x360_tab_crypt2 / sizeof x360_tab_crypt2[0];
