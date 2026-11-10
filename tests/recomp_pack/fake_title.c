/* A pack (libtitle.so), linked like tools/prepare links one: -Bsymbolic,
 * --no-undefined against the shell. Exports what a title exports. */
#include <stdint.h>

typedef void (*recomp_func_t)(void);
extern int toolkit_add(int a, int b);            /* in the shell */
extern uintptr_t toolkit_resolve(uint32_t va);   /* in the shell, calls back into us */

int g_fe_cursor = 3;

static int sub_00011000(void) { return 0x11000; }
static int manual_00022000(void) { return 0x22000; }

recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x11000 ? (recomp_func_t)sub_00011000 : 0;
}

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    return va == 0x22000 ? (recomp_func_t)manual_00022000 : 0;
}

int fe_menu_get_pb_state(void)
{
    return 5;
}

/* 100 + 1 from the shell, and the guest thread start path round trip
 * (shell -> our recomp_lookup) must find sub_00011000; the store's first
 * byte proves the argument got through. */
int recomp_title_main(const void *store)
{
    int r = toolkit_add(100, 1);
    if (toolkit_resolve(0x11000) != (uintptr_t)sub_00011000)
        return -2;
    g_fe_cursor = 4;   /* the title moves the cursor: the shell must see it */
    return store ? r + *(const unsigned char *)store : r;
}
