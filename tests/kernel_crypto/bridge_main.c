/* The Xc* crypto ordinals through the kernel bridge, the path a title takes:
 * guest buffers on the guest stack, stdcall pops, return values in eax.
 *
 *  - XcBlockCrypt (348): DES with the FIPS 81 example, every buffer a guest
 *    address;
 *  - XcModExp (345): returns 1 in eax;
 *  - XcCryptService (350): returns 0 in eax, pops two arguments;
 *  - XcUpdateCrypto (351): the original vector the title gets back holds the
 *    guest thunk address of each export it imported (XcSHAInit, XcBlockCrypt)
 *    and 0 for the others.
 * Same seam as tests/kernel_bridge (recomp_lookup_kernel). */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern ptrdiff_t g_xbox_mem_offset;

static int failures;
static uint8_t *mem;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

#define THUNK_VA 0x10000u
#define STACK_VA 0x20000u
#define M32(va) (*(uint32_t *)(mem + (va)))

static const uint32_t imports[] = {335, 345, 347, 348, 350, 351};
enum { I_SHAINIT, I_MODEXP, I_KEYTABLE, I_BLOCKCRYPT, I_SERVICE, I_UPDATE, N_IMPORTS };

/* Calls the import with stdcall arguments; returns eax. */
static uint32_t call(int import, const uint32_t *args, int nargs)
{
    recomp_func_t fn = recomp_lookup_kernel(M32(THUNK_VA + 4u * (uint32_t)import));
    int i;
    if (!fn) {
        printf("  import %d did not resolve\n", import);
        failures++;
        return 0xFFFFFFFFu;
    }
    M32(STACK_VA) = 0xBEEF0000u;
    for (i = 0; i < nargs; i++)
        M32(STACK_VA + 4u + 4u * (uint32_t)i) = args[i];
    g_esp = STACK_VA;
    g_eax = 0xDEADBEEFu;
    fn();
    if (g_esp != STACK_VA + 4u + 4u * (uint32_t)nargs) {
        printf("  import %d: esp 0x%08X, want 0x%08X\n", import, g_esp,
               STACK_VA + 4u + 4u * (uint32_t)nargs);
        failures++;
    }
    return g_eax;
}

int main(void)
{
    static const uint8_t key[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static const uint8_t want[8] = {0x3F, 0xA4, 0x0E, 0x8A, 0x98, 0x4D, 0x48, 0x15};
    const uint32_t VA_KEY = 0x30000, VA_TABLE = 0x30100, VA_IN = 0x30200, VA_OUT = 0x30300,
                   VA_A = 0x31000, VA_B = 0x31100, VA_C = 0x31200, VA_D = 0x31300,
                   VA_NEWV = 0x32000, VA_ROM = 0x32100;
    uint32_t args[7];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        printf("FAIL: no guest memory stand-in\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    for (i = 0; i < N_IMPORTS; i++)
        M32(THUNK_VA + 4u * (uint32_t)i) = 0x80000000u | imports[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_IMPORTS);
    xbox_kernel_bridge_init();

    memcpy(mem + VA_KEY, key, 8);
    memcpy(mem + VA_IN, "Now is t", 8);
    args[0] = 0; args[1] = VA_TABLE; args[2] = VA_KEY;
    call(I_KEYTABLE, args, 3);
    args[0] = 0; args[1] = VA_OUT; args[2] = VA_IN; args[3] = VA_TABLE; args[4] = 1;
    call(I_BLOCKCRYPT, args, 5);
    check(!memcmp(mem + VA_OUT, want, 8), "XcBlockCrypt through the bridge: FIPS 81 DES");

    M32(VA_B) = 65; M32(VA_C) = 17; M32(VA_D) = 3233;
    args[0] = VA_A; args[1] = VA_B; args[2] = VA_C; args[3] = VA_D; args[4] = 1;
    check(call(I_MODEXP, args, 5) == 1 && M32(VA_A) == 2790, "XcModExp through the bridge: 1 in eax");

    args[0] = 7; args[1] = 0;
    check(call(I_SERVICE, args, 2) == 0, "XcCryptService through the bridge: 0 in eax");

    memset(mem + VA_NEWV, 0, 64);
    M32(VA_NEWV) = 0x00123450u;                  /* a title's own XcSHAInit */
    memset(mem + VA_ROM, 0xCC, 64);
    args[0] = VA_NEWV; args[1] = VA_ROM;
    call(I_UPDATE, args, 2);
    check(M32(VA_ROM + 0) == M32(THUNK_VA + 4u * I_SHAINIT) && M32(VA_ROM) >= 0xFE000000u,
          "XcUpdateCrypto: rom XcSHAInit is its thunk address");
    check(M32(VA_ROM + 4 * 13) == M32(THUNK_VA + 4u * I_BLOCKCRYPT),
          "XcUpdateCrypto: rom XcBlockCrypt is its thunk address");
    check(M32(VA_ROM + 4 * 12) == M32(THUNK_VA + 4u * I_KEYTABLE),
          "XcUpdateCrypto: rom XcKeyTable is its thunk address");
    check(M32(VA_ROM + 4 * 1) == 0 && M32(VA_ROM + 4 * 9) == 0,
          "XcUpdateCrypto: an export not imported is 0");
    args[0] = VA_NEWV; args[1] = 0;
    call(I_UPDATE, args, 2);
    check(1, "XcUpdateCrypto without a rom vector");

    VirtualFree(mem, 0, MEM_RELEASE);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
