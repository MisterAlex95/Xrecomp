/* recomp_xdk_defaults (XBA-106): the XDK switches as the runtime's defaults
 * for a title no XDK profile layer matched, without overriding anything that
 * was already set (the environment, recomp.cfg, the layers). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_xdk_defaults.h"

#ifdef _WIN32
#define unsetenv(k) _putenv_s((k), "")
static int setenv(const char *k, const char *v, int o) { (void)o; return _putenv_s(k, v); }
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                                        failures++; } } while (0)

static void clear(void)
{
    const char *v;
    for (int i = 0; recomp_xdk_default_name(i, &v); i++)
        unsetenv(recomp_xdk_default_name(i, NULL));
    unsetenv("RECOMP_XDK_DEFAULTS");
}

static int is(const char *k, const char *v)
{
    const char *e = getenv(k);
    return e && !strcmp(e, v);
}

int main(void)
{
    const char *v;
    int n = 0;

    /* The table: what profiles/xdk/{xapi,dsound,d3d8} set, the same values. */
    while (recomp_xdk_default_name(n, NULL))
        n++;
    CHECK(n == 5);
    CHECK(recomp_xdk_default_name(n, &v) == NULL && recomp_xdk_default_name(-1, NULL) == NULL);

    /* Nothing set: all of them. */
    clear();
    CHECK(recomp_xdk_defaults() == 5);
    CHECK(is("RECOMP_DISPATCH_GATE", "1"));
    CHECK(is("RECOMP_VBLANK", "paced"));
    CHECK(is("RECOMP_USB", "1"));
    CHECK(is("RECOMP_KEYBOARD", "1"));
    CHECK(is("RECOMP_AC97_READY", "1"));
    /* Again: all set already, nothing to do (a title the layers matched). */
    CHECK(recomp_xdk_defaults() == 0);

    /* What was set wins, "0" included. */
    clear();
    setenv("RECOMP_VBLANK", "tick", 1);
    setenv("RECOMP_USB", "0", 1);
    CHECK(recomp_xdk_defaults() == 3);
    CHECK(is("RECOMP_VBLANK", "tick") && is("RECOMP_USB", "0") && is("RECOMP_DISPATCH_GATE", "1"));

    /* The A/B switch: none. */
    clear();
    setenv("RECOMP_XDK_DEFAULTS", "0", 1);
    CHECK(recomp_xdk_defaults() == 0);
    CHECK(!getenv("RECOMP_DISPATCH_GATE") || !*getenv("RECOMP_DISPATCH_GATE"));
    /* Any other value is on. */
    setenv("RECOMP_XDK_DEFAULTS", "1", 1);
    CHECK(recomp_xdk_defaults() == 5);

    if (failures)
        return 1;
    printf("recomp_xdk_defaults: ok\n");
    return 0;
}
