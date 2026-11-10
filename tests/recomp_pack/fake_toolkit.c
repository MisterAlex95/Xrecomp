/* Stands in for the toolkit inside libmain.so: calls the title's symbols the
 * way kernel_bridge.c (thread start, APCs) and nv2a_pb_replay.c (menu state,
 * then the cursor) do, from inside the shell library. */
#include <stdint.h>

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t va);
extern recomp_func_t recomp_lookup_manual(uint32_t va);
extern int fe_menu_get_pb_state(void);
extern int g_fe_cursor;

/* What a guest thread start does: the generated dispatch, then the manual one. */
__attribute__((visibility("default"))) uintptr_t toolkit_resolve(uint32_t va)
{
    recomp_func_t fn = recomp_lookup(va);
    if (!fn)
        fn = recomp_lookup_manual(va);
    return (uintptr_t)fn;
}

/* Calls the resolved function (a title's recompiled code). */
__attribute__((visibility("default"))) int toolkit_run(uint32_t va)
{
    recomp_func_t fn = recomp_lookup(va);
    if (!fn)
        fn = recomp_lookup_manual(va);
    if (!fn)
        return -1;
    return ((int (*)(void))fn)();
}

/* One pushbuffer replay frame: menu state, then the cursor. */
__attribute__((visibility("default"))) int toolkit_pb_frame(int *cursor)
{
    int state = fe_menu_get_pb_state();
    *cursor = g_fe_cursor;
    return state;
}

/* A shell function a title calls (as titles call the kernel bridge). */
__attribute__((visibility("default"))) int toolkit_add(int a, int b)
{
    return a + b;
}
