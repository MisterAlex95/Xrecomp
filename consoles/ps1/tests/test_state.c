/* Save state refusals (XBA-182, ps1_state.c): a state saved from an HLE machine, then one field per case set out of
 * range in the blob (the machine's load delay register and SIO byte index, the GPU's command length, the CD-ROM's
 * queue head, an SPU pitch counter, the MDEC's block index and zigzag table, an HLE card file's first block). Each is
 * refused as corrupt by ps1_state_check and ps1_state_load, and the machine is unchanged (it saves the same bytes);
 * the values at the edge of each range pass the check; the state itself still loads. Our own code. */
#include "ps1_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test_state_fields.c: the private fields' offsets in their sections */
size_t state_off_gpu_cmd_len(void);
size_t state_off_cdrom_qhead(void);
size_t state_off_spu_counter0(void);
size_t state_off_mdec_blk_index(void);
size_t state_off_hle_file_dev(int i);
size_t state_off_hle_file_first(int i);

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

/* ps1_state.c's layout: a 40-byte header, then sections (id u32, size u32, bytes) in id order. */
enum { SEC_MACHINE = 1, SEC_RAM, SEC_GPU, SEC_CDROM, SEC_SPU, SEC_MDEC, SEC_HLE };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The offset of section id's bytes in the blob. */
static size_t section(const uint8_t *blob, size_t n, uint32_t id)
{
    size_t off = 40;
    while (off + 8 <= n) {
        uint32_t sid = get32(blob + off), len = get32(blob + off + 4);
        if (sid == id)
            return off + 8;
        off += 8 + len;
    }
    printf("FAIL: no section %u\n", id);
    exit(1);
}

static ps1_machine *M;
static uint8_t *orig, *work, *again;
static size_t size;

/* work = the state with an int-sized field at off set to v. */
static void set_int(size_t off, int32_t v)
{
    memcpy(work, orig, size);
    memcpy(work + off, &v, sizeof v);
}

/* The state in work is refused as corrupt, and the machine still saves orig. */
static void refused(const char *what)
{
    CHECK(ps1_state_check(M, work, size) == PS1_STATE_ECORRUPT, "%s: check refuses it", what);
    CHECK(ps1_state_load(M, work, size) == PS1_STATE_ECORRUPT, "%s: load refuses it", what);
    CHECK(ps1_state_save(M, again, size) == size && memcmp(again, orig, size) == 0, "%s: the machine is unchanged",
          what);
}

static void accepted(const char *what)
{
    CHECK(ps1_state_check(M, work, size) == PS1_STATE_OK, "%s: the check accepts it", what);
}

int main(void)
{
    char err[256] = "";
    M = ps1_create(NULL, 0, NULL, NULL, 0, err, sizeof err);
    if (!M) {
        printf("FAIL: ps1_create (HLE): %s\n", err);
        return 1;
    }
    for (int i = 0; i < 3; i++)
        ps1_run_frame(M);
    size = ps1_state_size(M);
    orig = malloc(size);
    work = malloc(size);
    again = malloc(size);
    if (!orig || !work || !again)
        return 1;
    CHECK(ps1_state_save(M, orig, size) == size, "the state is saved");
    size_t mach = section(orig, size, SEC_MACHINE), gpu = section(orig, size, SEC_GPU);
    size_t cd = section(orig, size, SEC_CDROM), spu = section(orig, size, SEC_SPU);
    size_t mdec = section(orig, size, SEC_MDEC), hle = section(orig, size, SEC_HLE);

    /* the machine */
    set_int(mach + offsetof(ps1_machine, ld_reg), 32);
    refused("ld_reg 32");
    set_int(mach + offsetof(ps1_machine, ld_reg), 31);
    accepted("ld_reg 31");
    set_int(mach + offsetof(ps1_machine, pend_reg), 0x10000);
    refused("pend_reg 0x10000");
    set_int(mach + offsetof(ps1_machine, sio.step), 141);
    refused("sio.step 141");
    set_int(mach + offsetof(ps1_machine, sio.step), 140);
    accepted("sio.step 140");
    set_int(mach + offsetof(ps1_machine, sio.step), -1);
    refused("sio.step -1");
    set_int(mach + offsetof(ps1_machine, sio.device), 1); /* a pad selected at byte 0 */
    refused("sio.device 1, step 0");
    set_int(mach + offsetof(ps1_machine, sio.device), 3);
    refused("sio.device 3");
    {
        /* a card read past its address check with the sector outside the card */
        memcpy(work, orig, size);
        int32_t dev = 2, step = 10;
        uint16_t sector = 0x400;
        memcpy(work + mach + offsetof(ps1_machine, sio.device), &dev, sizeof dev);
        memcpy(work + mach + offsetof(ps1_machine, sio.step), &step, sizeof step);
        work[mach + offsetof(ps1_machine, sio.cmd)] = 'R';
        memcpy(work + mach + offsetof(ps1_machine, sio.sector), &sector, sizeof sector);
        refused("card read, sector 0x400");
        sector = 0x3FF;
        memcpy(work + mach + offsetof(ps1_machine, sio.sector), &sector, sizeof sector);
        accepted("card read, sector 0x3FF");
    }
    set_int(mach + offsetof(ps1_machine, line), 1000);
    refused("line 1000");

    /* the devices */
    set_int(gpu + state_off_gpu_cmd_len(), 16);
    refused("GPU cmd_len 16");
    set_int(gpu + state_off_gpu_cmd_len(), 15);
    accepted("GPU cmd_len 15");
    set_int(cd + state_off_cdrom_qhead(), 8);
    refused("CD-ROM qhead 8");
    set_int(cd + state_off_cdrom_qhead(), 7);
    accepted("CD-ROM qhead 7");
    set_int(spu + state_off_spu_counter0(), 28 << 12);
    refused("SPU voice 0 counter 28 << 12");
    set_int(spu + state_off_spu_counter0(), (28 << 12) - 1);
    accepted("SPU voice 0 counter (28 << 12) - 1");
    set_int(mdec + state_off_mdec_blk_index(), 6);
    refused("MDEC blk_index 6");
    set_int(mdec + state_off_mdec_blk_index(), 5);
    accepted("MDEC blk_index 5");
    memcpy(work, orig, size);
    work[mdec + 1] = 64; /* zagzig[1] (the first field) */
    refused("MDEC zagzig[1] 64");

    /* the HLE BIOS: file 2 open on the card */
    set_int(hle + state_off_hle_file_dev(2), 3);
    int32_t first = 15;
    memcpy(work + hle + state_off_hle_file_first(2), &first, sizeof first);
    refused("HLE card file, first block 15");
    first = 14;
    memcpy(work + hle + state_off_hle_file_first(2), &first, sizeof first);
    accepted("HLE card file, first block 14");
    set_int(hle + state_off_hle_file_dev(2), 4);
    refused("HLE file device 4");

    /* the state itself */
    CHECK(ps1_state_load(M, orig, size) == PS1_STATE_OK, "the unmodified state loads");
    CHECK(ps1_state_save(M, again, size) == size && memcmp(again, orig, size) == 0, "and saves the same bytes");

    free(orig);
    free(work);
    free(again);
    ps1_destroy(M);
    if (failures) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
