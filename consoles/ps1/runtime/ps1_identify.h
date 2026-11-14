/* ps1_identify: the PS1 disc reader (ps1_disc.c) as recomp_pack_rom_identify's disc hook, so a .chd PlayStation disc
 * gets its title key (SYSTEM.CNF's serial) wherever both are linked: libmain (xba_pack_rom_console), the launcher's
 * libxba_import (Console.identify through Ps1Native), ps1_play. recomp_pack.c itself stays libc-only (XBA-159). */
#ifndef PS1_IDENTIFY_H
#define PS1_IDENTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the hook (recomp_pack_set_disc_hook); idempotent. */
void ps1_identify_install(void);

#ifdef __cplusplus
}
#endif

#endif
