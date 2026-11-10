/* A title linked into the shell (the stub APK, -PxbaTitleDir): its strong
 * definitions must win over pack_loader.c's weak forwarders. */
#include <stdint.h>

typedef void (*recomp_func_t)(void);

static int static_00011000(void) { return 0x5711; }

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x11000 ? (recomp_func_t)static_00011000 : 0;
}

int g_fe_cursor = 9;

int fe_menu_get_pb_state(void)
{
    return 6;
}

int recomp_title_main(const void *store)
{
    (void)store;
    return 77;
}
