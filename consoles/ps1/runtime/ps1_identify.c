/* ps1_identify.c: ps1_disc as recomp_pack_rom_identify's disc hook (see ps1_identify.h). */
#include "ps1_identify.h"

#include "ps1_devices.h"
#include "recomp_pack.h"

#include <string.h>

static void *disc_open(const char *path)
{
    char err[160];
    return ps1_disc_open(path, err, sizeof err);
}

/* The 2048 user data bytes of a data sector: mode 1 at 16, mode 2 form 1 at 24 (a 2048-byte track comes back as
 * mode 2 form 1 with a synthesized header). */
static int disc_read(void *disc, uint32_t lba, unsigned char out[2048])
{
    uint8_t raw[2352];
    if (ps1_disc_read_sector((ps1_disc *)disc, lba, raw))
        return -1;
    memcpy(out, raw + (raw[15] == 1 ? 16 : 24), 2048);
    return 0;
}

static void disc_close(void *disc)
{
    ps1_disc_close((ps1_disc *)disc);
}

static const recomp_pack_disc_hook HOOK = { disc_open, disc_read, disc_close };

void ps1_identify_install(void)
{
    recomp_pack_set_disc_hook(&HOOK);
}
