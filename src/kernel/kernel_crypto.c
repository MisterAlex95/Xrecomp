/*
 * kernel_crypto.c - Xbox Crypto Functions
 *
 * SHA-1, RC4, HMAC-SHA1, DES / triple DES (ECB, CBC), RSA (BSAFE keys,
 * PKCS#1 v1.5 signature check) and XcModExp: the kernel's Xc* exports,
 * ordinals 335-351.
 *
 * The Xbox uses these for:
 *   - Save game signing (SHA-1 + HMAC)
 *   - Xbox Live authentication (RSA, RC4, DES)
 *   - Content verification (SHA-1, RSA)
 *
 * All portable software implementations: results must be the console's
 * byte for byte (a save signed here is read back by the title), and the
 * Android runtime links no crypto library.
 */

#include "kernel.h"
#include <string.h>

/* SHA-1 / RC4 / HMAC below are portable software implementations, so the
 * only platform dependency is BCrypt's types in the (unused) SHA_INTERNAL
 * struct. Keep it Windows-only. */
#if defined(_WIN32)
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

/* ============================================================================
 * SHA-1
 *
 * Xbox SHA-1 is standard FIPS 180-1 SHA-1. We use BCrypt for correctness.
 * The XBOX_SHA_CONTEXT is our own structure (defined in kernel.h).
 * We store the BCrypt hash handle inside it.
 * ============================================================================ */

/* We use a wrapper since XBOX_SHA_CONTEXT may not be large enough for
 * BCrypt's internal state. Store BCrypt handles in a separate mapping. */

#if defined(_WIN32)
typedef struct _SHA_INTERNAL {
    BCRYPT_ALG_HANDLE  hAlg;
    BCRYPT_HASH_HANDLE hHash;
    UCHAR              hashObject[256]; /* BCrypt hash object buffer */
} SHA_INTERNAL;
#endif

/*
 * Since XBOX_SHA_CONTEXT is only 92 bytes (State[5] + Count[2] + Buffer[64]),
 * we can't fit a BCrypt handle in it. We use a simple software SHA-1 instead.
 * This also ensures byte-exact compatibility with the Xbox implementation.
 */

/* Software SHA-1 implementation - matches Xbox/FIPS exactly */

static ULONG sha1_rotate_left(ULONG value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_transform(ULONG state[5], const UCHAR buffer[64])
{
    ULONG a, b, c, d, e, f, k, temp;
    ULONG w[80];
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        w[i] = ((ULONG)buffer[i * 4] << 24)
             | ((ULONG)buffer[i * 4 + 1] << 16)
             | ((ULONG)buffer[i * 4 + 2] << 8)
             | ((ULONG)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rotate_left(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = sha1_rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

VOID __stdcall xbox_XcSHAInit(PXBOX_SHA_CONTEXT ShaContext)
{
    if (!ShaContext)
        return;

    ShaContext->State[0] = 0x67452301;
    ShaContext->State[1] = 0xEFCDAB89;
    ShaContext->State[2] = 0x98BADCFE;
    ShaContext->State[3] = 0x10325476;
    ShaContext->State[4] = 0xC3D2E1F0;
    ShaContext->Count[0] = 0;
    ShaContext->Count[1] = 0;
    memset(ShaContext->Buffer, 0, 64);
}

VOID __stdcall xbox_XcSHAUpdate(PXBOX_SHA_CONTEXT ShaContext, const UCHAR* Input, ULONG InputLength)
{
    ULONG i, index, part_len;

    if (!ShaContext || !Input || InputLength == 0)
        return;

    index = (ShaContext->Count[0] >> 3) & 0x3F;

    /* Update bit count */
    ShaContext->Count[0] += (InputLength << 3);
    if (ShaContext->Count[0] < (InputLength << 3))
        ShaContext->Count[1]++;
    ShaContext->Count[1] += (InputLength >> 29);

    part_len = 64 - index;

    if (InputLength >= part_len) {
        memcpy(&ShaContext->Buffer[index], Input, part_len);
        sha1_transform(ShaContext->State, ShaContext->Buffer);

        for (i = part_len; i + 63 < InputLength; i += 64) {
            sha1_transform(ShaContext->State, &Input[i]);
        }

        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ShaContext->Buffer[index], &Input[i], InputLength - i);
}

VOID __stdcall xbox_XcSHAFinal(PXBOX_SHA_CONTEXT ShaContext, UCHAR* Digest)
{
    UCHAR finalcount[8];
    UCHAR pad;
    ULONG i;

    if (!ShaContext || !Digest)
        return;

    /* Store bit count (big-endian) */
    for (i = 0; i < 4; i++) {
        finalcount[i]     = (UCHAR)((ShaContext->Count[1] >> ((3 - i) * 8)) & 0xFF);
        finalcount[i + 4] = (UCHAR)((ShaContext->Count[0] >> ((3 - i) * 8)) & 0xFF);
    }

    /* Pad to 56 mod 64 */
    pad = 0x80;
    xbox_XcSHAUpdate(ShaContext, &pad, 1);
    pad = 0x00;
    while ((ShaContext->Count[0] >> 3) % 64 != 56) {
        xbox_XcSHAUpdate(ShaContext, &pad, 1);
    }

    /* Append length */
    xbox_XcSHAUpdate(ShaContext, finalcount, 8);

    /* Output digest (big-endian) */
    for (i = 0; i < 5; i++) {
        Digest[i * 4]     = (UCHAR)((ShaContext->State[i] >> 24) & 0xFF);
        Digest[i * 4 + 1] = (UCHAR)((ShaContext->State[i] >> 16) & 0xFF);
        Digest[i * 4 + 2] = (UCHAR)((ShaContext->State[i] >> 8) & 0xFF);
        Digest[i * 4 + 3] = (UCHAR)((ShaContext->State[i]) & 0xFF);
    }
}

/* ============================================================================
 * RC4 (ARC4)
 *
 * Standard RC4 stream cipher. Used for Xbox Live encryption and some
 * save game protection. Implemented in software for portability.
 * ============================================================================ */

VOID __stdcall xbox_XcRC4Key(PXBOX_RC4_CONTEXT Rc4Context, ULONG KeyLength, const UCHAR* Key)
{
    UCHAR temp;
    ULONG i;
    UCHAR j = 0;

    if (!Rc4Context || !Key || KeyLength == 0)
        return;

    /* KSA (Key-Scheduling Algorithm) */
    for (i = 0; i < 256; i++)
        Rc4Context->S[i] = (UCHAR)i;

    for (i = 0; i < 256; i++) {
        j = j + Rc4Context->S[i] + Key[i % KeyLength];
        temp = Rc4Context->S[i];
        Rc4Context->S[i] = Rc4Context->S[j];
        Rc4Context->S[j] = temp;
    }

    Rc4Context->i = 0;
    Rc4Context->j = 0;
}

VOID __stdcall xbox_XcRC4Crypt(PXBOX_RC4_CONTEXT Rc4Context, ULONG Length, UCHAR* Data)
{
    UCHAR temp;
    ULONG n;

    if (!Rc4Context || !Data || Length == 0)
        return;

    /* PRGA (Pseudo-Random Generation Algorithm) */
    for (n = 0; n < Length; n++) {
        Rc4Context->i++;
        Rc4Context->j += Rc4Context->S[Rc4Context->i];

        temp = Rc4Context->S[Rc4Context->i];
        Rc4Context->S[Rc4Context->i] = Rc4Context->S[Rc4Context->j];
        Rc4Context->S[Rc4Context->j] = temp;

        Data[n] ^= Rc4Context->S[(UCHAR)(Rc4Context->S[Rc4Context->i] + Rc4Context->S[Rc4Context->j])];
    }
}

/* ============================================================================
 * HMAC-SHA1
 *
 * Standard RFC 2104 HMAC using SHA-1. Used for save game authentication.
 * Xbox XcHMAC takes two data buffers to hash (Data1 and Data2, concatenated).
 * ============================================================================ */

VOID __stdcall xbox_XcHMAC(
    const UCHAR* Key, ULONG KeyLength,
    const UCHAR* Data1, ULONG Data1Length,
    const UCHAR* Data2, ULONG Data2Length,
    UCHAR* Digest)
{
    XBOX_SHA_CONTEXT sha;
    UCHAR key_block[64];
    UCHAR ipad[64];
    UCHAR opad[64];
    UCHAR inner_digest[20];
    ULONG i;

    if (!Key || !Digest)
        return;

    /* If key is longer than block size, hash it first */
    memset(key_block, 0, 64);
    if (KeyLength > 64) {
        xbox_XcSHAInit(&sha);
        xbox_XcSHAUpdate(&sha, Key, KeyLength);
        xbox_XcSHAFinal(&sha, key_block);
    } else {
        memcpy(key_block, Key, KeyLength);
    }

    /* Inner padding */
    for (i = 0; i < 64; i++)
        ipad[i] = key_block[i] ^ 0x36;

    /* Outer padding */
    for (i = 0; i < 64; i++)
        opad[i] = key_block[i] ^ 0x5C;

    /* Inner hash: SHA1(ipad || data1 || data2) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, ipad, 64);
    if (Data1 && Data1Length > 0)
        xbox_XcSHAUpdate(&sha, Data1, Data1Length);
    if (Data2 && Data2Length > 0)
        xbox_XcSHAUpdate(&sha, Data2, Data2Length);
    xbox_XcSHAFinal(&sha, inner_digest);

    /* Outer hash: SHA1(opad || inner_digest) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, opad, 64);
    xbox_XcSHAUpdate(&sha, inner_digest, 20);
    xbox_XcSHAFinal(&sha, Digest);

    /* Clean up sensitive data */
    SecureZeroMemory(key_block, sizeof(key_block));
    SecureZeroMemory(ipad, sizeof(ipad));
    SecureZeroMemory(opad, sizeof(opad));
    SecureZeroMemory(inner_digest, sizeof(inner_digest));
}


/* ============================================================================
 * Signatures and buffer layouts (ordinals 341-351)
 *
 * The prototypes follow nxdk's lib/xboxkrnl/xboxkrnl.h (XcPKEncPublic ...
 * XcUpdateCrypto, CRYPTO_VECTOR). nxdk gives names and types only; what the
 * buffers hold is the rest of this comment, with its source:
 *
 *  - Big numbers (RSA operands, keys, XcModExp's arguments) are stored least
 *    significant byte first, as XePublicKeyData stores its modulus.
 *  - An RSA key is Microsoft's BSAFE layout, the 284 bytes of XePublicKeyData
 *    for a 2048-bit key: magic "RSA1" (public) or "RSA2" (private), keylen
 *    (bytes reserved per number, bitlen/8 + 8), bitlen, datalen (bitlen/8 - 1),
 *    public exponent, then the modulus (keylen bytes). A private key then has
 *    prime1, prime2, exp1, exp2, coefficient (keylen/2 bytes each) and the
 *    private exponent (keylen bytes); only the modulus and the private
 *    exponent are used here. The repo has no private key to check this
 *    against: XcPKDecPrivate is tested by a round trip with a key of our own.
 *  - The operands of XcPKEncPublic / XcPKDecPrivate / XcVerifyPKCS1Signature
 *    are bitlen/8 bytes; XcPKGetKeyLen gives keylen, which is at least that,
 *    so a buffer sized from it is large enough.
 *  - The signature block, once raised to the public exponent, is PKCS#1 v1.5
 *    (RFC 8017 9.2, EMSA-PKCS1-v1_5 with SHA-1) stored least significant
 *    byte first: in memory the reversed digest, the reversed DigestInfo
 *    prefix, 0x00, 0xFF padding, 0x01, 0x00. The console also accepts the
 *    DigestInfo without its NULL parameters, or no DigestInfo at all (the
 *    xbedump tool's XBE signature check, which mirrors the kernel's).
 *  - XcModExp(A, B, C, D, n): A = B^C mod D, each n dwords; returns 1, or 0
 *    when it cannot compute (zero modulus, too large).
 *  - XcKeyTable / XcBlockCrypt / XcBlockCryptCBC: cipher 0 is DES, 1 is
 *    triple DES (EDE); operation 1 encrypts, 0 decrypts (the XDK's
 *    XC_SERVICE_* values, also the mbedTLS / Microsoft DES mode values). The
 *    key table is the caller's buffer of 128 bytes (DES) or 384 (3DES); what
 *    is inside is ours. The triple-DES key is 16 bytes, K1 K2 with K3 = K1:
 *    Cxbx-Reloaded found a title passing a 16-byte buffer, and reading 24
 *    bytes ran past it. CBC updates the feedback block in place, as the
 *    console's (and every CBC API of that era) does, so consecutive calls
 *    chain.
 *  - XcCryptService does nothing on the console and returns 0.
 *  - XcUpdateCrypto(new, rom): replaces kernel entries with the non-NULL
 *    pointers of `new` and copies the original vector to `rom`.
 *
 * Algorithms: DES from FIPS 46-3, triple DES from NIST SP 800-67, RSA as
 * modular exponentiation (RFC 8017 5.1 / 5.2), Montgomery multiplication.
 * Self-contained: the Android runtime links no crypto library.
 * ============================================================================ */

#include <stdlib.h>

#define XC_RSA1_MAGIC 0x31415352u  /* "RSA1" */
#define XC_RSA2_MAGIC 0x32415352u  /* "RSA2" */
#define XC_BN_MAX_LIMBS 128         /* 4096-bit operands */

static uint32_t xc_rd32(const UCHAR *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── Big numbers: little-endian arrays of 32-bit limbs ────────────────── */

static int bn_cmp(const uint32_t *a, const uint32_t *b, int k)
{
    int i;
    for (i = k - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    return 0;
}

/* a -= b; returns the borrow. */
static uint32_t bn_sub(uint32_t *a, const uint32_t *b, int k)
{
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < k; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

/* r = (2r + bit) mod n, r < n on entry. */
static void bn_shl1_mod(uint32_t *r, int bit, const uint32_t *n, int k)
{
    uint32_t carry = (uint32_t)bit;
    int i;
    for (i = 0; i < k; i++) {
        uint32_t top = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = top;
    }
    if (carry || bn_cmp(r, n, k) >= 0)
        bn_sub(r, n, k);
}

/* r = a mod n, a has alen limbs, n has k significant limbs. */
static void bn_mod(uint32_t *r, const uint32_t *a, int alen, const uint32_t *n, int k)
{
    int i, b;
    memset(r, 0, (size_t)k * 4);
    for (i = alen - 1; i >= 0; i--)
        for (b = 31; b >= 0; b--)
            bn_shl1_mod(r, (int)((a[i] >> b) & 1), n, k);
}

/* r = a * b mod n by shift and add, for an even modulus (no Montgomery). */
static void bn_mulmod_slow(uint32_t *r, const uint32_t *a, const uint32_t *b,
                           const uint32_t *n, int k)
{
    uint32_t t[XC_BN_MAX_LIMBS];
    int i, bit;
    memset(t, 0, (size_t)k * 4);
    for (i = k - 1; i >= 0; i--)
        for (bit = 31; bit >= 0; bit--) {
            bn_shl1_mod(t, 0, n, k);
            if ((b[i] >> bit) & 1) {
                uint64_t c = 0;
                int j;
                for (j = 0; j < k; j++) {
                    c += (uint64_t)t[j] + a[j];
                    t[j] = (uint32_t)c;
                    c >>= 32;
                }
                if (c || bn_cmp(t, n, k) >= 0)
                    bn_sub(t, n, k);
            }
        }
    memcpy(r, t, (size_t)k * 4);
}

/* Montgomery product r = a * b / 2^(32k) mod n (CIOS), n odd, a b < n. */
static void bn_montmul(uint32_t *r, const uint32_t *a, const uint32_t *b,
                       const uint32_t *n, uint32_t n0, int k)
{
    uint32_t t[XC_BN_MAX_LIMBS + 2];
    int i, j;
    memset(t, 0, (size_t)(k + 2) * 4);
    for (i = 0; i < k; i++) {
        uint64_t c = 0, s;
        uint32_t m;
        for (j = 0; j < k; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
            t[j] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k] = (uint32_t)s;
        t[k + 1] = (uint32_t)(s >> 32);
        m = t[0] * n0;
        s = (uint64_t)t[0] + (uint64_t)m * n[0];
        c = s >> 32;
        for (j = 1; j < k; j++) {
            s = (uint64_t)t[j] + (uint64_t)m * n[j] + c;
            t[j - 1] = (uint32_t)s;
            c = s >> 32;
        }
        s = (uint64_t)t[k] + c;
        t[k - 1] = (uint32_t)s;
        t[k] = t[k + 1] + (uint32_t)(s >> 32);
    }
    if (t[k] || bn_cmp(t, n, k) >= 0)
        bn_sub(t, n, k);
    memcpy(r, t, (size_t)k * 4);
}

/* r = b^e mod n. b has blen limbs, e has elen, n has nlen; r gets nlen
 * limbs. Returns 0 for a zero modulus or one wider than XC_BN_MAX_LIMBS. */
static int bn_modexp(uint32_t *r, const uint32_t *b, int blen, const uint32_t *e, int elen,
                     const uint32_t *n, int nlen)
{
    uint32_t base[XC_BN_MAX_LIMBS], x[XC_BN_MAX_LIMBS], one[XC_BN_MAX_LIMBS];
    int k = nlen, i, bit, started = 0;

    while (k > 0 && n[k - 1] == 0)
        k--;
    if (k == 0 || k > XC_BN_MAX_LIMBS)
        return 0;
    memset(r, 0, (size_t)nlen * 4);
    if (k == 1 && n[0] == 1)
        return 1;                       /* everything is 0 mod 1 */
    bn_mod(base, b, blen, n, k);
    memset(one, 0, (size_t)k * 4);
    one[0] = 1;

    if (n[0] & 1) {
        uint32_t inv = 1, n0, r2[XC_BN_MAX_LIMBS];
        for (i = 0; i < 5; i++)
            inv *= 2u - n[0] * inv;     /* Newton: inv = n[0]^-1 mod 2^32 */
        n0 = 0u - inv;
        memcpy(r2, one, (size_t)k * 4);  /* R^2 mod n by doubling 1, 64k times */
        for (i = 0; i < 64 * k; i++)
            bn_shl1_mod(r2, 0, n, k);
        bn_montmul(base, base, r2, n, n0, k);   /* to the Montgomery domain */
        bn_montmul(x, one, r2, n, n0, k);       /* 1 */
        for (i = elen - 1; i >= 0; i--)
            for (bit = 31; bit >= 0; bit--) {
                if (started)
                    bn_montmul(x, x, x, n, n0, k);
                if ((e[i] >> bit) & 1) {
                    bn_montmul(x, x, base, n, n0, k);
                    started = 1;
                }
            }
        bn_montmul(x, x, one, n, n0, k);        /* out of it */
        SecureZeroMemory(r2, sizeof r2);
    } else {
        memcpy(x, one, (size_t)k * 4);
        for (i = elen - 1; i >= 0; i--)
            for (bit = 31; bit >= 0; bit--) {
                if (started)
                    bn_mulmod_slow(x, x, x, n, k);
                if ((e[i] >> bit) & 1) {
                    bn_mulmod_slow(x, x, base, n, k);
                    started = 1;
                }
            }
    }
    memcpy(r, x, (size_t)k * 4);
    SecureZeroMemory(base, sizeof base);
    SecureZeroMemory(x, sizeof x);
    return 1;
}

static void bn_from_bytes(uint32_t *r, int k, const UCHAR *p, uint32_t nbytes)
{
    uint32_t i;
    memset(r, 0, (size_t)k * 4);
    for (i = 0; i < nbytes; i++)
        r[i / 4] |= (uint32_t)p[i] << (8 * (i % 4));
}

static void bn_to_bytes(UCHAR *p, uint32_t nbytes, const uint32_t *a)
{
    uint32_t i;
    for (i = 0; i < nbytes; i++)
        p[i] = (UCHAR)(a[i / 4] >> (8 * (i % 4)));
}

/* ── RSA over BSAFE keys ──────────────────────────────────────────────── */

typedef struct {
    uint32_t keylen, bitlen, nbytes, pubexp;
    const UCHAR *modulus;
} XcRsaKey;

static int xc_rsa_key(const UCHAR *key, uint32_t magic, XcRsaKey *out)
{
    if (!key || xc_rd32(key) != magic)
        return 0;
    out->keylen = xc_rd32(key + 4);
    out->bitlen = xc_rd32(key + 8);
    out->pubexp = xc_rd32(key + 16);
    out->nbytes = (out->bitlen + 7) / 8;
    out->modulus = key + 20;
    if (out->bitlen < 16 || out->nbytes > XC_BN_MAX_LIMBS * 4 || out->keylen < out->nbytes
        || out->keylen > XC_BN_MAX_LIMBS * 4 + 64)
        return 0;
    return 1;
}

/* out = in^exp mod modulus, nbytes each; exp is exp_bytes long. */
static int xc_rsa_op(const XcRsaKey *key, const UCHAR *exp, uint32_t exp_bytes,
                     const UCHAR *in, UCHAR *out)
{
    uint32_t n[XC_BN_MAX_LIMBS], e[XC_BN_MAX_LIMBS], m[XC_BN_MAX_LIMBS], r[XC_BN_MAX_LIMBS];
    int k = (int)((key->nbytes + 3) / 4);
    int ek = (int)((exp_bytes + 3) / 4);
    int ok;
    if (!in || !out || ek > XC_BN_MAX_LIMBS)
        return 0;
    bn_from_bytes(n, k, key->modulus, key->nbytes);
    bn_from_bytes(e, ek, exp, exp_bytes);
    bn_from_bytes(m, k, in, key->nbytes);
    ok = bn_modexp(r, m, k, e, ek, n, k);
    if (ok)
        bn_to_bytes(out, key->nbytes, r);
    SecureZeroMemory(e, sizeof e);
    SecureZeroMemory(m, sizeof m);
    SecureZeroMemory(r, sizeof r);
    return ok;
}

ULONG __stdcall xbox_XcPKGetKeyLen(PVOID PublicKey)
{
    XcRsaKey k;
    return xc_rsa_key((const UCHAR *)PublicKey, XC_RSA1_MAGIC, &k) ? k.keylen : 0;
}

ULONG __stdcall xbox_XcPKEncPublic(PVOID PublicKey, PVOID Input, PVOID Output)
{
    XcRsaKey k;
    UCHAR e[4];
    if (!xc_rsa_key((const UCHAR *)PublicKey, XC_RSA1_MAGIC, &k))
        return 0;
    e[0] = (UCHAR)k.pubexp;         e[1] = (UCHAR)(k.pubexp >> 8);
    e[2] = (UCHAR)(k.pubexp >> 16); e[3] = (UCHAR)(k.pubexp >> 24);
    return (ULONG)xc_rsa_op(&k, e, 4, (const UCHAR *)Input, (UCHAR *)Output);
}

ULONG __stdcall xbox_XcPKDecPrivate(PVOID PrivateKey, PVOID Input, PVOID Output)
{
    XcRsaKey k;
    const UCHAR *key = (const UCHAR *)PrivateKey;
    if (!xc_rsa_key(key, XC_RSA2_MAGIC, &k))
        return 0;
    /* modulus (keylen), 5 halves (keylen/2), then the private exponent. */
    return (ULONG)xc_rsa_op(&k, key + 20 + k.keylen + 5 * (k.keylen / 2), k.nbytes,
                            (const UCHAR *)Input, (UCHAR *)Output);
}

/* The real check: is Signature^e mod n the PKCS#1 v1.5 block of Digest? */
static int xc_pkcs1_check(const UCHAR *sig, const UCHAR *pub, const UCHAR *digest)
{
    /* DigestInfo prefixes for SHA-1, as on the wire (RFC 8017 9.2 note 1),
     * with and without the NULL parameters. */
    static const UCHAR di_null[15] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
                                      0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
    static const UCHAR di_bare[13] = {0x30, 0x1F, 0x30, 0x07, 0x06, 0x05, 0x2B, 0x0E,
                                      0x03, 0x02, 0x1A, 0x04, 0x14};
    UCHAR le[XC_BN_MAX_LIMBS * 4], em[XC_BN_MAX_LIMBS * 4];
    XcRsaKey k;
    uint32_t i, n, ps, t;
    if (!sig || !digest || !xbox_XcPKEncPublic((PVOID)pub, (PVOID)sig, le))
        return 0;
    xc_rsa_key(pub, XC_RSA1_MAGIC, &k);
    n = k.nbytes;
    for (i = 0; i < n; i++)             /* EM, most significant byte first */
        em[i] = le[n - 1 - i];
    if (n < 11 + 20 || em[0] != 0x00 || em[1] != 0x01)
        return 0;
    for (ps = 2; ps < n && em[ps] == 0xFF; ps++)
        ;
    if (ps - 2 < 8 || ps >= n || em[ps] != 0x00)
        return 0;
    t = ps + 1;                          /* T = [DigestInfo] || H */
    if (n - t == 20 + sizeof di_null && !memcmp(em + t, di_null, sizeof di_null))
        t += sizeof di_null;
    else if (n - t == 20 + sizeof di_bare && !memcmp(em + t, di_bare, sizeof di_bare))
        t += sizeof di_bare;
    return n - t == 20 && !memcmp(em + t, digest, 20);
}

/* Who calls this in a title's life, and why the answer is TRUE by default:
 *  - XAPI start-up checks the DVD security sector's signature (the sector is
 *    synthesised from the certificate, see bridge_dvd_security_sector, and
 *    has no signature: there is no disc and no Microsoft private key);
 *  - Xbox Live checks server certificates (no Live).
 * Both verify against Microsoft's key (XePublicKeyData), which is not in the
 * repo: ours is zeros. Saves and downloaded content are HMAC-SHA1 (XcHMAC),
 * not RSA. So a real verify can only reject what a player legitimately
 * has, and a failed verify sends the title back to the dashboard.
 * RECOMP_PKCS1_VERIFY=strict returns the real answer (a title that signs and
 * verifies with a key of its own); the default runs the same check, logs a
 * failure and accepts. */
BOOLEAN __stdcall xbox_XcVerifyPKCS1Signature(PVOID Signature, PVOID PublicKey, PVOID Digest)
{
    static volatile LONG s_logged;
    const char *mode = getenv("RECOMP_PKCS1_VERIFY");
    int ok = xc_pkcs1_check((const UCHAR *)Signature, (const UCHAR *)PublicKey,
                            (const UCHAR *)Digest);
    int strict = mode && !strcmp(mode, "strict");
    if (!ok && InterlockedIncrement(&s_logged) <= 8)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO,
                 "XcVerifyPKCS1Signature: signature does not verify (%s)%s",
                 xbox_XcPKGetKeyLen(PublicKey) ? "key present" : "no key",
                 strict ? ", rejected (RECOMP_PKCS1_VERIFY=strict)" : ", accepted");
    return (BOOLEAN)(ok || !strict);
}

ULONG __stdcall xbox_XcModExp(PULONG Result, PULONG Base, PULONG Exponent, PULONG Modulus, ULONG ModulusLength)
{
    uint32_t a[XC_BN_MAX_LIMBS], b[XC_BN_MAX_LIMBS], c[XC_BN_MAX_LIMBS], d[XC_BN_MAX_LIMBS];
    int k = (int)ModulusLength, ok;
    if (!Result || !Base || !Exponent || !Modulus || k <= 0 || k > XC_BN_MAX_LIMBS)
        return 0;
    /* PULONG is 32 bits on every host this builds for (the guest ABI). */
    memcpy(b, Base, (size_t)k * 4);
    memcpy(c, Exponent, (size_t)k * 4);
    memcpy(d, Modulus, (size_t)k * 4);
    ok = bn_modexp(a, b, k, c, k, d, k);
    if (ok)
        memcpy(Result, a, (size_t)k * 4);
    SecureZeroMemory(c, sizeof c);
    return (ULONG)ok;
}

/* ============================================================================
 * DES / triple DES (FIPS 46-3, NIST SP 800-67)
 *
 * Bit-level: the tables are the standard's, bit 1 the most significant. The
 * key table holds 16 round keys of 48 bits as 64-bit words (128 bytes), three
 * of them for triple DES (384 bytes): the sizes the XDK reserves.
 * ============================================================================ */

static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17,  9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41,  9, 49, 17, 57, 25};
static const uint8_t des_e[48] = {
    32,  1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9, 10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32,  1};
static const uint8_t des_p[32] = {
    16,  7, 20, 21, 29, 12, 28, 17,  1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9, 19, 13, 30,  6, 22, 11,  4, 25};
static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17,  9,  1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27, 19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,  7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29, 21, 13,  5, 28, 20, 12,  4};
static const uint8_t des_pc2[48] = {
    14, 17, 11, 24,  1,  5,  3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8, 16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
static const uint8_t des_shifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
static const uint8_t des_s[8][64] = {
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
      0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
     15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
      3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
     13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
     13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
      1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
     13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
      3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
     14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
     11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
     10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
      4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
     13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
      6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
      1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
      2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}};

#define XC_DES_TABLE_BYTES 128

/* Output bit i (1-based, MSB first, n bits) is input bit table[i-1] of an
 * in_bits-wide input. */
static uint64_t des_permute(uint64_t in, int in_bits, const uint8_t *table, int n)
{
    uint64_t out = 0;
    int i;
    for (i = 0; i < n; i++)
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1);
    return out;
}

static void des_schedule(uint64_t ks[16], const UCHAR key[8])
{
    uint64_t k = 0, cd;
    uint32_t c, d;
    int i;
    for (i = 0; i < 8; i++)
        k = (k << 8) | key[i];
    cd = des_permute(k, 64, des_pc1, 56);
    c = (uint32_t)(cd >> 28) & 0x0FFFFFFF;
    d = (uint32_t)cd & 0x0FFFFFFF;
    for (i = 0; i < 16; i++) {
        int s = des_shifts[i];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFF;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFF;
        ks[i] = des_permute(((uint64_t)c << 28) | d, 56, des_pc2, 48);
    }
}

static uint32_t des_f(uint32_t r, uint64_t k)
{
    uint64_t x = des_permute(r, 32, des_e, 48) ^ k;
    uint32_t out = 0;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned b = (unsigned)(x >> (42 - 6 * i)) & 0x3F;
        out = (out << 4) | des_s[i][((((b >> 4) & 2) | (b & 1)) << 4) | ((b >> 1) & 0xF)];
    }
    return (uint32_t)des_permute(out, 32, des_p, 32);
}

/* One block with a schedule; encrypt != 0 uses the round keys forwards. */
static void des_block(UCHAR out[8], const UCHAR in[8], const uint64_t ks[16], int encrypt)
{
    uint64_t v = 0;
    uint32_t l, r;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | in[i];
    v = des_permute(v, 64, des_ip, 64);
    l = (uint32_t)(v >> 32);
    r = (uint32_t)v;
    for (i = 0; i < 16; i++) {
        uint32_t t = r;
        r = l ^ des_f(r, ks[encrypt ? i : 15 - i]);
        l = t;
    }
    v = des_permute(((uint64_t)r << 32) | l, 64, des_fp, 64);
    for (i = 7; i >= 0; i--) {
        out[i] = (UCHAR)v;
        v >>= 8;
    }
}

static void xc_load_schedule(uint64_t ks[16], const UCHAR *table)
{
    memcpy(ks, table, XC_DES_TABLE_BYTES);    /* the guest table may be 4-aligned */
}

/* ECB on one 8-byte block, DES or EDE triple DES. */
static void xc_crypt_block(ULONG cipher, UCHAR out[8], const UCHAR in[8],
                           const UCHAR *table, ULONG op)
{
    uint64_t ks[16];
    UCHAR t[8];
    int enc = op != 0;
    if (cipher == 0) {
        xc_load_schedule(ks, table);
        des_block(out, in, ks, enc);
    } else {
        /* encrypt: E(K1) D(K2) E(K3); decrypt: D(K3) E(K2) D(K1). */
        xc_load_schedule(ks, table + (enc ? 0 : 2) * XC_DES_TABLE_BYTES);
        des_block(t, in, ks, enc);
        xc_load_schedule(ks, table + XC_DES_TABLE_BYTES);
        des_block(t, t, ks, !enc);
        xc_load_schedule(ks, table + (enc ? 2 : 0) * XC_DES_TABLE_BYTES);
        des_block(out, t, ks, enc);
    }
    SecureZeroMemory(ks, sizeof ks);
}

VOID __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength)
{
    ULONG i;
    if (!Key)
        return;
    for (i = 0; i < KeyLength; i++) {
        UCHAR b = Key[i] & 0xFE, p = b;
        p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
        Key[i] = b | (UCHAR)(~p & 1);     /* odd parity in the low bit */
    }
}

VOID __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR* Key)
{
    uint64_t ks[16];
    UCHAR *t = (UCHAR *)KeyTable;
    if (!t || !Key)
        return;
    des_schedule(ks, Key);
    memcpy(t, ks, XC_DES_TABLE_BYTES);
    if (CipherSelect != 0) {
        memcpy(t + 2 * XC_DES_TABLE_BYTES, ks, XC_DES_TABLE_BYTES);   /* K3 = K1 */
        des_schedule(ks, Key + 8);
        memcpy(t + XC_DES_TABLE_BYTES, ks, XC_DES_TABLE_BYTES);
    }
    SecureZeroMemory(ks, sizeof ks);
}

VOID __stdcall xbox_XcBlockCrypt(ULONG CipherSelect, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation)
{
    if (!Output || !Input || !KeyTable)
        return;
    xc_crypt_block(CipherSelect, (UCHAR *)Output, (const UCHAR *)Input,
                   (const UCHAR *)KeyTable, Operation);
}

/* Whole 8-byte blocks; a trailing partial block is left as it was. */
VOID __stdcall xbox_XcBlockCryptCBC(
    ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input,
    PVOID KeyTable, ULONG Operation, PVOID FeedbackVector)
{
    UCHAR *out = (UCHAR *)Output, *fb = (UCHAR *)FeedbackVector;
    const UCHAR *in = (const UCHAR *)Input;
    UCHAR x[8], c[8];
    ULONG off;
    int j;
    if (!out || !in || !KeyTable || !fb)
        return;
    if (OutputLength % 8)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO,
                 "XcBlockCryptCBC: length %lu is not a multiple of 8, tail left as is",
                 (unsigned long)OutputLength);
    for (off = 0; off + 8 <= OutputLength; off += 8) {
        if (Operation) {
            for (j = 0; j < 8; j++)
                x[j] = in[off + j] ^ fb[j];
            xc_crypt_block(CipherSelect, out + off, x, (const UCHAR *)KeyTable, 1);
            memcpy(fb, out + off, 8);
        } else {
            memcpy(c, in + off, 8);         /* Output may be Input */
            xc_crypt_block(CipherSelect, x, c, (const UCHAR *)KeyTable, 0);
            for (j = 0; j < 8; j++)
                out[off + j] = x[j] ^ fb[j];
            memcpy(fb, c, 8);
        }
    }
    SecureZeroMemory(x, sizeof x);
}

ULONG __stdcall xbox_XcCryptService(ULONG Operation, PVOID Param)
{
    (void)Operation;
    (void)Param;
    return 0;
}

/* The title's replacement entries are guest code: the kernel here is native
 * and computes the same standard algorithms, so it keeps its own and says
 * which entries a title tried to replace. The original vector (`rom`) is
 * filled by the bridge, which knows the guest addresses of the exports. */
VOID __stdcall xbox_XcUpdateCrypto(PVOID NewVector, PVOID RomVector)
{
    const UCHAR *v = (const UCHAR *)NewVector;
    unsigned mask = 0;
    int i;
    (void)RomVector;
    if (v)
        for (i = 0; i < 16; i++)
            if (xc_rd32(v + 4 * i))
                mask |= 1u << i;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO,
             "XcUpdateCrypto: title replaces entries 0x%04X (bit 0 = XcSHAInit); "
             "the kernel keeps its own", mask);
}
