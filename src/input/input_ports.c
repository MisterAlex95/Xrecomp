/* Which host controller is which Xbox port: see input_ports.h. */
#include "input_ports.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void xbox_InputPortsKey(char *out, unsigned vendor, unsigned product)
{
    snprintf(out, INPUT_PORT_KEY, "%04x:%04x", vendor & 0xFFFFu, product & 0xFFFFu);
}

/* A "vvvv:pppp" key at s (9 characters, hex), copied lower case into out. */
static int read_key(const char *s, char *out)
{
    int i;

    for (i = 0; i < 9; i++) {
        if (i == 4 ? s[i] != ':' : !isxdigit((unsigned char)s[i]))
            return 0;
    }
    for (i = 0; i < 9; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[9] = 0;
    return 1;
}

int xbox_InputPortsInit(InputPorts *p, const char *pins, const char *builtin)
{
    const char *s = pins;
    int bad = 0, i;

    memset(p, 0, sizeof *p);
    for (i = 0; i < INPUT_PORTS; i++)
        p->id[i] = -1;
    if (builtin && !read_key(builtin, p->builtin))
        p->builtin[0] = 0;
    while (s && *s) {
        const char *end = strchr(s, ',');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        char key[INPUT_PORT_KEY];

        while (len && (*s == ' ' || *s == '\t')) {
            s++;
            len--;
        }
        if (len) {
            if (len >= 11 && s[0] >= '1' && s[0] <= '4' && s[1] == '=' && read_key(s + 2, key)
                && (len == 11 || s[11] == ':' || s[11] == ' '))
                memcpy(p->pin[s[0] - '1'], key, sizeof key);
            else
                bad++;
        }
        s = end ? end + 1 : NULL;
    }
    return bad;
}

static int is_free(const InputPorts *p, int port)
{
    return p->id[port] < 0;
}

static void take(InputPorts *p, int port, const char *key, int id)
{
    snprintf(p->owner[port], INPUT_PORT_KEY, "%s", key);
    p->id[port] = id;
}

void xbox_InputPortsStart(InputPorts *p, int n, const char *const keys[], const int ids[], int out_port[])
{
    int order[64], used[64], count = 0, i, k;

    if (n > 64)
        n = 64;
    for (i = 0; i < n; i++) {
        used[i] = 0;
        out_port[i] = -1;
    }
    /* Pinned, in the order of their ports. */
    for (k = 0; k < INPUT_PORTS; k++) {
        if (!p->pin[k][0])
            continue;
        for (i = 0; i < n; i++)
            if (!used[i] && strcmp(keys[i], p->pin[k]) == 0) {
                used[i] = 1;
                order[count++] = i;
                break;
            }
    }
    /* The built-in pad, then everybody else, in host order. */
    for (i = 0; i < n; i++)
        if (!used[i] && p->builtin[0] && strcmp(keys[i], p->builtin) == 0) {
            used[i] = 1;
            order[count++] = i;
        }
    for (i = 0; i < n; i++)
        if (!used[i]) {
            used[i] = 1;
            order[count++] = i;
        }
    for (k = 0; k < count && k < INPUT_PORTS; k++) {
        take(p, k, keys[order[k]], ids[order[k]]);
        out_port[order[k]] = k;
    }
    p->started = 1;
}

int xbox_InputPortsAttach(InputPorts *p, const char *key, int id)
{
    int pass, k;

    if (!p->started) {
        int port;
        xbox_InputPortsStart(p, 1, &key, &id, &port);
        return port;
    }
    for (pass = 0; pass < 5; pass++) {
        for (k = 0; k < INPUT_PORTS; k++) {
            int ok;
            if (!is_free(p, k))
                continue;
            switch (pass) {
            case 0:  ok = strcmp(p->owner[k], key) == 0; break;                    /* its own */
            case 1:  ok = !p->owner[k][0] && strcmp(p->pin[k], key) == 0; break;   /* pinned to it */
            case 2:  ok = !p->owner[k][0] && !p->pin[k][0]; break;                 /* nobody's */
            case 3:  ok = !p->owner[k][0]; break;                                  /* unused */
            default: ok = 1; break;                                                /* any free */
            }
            if (ok) {
                take(p, k, key, id);
                return k;
            }
        }
    }
    return -1;
}

int xbox_InputPortsDetach(InputPorts *p, int id)
{
    int k = xbox_InputPortsPortOf(p, id);

    if (k >= 0)
        p->id[k] = -1;   /* the owner stays: the port waits for it */
    return k;
}

int xbox_InputPortsPortOf(const InputPorts *p, int id)
{
    int k;

    if (id < 0)
        return -1;
    for (k = 0; k < INPUT_PORTS; k++)
        if (p->id[k] == id)
            return k;
    return -1;
}
