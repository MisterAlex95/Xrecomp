#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static uint8_t *mem;

#define CERT_VA   0x11000u   /* a synthetic certificate in the image header */
#define DATA_VA   0x31000u
#define SECTOR_LEN 1636u     /* 4-byte header + 1632-byte security sector */

/* The certificate the "title" carries: `size` bytes long, `flags` at +0x1D8,
 * time stamp 0x12345678 at +4. size 0 removes the certificate pointer. */
static void set_cert(uint32_t size, uint32_t flags)
{
    memset(mem + CERT_VA, 0, 0x200);
    *(uint32_t *)(mem + 0x10118) = size ? CERT_VA : 0;
    *(uint32_t *)(mem + CERT_VA + 0)     = size;
    *(uint32_t *)(mem + CERT_VA + 4)     = 0x12345678;
    *(uint32_t *)(mem + CERT_VA + 0x1D8) = flags;
}

/* NtDeviceIoControlFile (ordinal 196) on the synthetic DVD handle, with a
 * 44-byte SCSI_PASS_THROUGH_DIRECT whose CDB opcode is `opcode` and whose
 * DataBuffer (`len` bytes) starts out filled with 0xCC. Returns 1 when the
 * call returned STATUS_SUCCESS with ScsiStatus GOOD. */
static int scsi(uint8_t opcode, uint32_t len)
{
    uint32_t *sp;

    memset(mem + 0x30000, 0, 44);
    mem[0x30000 + 2] = 0x02;                            /* ScsiStatus: CHECK CONDITION */
    *(uint32_t *)(mem + 0x30000 + 12) = len;            /* DataTransferLength */
    *(uint32_t *)(mem + 0x30000 + 20) = DATA_VA;        /* DataBuffer */
    mem[0x30000 + 28] = opcode;
    memset(mem + DATA_VA, 0xCC, 0x1000);

    sp = (uint32_t *)(mem + 0x20000);
    memset(sp, 0, 64);
    sp[0] = 0xBEEF0001;
    sp[1] = 0xDECD0001;                                 /* synthetic DVD handle */
    sp[5] = 0x32000;                                    /* IoStatusBlock */
    sp[6] = 0x4D014;                                    /* IOCTL_SCSI_PASS_THROUGH_DIRECT */
    sp[7] = 0x30000;                                    /* input */
    sp[8] = 44;
    g_esp = 0x20000;
    recomp_lookup_kernel(*(uint32_t *)(mem + 0x10000))();

    if (g_eax != 0 || g_esp != 0x2002C) {
        fprintf(stderr, "FAIL status=%08X esp=%08X\n", g_eax, g_esp);
        return 0;
    }
    if (mem[0x30000 + 2] != 0) { puts("FAIL ScsiStatus not GOOD"); return 0; }
    return 1;
}

static int zeroed(uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        if (mem[DATA_VA + i]) return 0;
    return mem[DATA_VA + len] == 0xCC;                  /* nothing past the buffer */
}

int main(void)
{
    /* SHA-1 of the dword 0x4CB followed by 0x4CB bytes that are all zero
     * except 78 56 34 12 at sector offset 0x4AB (computed with hashlib). */
    static const uint8_t expect_digest[20] = {
        0x06, 0x21, 0xB5, 0xB8, 0x00, 0x00, 0xA0, 0xA6, 0x41, 0x3C,
        0x46, 0x99, 0x0D, 0x17, 0xE7, 0x82, 0x21, 0x5D, 0x04, 0x2C,
    };
    uint32_t i;
    int ok = 1;

    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return 10;
    g_xbox_mem_offset = (ptrdiff_t)mem;
    *(uint32_t *)(mem + 0x10000) = 0x800000C4;          /* ordinal 196 */
    xbox_kernel_set_thunk_address(0x10000, 1);
    xbox_kernel_bridge_init();

    /* 1. No certificate: READ DVD STRUCTURE, 64 bytes -> GOOD, zeroed. */
    set_cert(0, 0);
    if (!scsi(0xAD, 64) || !zeroed(64)) { puts("FAIL 64-byte 0xAD not zeroed"); ok = 0; }

    /* 2. Certificate with the check enabled (bit 0 at +0x1D8): the 1636-byte
     *    read gets the security sector that matches it. */
    set_cert(0x1EC, 3);
    if (!scsi(0xAD, SECTOR_LEN)) ok = 0;
    if (*(uint32_t *)(mem + DATA_VA + 0x4AB) != 0x12345678) {
        puts("FAIL sector time stamp is not the certificate's"); ok = 0;
    }
    if (memcmp(mem + DATA_VA + 0x4CF, expect_digest, 20) != 0) {
        puts("FAIL sector digest"); ok = 0;
    }
    for (i = 0; i < SECTOR_LEN; i++) {
        if (i >= 0x4AB && i < 0x4AF) continue;
        if (i >= 0x4CF && i < 0x4E3) continue;
        if (mem[DATA_VA + i]) { printf("FAIL sector byte 0x%X not zero\n", i); ok = 0; break; }
    }
    if (mem[DATA_VA + SECTOR_LEN] != 0xCC) { puts("FAIL wrote past the sector"); ok = 0; }

    /* 3. Same certificate, a buffer too short for the sector -> zeroed. */
    if (!scsi(0xAD, 64) || !zeroed(64)) { puts("FAIL short 0xAD with cert not zeroed"); ok = 0; }

    /* 4. Same certificate, another command -> zeroed. */
    if (!scsi(0x12, SECTOR_LEN) || !zeroed(SECTOR_LEN)) { puts("FAIL INQUIRY not zeroed"); ok = 0; }

    /* 5. Check disabled (bit 0 clear) -> zeroed. */
    set_cert(0x1EC, 2);
    if (!scsi(0xAD, SECTOR_LEN) || !zeroed(SECTOR_LEN)) { puts("FAIL bit 0 clear not zeroed"); ok = 0; }

    /* 6. Certificate too short to have the field (0x1D0 bytes, an older XDK)
     *    even with junk where +0x1D8 would be -> zeroed. */
    set_cert(0x1D0, 1);
    if (!scsi(0xAD, SECTOR_LEN) || !zeroed(SECTOR_LEN)) { puts("FAIL short certificate not zeroed"); ok = 0; }

    VirtualFree(mem, 0, MEM_RELEASE);
    if (!ok) return 1;
    puts("PASS: DVD SCSI pass-through: zeroed DataBuffer, security sector from the certificate");
    return 0;
}
