/* recomp_sha256 -- SHA-256 for the console packs (console_pack.h): a pack's library and a game's identity are hashed
 * with it at each start. Plain C, and the ARMv8 SHA-2 instructions where the caller asks for them and the CPU has them
 * (aarch64 Linux / Android, checked at run time). Both give the same digest. libc only. */
#ifndef RECOMP_SHA256_H
#define RECOMP_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A caller's choice of the instructions, decided once at its first hash. off_env: an environment variable that forces
 * plain C when it starts with '1', for tests (NULL: none). Declare it static with RECOMP_SHA256_ACCEL("NAME"). A NULL
 * recomp_sha256_accel * wherever one is taken means plain C always. */
typedef struct recomp_sha256_accel {
    const char *off_env;
    int have; /* -1 until decided */
} recomp_sha256_accel;

#define RECOMP_SHA256_ACCEL(off_env) {(off_env), -1}

typedef struct recomp_sha256_ctx {
    uint32_t h[8];
    uint8_t buf[64];
    size_t len;
    uint64_t total;
    void (*block)(uint32_t h[8], const uint8_t *p);
} recomp_sha256_ctx;

void recomp_sha256_init(recomp_sha256_ctx *c, recomp_sha256_accel *accel);
void recomp_sha256_update(recomp_sha256_ctx *c, const void *data, size_t n);
void recomp_sha256_final(recomp_sha256_ctx *c, uint8_t out[32]);
/* 1 when this context hashes with the instructions, 0 in plain C. */
int recomp_sha256_accelerated(const recomp_sha256_ctx *c);

void recomp_sha256(const void *data, size_t n, uint8_t out[32], recomp_sha256_accel *accel);
/* Lower-case hex of a digest (65 bytes with the NUL). */
void recomp_sha256_hex(const uint8_t sum[32], char hex[65]);
/* Lower-case hex of a file's SHA-256 into hex. Returns 0, or -1 when it cannot be read. */
int recomp_sha256_file(const char *path, char hex[65], recomp_sha256_accel *accel);

#ifdef __cplusplus
}
#endif

#endif
