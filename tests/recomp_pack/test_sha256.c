/* recomp_sha256 against the published vectors (FIPS 180-4 examples, NIST CAVP), on the plain C path and on the ARMv8
 * instructions where this host has them: whole, in pieces of every awkward size, from an unaligned buffer, and from a
 * file. The test states which paths it ran and fails when the instructions should have run and did not.
 *   recomp_sha256_test [SCRATCH_FILE] */
#define _GNU_SOURCE
#include "recomp_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAILED line %d: ", __LINE__);                                                                      \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static const struct {
    const char *text, *hex;
} vectors[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
     "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
};
#define MILLION_A_HEX "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

static const size_t pieces[] = {1, 3, 55, 56, 63, 64, 65, 119, 128, 977};

static void check_digest(const uint8_t sum[32], const char *want, const char *what, const char *path)
{
    char hex[65];
    recomp_sha256_hex(sum, hex);
    CHECK(!strcmp(hex, want), "%s (%s): %s, want %s", what, path, hex, want);
}

/* Every vector on one path: at once, in pieces, unaligned; and a million 'a'. */
static void check_path(recomp_sha256_accel *accel, int want_accel, const char *path)
{
    recomp_sha256_ctx c;
    uint8_t sum[32];
    recomp_sha256_init(&c, accel);
    CHECK(recomp_sha256_accelerated(&c) == want_accel, "path %s: accelerated = %d", path,
          recomp_sha256_accelerated(&c));

    for (size_t v = 0; v < sizeof vectors / sizeof vectors[0]; v++) {
        const char *text = vectors[v].text;
        size_t n = strlen(text);
        recomp_sha256(text, n, sum, accel);
        check_digest(sum, vectors[v].hex, "whole", path);

        for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
            recomp_sha256_init(&c, accel);
            for (size_t off = 0; off < n; off += pieces[p])
                recomp_sha256_update(&c, text + off, n - off < pieces[p] ? n - off : pieces[p]);
            recomp_sha256_final(&c, sum);
            check_digest(sum, vectors[v].hex, "pieces", path);
        }
        for (size_t shift = 1; shift < 8; shift++) { /* the input at every alignment */
            char buf[256];
            memcpy(buf + shift, text, n);
            recomp_sha256(buf + shift, n, sum, accel);
            check_digest(sum, vectors[v].hex, "unaligned", path);
        }
    }

    enum { MILLION = 1000000 };
    char *a = malloc(MILLION);
    if (!a) {
        CHECK(0, "out of memory");
        return;
    }
    memset(a, 'a', MILLION);
    recomp_sha256(a, MILLION, sum, accel);
    check_digest(sum, MILLION_A_HEX, "a million a, whole", path);
    for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
        recomp_sha256_init(&c, accel);
        for (size_t off = 0; off < MILLION; off += pieces[p])
            recomp_sha256_update(&c, a + off, MILLION - off < pieces[p] ? MILLION - off : pieces[p]);
        recomp_sha256_final(&c, sum);
        check_digest(sum, MILLION_A_HEX, "a million a, pieces", path);
    }
    free(a);
}

int main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : "recomp_sha256_test.tmp";
    int cpu_has = 0;
#if defined(__aarch64__) && defined(__linux__)
    cpu_has = (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#endif

    check_path(NULL, 0, "plain C");
    printf("recomp_sha256: plain C path checked\n");

    static recomp_sha256_accel off = RECOMP_SHA256_ACCEL("RECOMP_SHA256_TEST_OFF");
    setenv("RECOMP_SHA256_TEST_OFF", "1", 1);
    check_path(&off, 0, "instructions switched off");

    static recomp_sha256_accel on = RECOMP_SHA256_ACCEL("RECOMP_SHA256_TEST_ON");
    unsetenv("RECOMP_SHA256_TEST_ON");
    check_path(&on, cpu_has, cpu_has ? "ARMv8 instructions" : "plain C (no ARMv8 SHA-2 here)");
    printf("recomp_sha256: accelerated path %s\n", cpu_has ? "checked (ARMv8 SHA-2 instructions ran)"
                                                           : "not available on this host, plain C ran instead");

    FILE *f = fopen(scratch, "wb");
    CHECK(f != NULL, "cannot write %s", scratch);
    if (f) {
        char hex[65];
        fwrite(vectors[2].text, 1, strlen(vectors[2].text), f);
        fclose(f);
        CHECK(recomp_sha256_file(scratch, hex, &on) == 0 && !strcmp(hex, vectors[2].hex), "file: %s", hex);
        CHECK(recomp_sha256_file(scratch, hex, NULL) == 0 && !strcmp(hex, vectors[2].hex), "file, plain C: %s", hex);
        remove(scratch);
        CHECK(recomp_sha256_file(scratch, hex, NULL) == -1, "a missing file must fail");
    }

    if (failures) {
        printf("recomp_sha256: %d check(s) failed\n", failures);
        return 1;
    }
    printf("recomp_sha256: all checks passed\n");
    return 0;
}
