/* The XDK switches as toolkit defaults (XBA-106): recomp_xdk_defaults.h. */
#include "recomp_xdk_defaults.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static int set_default(const char *k, const char *v) { return getenv(k) ? 0 : _putenv_s(k, v) == 0; }
#else
static int set_default(const char *k, const char *v) { return getenv(k) ? 0 : setenv(k, v, 0) == 0; }
#endif

/* Each one the console's hardware, not a title's taste; what the XDK layers
 * set before they went (profiles/xdk/<name>/layer.json in the history):
 *   xapi    RECOMP_DISPATCH_GATE, RECOMP_VBLANK, RECOMP_USB, RECOMP_KEYBOARD
 *           (XPP locks by raising to DISPATCH_LEVEL, a lock on the one-CPU
 *           console only: without the gate the pad dies within a minute)
 *   dsound  RECOMP_AC97_READY (DirectSoundCreate waits for the codec)
 *   d3d8    RECOMP_VBLANK (RECOMP_PB_EXEC: already a default elsewhere; the
 *           D3D service, recomp_d3d_service.c, does the rest) */
static const char *const k_defaults[][2] = {
    {"RECOMP_DISPATCH_GATE", "1"},
    {"RECOMP_VBLANK", "paced"},
    {"RECOMP_USB", "1"},
    {"RECOMP_KEYBOARD", "1"},
    {"RECOMP_AC97_READY", "1"},
};

const char *recomp_xdk_default_name(int i, const char **value)
{
    if (i < 0 || i >= (int)(sizeof k_defaults / sizeof k_defaults[0]))
        return NULL;
    if (value)
        *value = k_defaults[i][1];
    return k_defaults[i][0];
}

int recomp_xdk_defaults(void)
{
    const char *off = getenv("RECOMP_XDK_DEFAULTS");
    char set[192] = "";
    int n = 0;

    if (off && *off == '0')
        return 0;
    for (int i = 0; i < (int)(sizeof k_defaults / sizeof k_defaults[0]); i++) {
        if (!set_default(k_defaults[i][0], k_defaults[i][1]))
            continue;
        n++;
        if (strlen(set) + strlen(k_defaults[i][0]) + strlen(k_defaults[i][1]) + 3 < sizeof set) {
            strcat(set, " ");
            strcat(set, k_defaults[i][0]);
            strcat(set, "=");
            strcat(set, k_defaults[i][1]);
        }
    }
    if (n)
        fprintf(stderr, "[XDK] defaults:%s (no profile layer set them; RECOMP_XDK_DEFAULTS=0 turns them off)\n",
                set);
    return n;
}
