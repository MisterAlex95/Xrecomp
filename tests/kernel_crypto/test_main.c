/* The kernel's Xc* crypto against published known answers.
 *
 *  - DES: FIPS 46-3 worked example (key 133457799BBCDFF1) and FIPS 81's ECB
 *    and CBC examples ("Now is the time for all ");
 *  - two-key triple DES (EDE, K3 = K1), ECB and CBC: reference values from
 *    `openssl enc -des-ede-ecb / -des-ede-cbc -nopad` (OpenSSL's legacy
 *    provider), the same inputs;
 *  - XcDESKeyParity: odd parity, the key bits kept;
 *  - XcModExp: textbook RSA (n = 3233), an even modulus, and a 2048-bit case;
 *  - RSA on BSAFE keys: a 2048-bit key of our own (gen_vectors.py, Python's
 *    pow() is the reference), PKCS#1 v1.5 signatures with and without the
 *    DigestInfo, RECOMP_PKCS1_VERIFY accept / strict, and the all-zero key the
 *    runtime exports as XePublicKeyData. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsa_vectors.h"

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int failures;

static void hex(const char *s, UCHAR *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (UCHAR)v;
    }
}

static void expect(const char *what, const UCHAR *got, const char *want_hex, size_t n)
{
    UCHAR want[64];
    size_t i;
    hex(want_hex, want, n);
    if (memcmp(got, want, n)) {
        fprintf(stderr, "FAIL: %s: got ", what);
        for (i = 0; i < n; i++) fprintf(stderr, "%02x", got[i]);
        fprintf(stderr, ", want %s\n", want_hex);
        failures++;
    }
}

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void set_mode(const char *v)
{
#ifdef _WIN32
    _putenv_s("RECOMP_PKCS1_VERIFY", v ? v : "");
#else
    if (v) setenv("RECOMP_PKCS1_VERIFY", v, 1);
    else unsetenv("RECOMP_PKCS1_VERIFY");
#endif
}

static void test_des(void)
{
    UCHAR key[24], table[384], in[24], out[24], fb[8];
    const char *now = "Now is the time for all ";

    /* FIPS 46-3 / the classic worked example. */
    hex("133457799BBCDFF1", key, 8);
    hex("0123456789ABCDEF", in, 8);
    xbox_XcKeyTable(0, table, key);
    xbox_XcBlockCrypt(0, out, in, table, 1);
    expect("DES encrypt (133457799BBCDFF1)", out, "85E813540F0AB405", 8);
    xbox_XcBlockCrypt(0, out, out, table, 0);
    expect("DES decrypt in place", out, "0123456789ABCDEF", 8);

    /* FIPS 81 ECB and CBC. */
    hex("0123456789ABCDEF", key, 8);
    xbox_XcKeyTable(0, table, key);
    xbox_XcBlockCrypt(0, out, (PVOID)now, table, 1);
    expect("DES FIPS 81 ECB", out, "3FA40E8A984D4815", 8);
    hex("1234567890ABCDEF", fb, 8);
    xbox_XcBlockCryptCBC(0, 24, out, (PVOID)now, table, 1, fb);
    expect("DES FIPS 81 CBC", out, "E5C7CDDE872BF27C43E934008C389C0F683788499A7C05F6", 24);
    expect("CBC feedback is the last block", fb, "683788499A7C05F6", 8);
    hex("1234567890ABCDEF", fb, 8);
    xbox_XcBlockCryptCBC(0, 24, out, out, table, 0, fb);
    check("DES CBC decrypt in place", !memcmp(out, now, 24));
    /* Chained calls: two calls of 8 and 16 bytes give one 24-byte call. */
    hex("1234567890ABCDEF", fb, 8);
    xbox_XcBlockCryptCBC(0, 8, out, (PVOID)now, table, 1, fb);
    xbox_XcBlockCryptCBC(0, 16, out + 8, (PVOID)(now + 8), table, 1, fb);
    expect("DES CBC in two calls", out, "E5C7CDDE872BF27C43E934008C389C0F683788499A7C05F6", 24);

    /* Two-key triple DES (openssl -des-ede-*). */
    hex("0123456789ABCDEF23456789ABCDEF01", key, 16);
    memset(table, 0xCC, sizeof table);
    xbox_XcKeyTable(1, table, key);
    hex("0123456789ABCDEF", in, 8);
    xbox_XcBlockCrypt(1, out, in, table, 1);
    expect("3DES-EDE2 ECB", out, "A6BB373E196B375E", 8);
    xbox_XcBlockCrypt(1, out, out, table, 0);
    expect("3DES-EDE2 ECB decrypt", out, "0123456789ABCDEF", 8);
    hex("1234567890ABCDEF", fb, 8);
    xbox_XcBlockCryptCBC(1, 24, out, (PVOID)now, table, 1, fb);
    expect("3DES-EDE2 CBC", out, "134B98F8EEB3F6079F1A82E0640D5F2F8E090661C42864A1", 24);
    hex("1234567890ABCDEF", fb, 8);
    xbox_XcBlockCryptCBC(1, 24, out, out, table, 0, fb);
    check("3DES-EDE2 CBC decrypt", !memcmp(out, now, 24));
    /* K1 = K2: triple DES is single DES. */
    hex("0123456789ABCDEF0123456789ABCDEF", key, 16);
    xbox_XcKeyTable(1, table, key);
    xbox_XcBlockCrypt(1, out, (PVOID)now, table, 1);
    expect("3DES with K1 = K2 is DES", out, "3FA40E8A984D4815", 8);
}

static void test_parity(void)
{
    UCHAR k[256];
    int i, ok = 1;
    for (i = 0; i < 256; i++)
        k[i] = (UCHAR)i;
    xbox_XcDESKeyParity(k, 256);
    for (i = 0; i < 256; i++) {
        int bits = 0, b;
        for (b = 0; b < 8; b++)
            bits += (k[i] >> b) & 1;
        if (!(bits & 1) || (k[i] & 0xFE) != (i & 0xFE))
            ok = 0;
    }
    check("XcDESKeyParity: every byte odd, key bits kept", ok);
    hex("0123456789ABCDEF", k, 8);
    xbox_XcDESKeyParity(k, 8);
    expect("XcDESKeyParity on 0123456789ABCDEF", k, "0123456789ABCDEF", 8);
    hex("0022446688AACCEE", k, 8);
    xbox_XcDESKeyParity(k, 8);
    expect("XcDESKeyParity on 0022446688AACCEE", k, "0123456789ABCDEF", 8);
}

static void test_modexp(void)
{
    ULONG a[64], b[64], c[64], d[64];
    memset(b, 0, sizeof b); memset(c, 0, sizeof c); memset(d, 0, sizeof d);
    b[0] = 65; c[0] = 17; d[0] = 3233;
    check("XcModExp returns 1", xbox_XcModExp(a, b, c, d, 2) == 1);
    check("XcModExp 65^17 mod 3233 = 2790", a[0] == 2790 && a[1] == 0);
    b[0] = 2790; c[0] = 2753;
    xbox_XcModExp(a, b, c, d, 2);
    check("XcModExp 2790^2753 mod 3233 = 65", a[0] == 65);
    b[0] = 7; c[0] = 123; d[0] = 1024;
    xbox_XcModExp(a, b, c, d, 1);
    check("XcModExp even modulus 7^123 mod 1024 = 535", a[0] == 535);
    b[0] = 5000; c[0] = 1; d[0] = 3233;
    xbox_XcModExp(a, b, c, d, 1);
    check("XcModExp reduces a base above the modulus", a[0] == 5000 % 3233);
    d[0] = 0;
    check("XcModExp zero modulus fails", xbox_XcModExp(a, b, c, d, 1) == 0);

    memcpy(b, modexp_b, 256);
    memcpy(c, modexp_e, 256);
    memcpy(d, rsa_n, 256);
    check("XcModExp 2048-bit", xbox_XcModExp(a, b, c, d, 64) == 1 && !memcmp(a, modexp_r, 256));
}

/* BSAFE key blobs: "RSA1"/"RSA2", keylen = 264, bitlen 2048, datalen 255, e. */
static UCHAR pub[20 + 264];
static UCHAR prv[20 + 264 + 5 * 132 + 264];

static void put32(UCHAR *p, ULONG v)
{
    p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
}

static void make_keys(void)
{
    memset(pub, 0, sizeof pub);
    memcpy(pub, "RSA1", 4);
    put32(pub + 4, 264); put32(pub + 8, 2048); put32(pub + 12, 255); put32(pub + 16, 65537);
    memcpy(pub + 20, rsa_n, 256);
    memset(prv, 0, sizeof prv);
    memcpy(prv, pub, sizeof pub);
    memcpy(prv, "RSA2", 4);
    memcpy(prv + 20 + 264 + 5 * 132, rsa_d, 256);
}

/* The PKCS#1 v1.5 block for rsa_digest, least significant byte first. */
static void make_block(UCHAR em_le[256], int digest_info)
{
    static const UCHAR di[15] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
                                 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
    UCHAR be[256];
    int t = 256 - 20 - (digest_info ? 15 : 0), i;
    be[0] = 0; be[1] = 1;
    memset(be + 2, 0xFF, (size_t)(t - 3));
    be[t - 1] = 0;
    if (digest_info) memcpy(be + t, di, 15);
    memcpy(be + 236, rsa_digest, 20);
    for (i = 0; i < 256; i++)
        em_le[i] = be[255 - i];
}

static void test_rsa(void)
{
    UCHAR em[256], sig[256], out[256], bad[256], zero_key[284];
    make_keys();
    check("XcPKGetKeyLen gives keylen", xbox_XcPKGetKeyLen(pub) == 264);
    memset(zero_key, 0, sizeof zero_key);
    check("XcPKGetKeyLen of the zero key is 0", xbox_XcPKGetKeyLen(zero_key) == 0);

    make_block(em, 1);
    check("XcPKDecPrivate succeeds", xbox_XcPKDecPrivate(prv, em, sig) == 1);
    check("XcPKDecPrivate matches Python's signature", !memcmp(sig, rsa_sig, 256));
    check("XcPKEncPublic succeeds", xbox_XcPKEncPublic(pub, sig, out) == 1);
    check("XcPKEncPublic recovers the block", !memcmp(out, em, 256));
    check("XcPKEncPublic with the zero key fails", xbox_XcPKEncPublic(zero_key, sig, out) == 0);

    set_mode("strict");
    check("strict: the signature verifies",
          xbox_XcVerifyPKCS1Signature((PVOID)rsa_sig, pub, (PVOID)rsa_digest) == TRUE);
    make_block(em, 0);
    xbox_XcPKDecPrivate(prv, em, sig);
    check("strict: a signature without DigestInfo verifies",
          xbox_XcVerifyPKCS1Signature(sig, pub, (PVOID)rsa_digest) == TRUE);
    memcpy(bad, rsa_sig, 256);
    bad[17] ^= 0x40;
    check("strict: a corrupted signature is rejected",
          xbox_XcVerifyPKCS1Signature(bad, pub, (PVOID)rsa_digest) == FALSE);
    memcpy(bad, rsa_digest, 20);
    bad[0] ^= 1;
    check("strict: another digest is rejected",
          xbox_XcVerifyPKCS1Signature((PVOID)rsa_sig, pub, bad) == FALSE);
    check("strict: the zero key rejects",
          xbox_XcVerifyPKCS1Signature((PVOID)rsa_sig, zero_key, (PVOID)rsa_digest) == FALSE);
    /* A block with too little padding (PS < 8 bytes) is not PKCS#1. */
    memset(em, 0, sizeof em);
    em[255] = 0; em[254] = 1;
    xbox_XcPKDecPrivate(prv, em, sig);
    check("strict: an all-zero block is rejected",
          xbox_XcVerifyPKCS1Signature(sig, pub, (PVOID)rsa_digest) == FALSE);

    set_mode(NULL);
    memcpy(bad, rsa_sig, 256);
    bad[17] ^= 0x40;
    check("default: a failed check is accepted",
          xbox_XcVerifyPKCS1Signature(bad, pub, (PVOID)rsa_digest) == TRUE);
    check("default: the zero key accepts (the DVD sector check)",
          xbox_XcVerifyPKCS1Signature((PVOID)rsa_sig, zero_key, (PVOID)rsa_digest) == TRUE);
    set_mode("accept");
    check("accept: a failed check is accepted",
          xbox_XcVerifyPKCS1Signature(bad, pub, (PVOID)rsa_digest) == TRUE);
    set_mode(NULL);
}

int main(void)
{
    test_des();
    test_parity();
    test_modexp();
    test_rsa();
    check("XcCryptService returns 0", xbox_XcCryptService(0, NULL) == 0);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: DES / 3DES (FIPS 46-3, FIPS 81, openssl EDE2), key parity, XcModExp, "
           "RSA BSAFE keys and PKCS#1 v1.5 verify.\n");
    return 0;
}
