/* gc_identify.c -- gc_disc as recomp_pack_rom_identify's GameCube hook (XBA-261): the formats libc alone cannot
 * read (GCZ). Its own target (gc_identify), so gc_core does not depend on the pack loader. */
#include "gc_disc.h"
#include "recomp_pack.h"

static int gc_header(const char *path, unsigned char hdr[0x20])
{
    char why[160];
    gc_disc *d = gc_disc_open(path, why, sizeof why);
    if (!d)
        return -1;
    int rc = gc_disc_read(d, 0, hdr, 0x20);
    gc_disc_close(d);
    return rc;
}

void gc_disc_identify_install(void)
{
    recomp_pack_set_gc_hook(gc_header);
}
