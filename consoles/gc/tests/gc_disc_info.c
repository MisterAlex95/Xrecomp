/* gc_disc_info -- what the GameCube disc readers see in an image (XBA-261 tests): its title key from gc_disc and
 * from recomp_pack_rom_identify (with gc_identify's hook), the main DOL's size and CRC-32, every file of the FST with
 * its offset, size and CRC-32. stdout is the same for an image in every format (gc_tools.disc writes the expected
 * listing); the format and header go to stderr.
 *
 *   gc_disc_info IMAGE    exit 0, or 1 when the image does not read */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gc_disc.h"
#include "recomp_pack.h"

static uint32_t crc_table[256];

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int crc_range(gc_disc *d, uint64_t off, uint32_t n, uint32_t *crc)
{
    uint8_t *b = malloc(n ? n : 1);
    int rc = b ? gc_disc_read(d, off, b, n) : -1;
    if (!rc)
        *crc = crc32_of(b, n);
    free(b);
    return rc;
}

int main(int argc, char **argv)
{
    char why[200], key[48], key2[48];
    if (argc != 2) {
        fprintf(stderr, "usage: gc_disc_info IMAGE\n");
        return 2;
    }
    gc_disc *d = gc_disc_open(argv[1], why, sizeof why);
    if (!d) {
        fprintf(stderr, "gc_disc_info: %s: %s\n", argv[1], why);
        return 1;
    }
    gc_disc_header h;
    gc_fst fst;
    if (gc_disc_header_read(d, &h) || gc_disc_title_key(d, key, sizeof key) || gc_disc_fst(d, &fst)) {
        fprintf(stderr, "gc_disc_info: %s: %s\n", argv[1], gc_disc_error(d) ? gc_disc_error(d) : "?");
        gc_disc_close(d);
        return 1;
    }
    fprintf(stderr, "%s: %s image, disc of 0x%llX bytes, \"%s\", apploader %s entry %08X size 0x%X\n", argv[1],
            gc_disc_format(d), (unsigned long long)gc_disc_size(d), h.name, h.apploader_date, h.apploader_entry,
            h.apploader_size);
    printf("key %s\n", key);
    uint32_t dol = gc_disc_dol_size(d), crc = 0;
    /* the whole disc, padding included (RVZ regenerates it from seeds) */
    if (gc_disc_size(d) > 0x10000000u || crc_range(d, 0, (uint32_t)gc_disc_size(d), &crc)) {
        fprintf(stderr, "gc_disc_info: disc unreadable: %s\n", gc_disc_error(d) ? gc_disc_error(d) : "too big");
        return 1;
    }
    printf("disc %llX %08x\n", (unsigned long long)gc_disc_size(d), crc);
    if (!dol || crc_range(d, h.dol_off, dol, &crc)) {
        fprintf(stderr, "gc_disc_info: DOL unreadable\n");
        return 1;
    }
    printf("dol %u %08x\n", dol, crc);
    int rc = 0;
    for (size_t i = 1; i < fst.n; i++) {
        if (fst.e[i].dir)
            continue;
        if (crc_range(d, fst.e[i].offset, fst.e[i].size, &crc)) {
            fprintf(stderr, "gc_disc_info: %s: %s\n", fst.e[i].path, gc_disc_error(d) ? gc_disc_error(d) : "?");
            rc = 1;
            break;
        }
        printf("file %s %X %X %08x\n", fst.e[i].path, fst.e[i].offset, fst.e[i].size, crc);
    }
    gc_fst_free(&fst);
    gc_disc_close(d);
    /* the pack loader's view: recomp_pack_rom_identify (GCZ through the hook) */
    int console = -1;
    gc_disc_identify_install();
    if (recomp_pack_rom_identify(argv[1], &console, key2, sizeof key2) || console != RECOMP_CONSOLE_GC
        || strcmp(key, key2)) {
        fprintf(stderr, "gc_disc_info: recomp_pack_rom_identify says console %d key %s\n", console,
                console == RECOMP_CONSOLE_GC ? key2 : "-");
        rc = 1;
    }
    return rc;
}
