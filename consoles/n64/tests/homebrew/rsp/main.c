/* N64 rsp test (XBA-173): the RSP differential groups of common/rsp_oracle.h run on the console's RSP through its
 * register interface (SP memory at 0xA4000000, SP registers at 0xA4040000, SP_PC at 0xA4080000), one FNV-1a
 * checksum per group in table[0..92]. Run on ares, the checksums become the expected values of the host test
 * consoles/n64/tests/test_rsp.c (rsp_oracle_expected.h). No check here: PASS once every group ran.
 * Result protocol: README.md. Our own code. */
#include <libdragon.h>
#include <stdint.h>

#include "../common/xba_result.h"
#include "../common/rsp_oracle.h"

#define XSP_MEM ((volatile uint32_t *)0xA4000000u)
#define XSP_REG ((volatile uint32_t *)0xA4040000u)
#define XSP_PC ((volatile uint32_t *)0xA4080000u)

static uint8_t dma_buf[RSPO_DRAM_SIZE] __attribute__((aligned(16)));

static void io_wmem(void *ctx, uint32_t addr, uint32_t word)
{
    (void)ctx;
    XSP_MEM[(addr & 0x1FFC) >> 2] = word;
}

static uint32_t io_rmem(void *ctx, uint32_t addr)
{
    (void)ctx;
    return XSP_MEM[(addr & 0x1FFC) >> 2];
}

static void io_wreg(void *ctx, uint32_t reg, uint32_t v)
{
    (void)ctx;
    if (reg == 8)
        *XSP_PC = v;
    else
        XSP_REG[reg] = v;
}

static uint32_t io_rreg(void *ctx, uint32_t reg)
{
    (void)ctx;
    return reg == 8 ? *XSP_PC : XSP_REG[reg];
}

static void io_wdram(void *ctx, uint32_t off, uint32_t word)
{
    ((volatile uint32_t *)ctx)[off >> 2] = word;
}

static uint32_t io_rdram(void *ctx, uint32_t off)
{
    return ((volatile uint32_t *)ctx)[off >> 2];
}

static void io_run(void *ctx)
{
    (void)ctx;
    *XSP_PC = 0;
    XSP_REG[4] = 0x5; /* clear halt, clear broke */
    for (uint32_t spin = 0; spin < 10000000u && !(XSP_REG[4] & 1); spin++)
        ;
}

int main(void)
{
    volatile xba_result *r = XBA_RESULT;
    xba_begin(XBA_FOURCC('R', 'S', 'P', ' '));
    data_cache_hit_writeback_invalidate(dma_buf, sizeof dma_buf);
    rspo_io io = {
        .ctx = UncachedAddr(dma_buf),
        .wmem = io_wmem,
        .rmem = io_rmem,
        .wreg = io_wreg,
        .rreg = io_rreg,
        .wdram = io_wdram,
        .rdram = io_rdram,
        .run = io_run,
        .dram_base = PhysicalAddr(dma_buf),
    };
    XSP_REG[4] = 0x2; /* make sure the RSP is halted */
    for (uint32_t g = 0; g < RSPO_GROUPS; g++) {
        r->stage = g;
        xba_check(g, rspo_group(&io, g), 0);
    }
    r->detail = 0; /* the checksums are compared by the host test, not here */
    r->checksum = xba_fnv1a(r->table, RSPO_GROUPS);
    r->stage = RSPO_GROUPS;
    xba_finish();
}
