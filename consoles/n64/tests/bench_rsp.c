/* RSP micro benchmark (N-5, XBA-173): RSP instructions per second on a vector-heavy microcode loop shaped like a
 * vertex transform / audio mix inner loop (double-precision multiply-accumulate chains, clamps, clips, a select,
 * reciprocals, quad loads and stores, two scalar ops and a jump), against the RSP's 62.5 MHz.
 *   n64_rsp_bench [seconds]      (default 2) */
#define _POSIX_C_SOURCE 199309L
#include "n64_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "homebrew/common/rsp_asm.h"

/* No RDP here. */
uint32_t n64_rdp_read_reg(n64_rdp *d, uint32_t reg) { (void)d; (void)reg; return 0; }
void n64_rdp_write_reg(n64_rdp *d, uint32_t reg, uint32_t v) { (void)d; (void)reg; (void)v; }
void n64_rdp_run(n64_rdp *d) { (void)d; }
void n64_rdp_attach_dmem(n64_rdp *d, const uint8_t *dmem) { (void)d; (void)dmem; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 2.0;
    static uint8_t rdram[1 << 20];
    n64_rcp_bus bus = {rdram, sizeof rdram, NULL, NULL, NULL};
    n64_rsp *s = n64_rsp_create(bus);
    uint8_t *m = n64_rsp_mem(s);
    const uint32_t prog[] = {
        R_ORI(2, 0, 0),
        R_LQV(1, 0, 0, 0), R_LQV(2, 0, 1, 0), R_LQV(3, 0, 2, 0), R_LQV(4, 0, 3, 0),
        /* loop: 0x14 */
        R_LQV(5, 0, 8, 2),
        R_LQV(6, 0, 9, 2),
        R_VOP(RV_VMUDL, 9, 5, 1, 8),
        R_VOP(RV_VMADM, 9, 6, 1, 8),
        R_VOP(RV_VMADN, 9, 5, 2, 9),
        R_VOP(RV_VMADH, 10, 6, 2, 9),
        R_VOP(RV_VMADN, 9, 5, 3, 10),
        R_VOP(RV_VMADH, 10, 6, 3, 10),
        R_VOP(RV_VMADN, 9, 5, 4, 11),
        R_VOP(RV_VMADH, 10, 6, 4, 11),
        R_VOP(RV_VMULF, 11, 10, 1, 4),
        R_VOP(RV_VMACF, 11, 9, 2, 5),
        R_VOP(RV_VADD, 12, 11, 10, 0),
        R_VOP(RV_VSUB, 13, 11, 9, 0),
        R_VOP(RV_VCH, 14, 12, 3, 3),
        R_VOP(RV_VCL, 14, 13, 4, 3),
        R_VOP(RV_VMRG, 15, 12, 13, 0),
        R_VOP(RV_VAND, 16, 15, 1, 0),
        R_VOP(RV_VRCPH, 17, 0, 10, 8),
        R_VOP(RV_VRCPL, 17, 0, 9, 8),
        R_VOP(RV_VRCPH, 17, 1, 0, 8),
        R_VOP(RV_VMULU, 18, 17, 16, 2),
        R_VOP(RV_VMACU, 18, 15, 14, 0),
        R_SQV(18, 0, 16, 2),
        R_SQV(15, 0, 17, 2),
        R_ADDIU(2, 2, 16),
        R_ANDI(2, 2, 0x3F0),
        R_J(0x14),
        R_VOP(RV_VXOR, 19, 19, 18, 0),
    };
    for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++)
        n64_wbe32(m + 0x1000 + 4 * i, prog[i]);
    for (unsigned i = 0; i < 0x1000; i++)
        m[i] = (uint8_t)(i * 37 + (i >> 3));
    n64_rsp_write_reg(s, 8, 0);
    n64_rsp_write_reg(s, 4, 0x1);

    uint64_t total = 0;
    double t0 = now(), t;
    do {
        total += n64_rsp_run(s, 1000000);
        t = now();
    } while (t - t0 < seconds);
    double ips = (double)total / (t - t0);
    printf("n64 rsp bench: %.1f M RSP instructions/s (%.2fx the RSP's 62.5 MHz), %llu instructions in %.2f s\n",
           ips / 1e6, ips / 62.5e6, (unsigned long long)total, t - t0);
    n64_rsp_destroy(s);
    return 0;
}
