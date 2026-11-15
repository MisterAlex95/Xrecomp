/* recomp_hotkeys -- the player's hotkeys (XBA-190, recomp_hotkeys.h). libc only. */
#include "recomp_hotkeys.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recomp_states.h"

static const char *const k_actions[RECOMP_HK_COUNT] = {
    "ff_hold", "ff_toggle", "qsave", "qload", "slot_next", "slot_prev", "menu",
    "save1", "save2", "save3", "save4", "load1", "load2", "load3", "load4",
};

/* console_pad.h's CPS_* order and names (tests/recomp_states checks they agree). */
static const char *const k_sources[RECOMP_HK_SOURCES] = {
    "south", "east", "west", "north", "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
    "left_stick_click", "right_stick_click", "back", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

static const struct { const char *name; int source; } k_short[] = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"lb", 4}, {"rb", 5}, {"lt", 6}, {"rt", 7}, {"l3", 8}, {"r3", 9},
    {"select", 10}, {"up", 12}, {"down", 13}, {"left", 14}, {"right", 15},
};

enum { SRC_LSHOULDER = 4, SRC_RSHOULDER = 5, SRC_RTRIGGER = 7, SRC_BACK = 10, SRC_START = 11 };

/* USB HID usages (SDL_Scancode). */
static const struct { const char *name; int key; } k_keys[] = {
    {"enter", 40}, {"return", 40}, {"escape", 41}, {"esc", 41}, {"backspace", 42}, {"tab", 43}, {"space", 44},
    {"minus", 45}, {"equals", 46}, {"leftbracket", 47}, {"rightbracket", 48}, {"backslash", 49},
    {"semicolon", 51}, {"apostrophe", 52}, {"grave", 53}, {"comma", 54}, {"period", 55}, {"slash", 56},
    {"insert", 73}, {"home", 74}, {"pageup", 75}, {"delete", 76}, {"end", 77}, {"pagedown", 78},
    {"right", 79}, {"left", 80}, {"down", 81}, {"up", 82},
};
#define HID_A 4
#define HID_1 30
#define HID_0 39
#define HID_F1 58
#define HID_F12 69
#define HID_LCTRL 224
#define HID_LSHIFT 225
#define HID_LALT 226
#define HID_RCTRL 228
#define HID_RSHIFT 229
#define HID_RALT 230

const char *recomp_hotkeys_action_name(int action)
{
    return action >= 0 && action < RECOMP_HK_COUNT ? k_actions[action] : NULL;
}

int recomp_hotkeys_action(const char *name)
{
    for (int i = 0; name && i < RECOMP_HK_COUNT; i++)
        if (!strcasecmp(name, k_actions[i]))
            return i;
    return -1;
}

const char *recomp_hotkeys_source_name(int source)
{
    return source >= 0 && source < RECOMP_HK_SOURCES ? k_sources[source] : NULL;
}

int recomp_hotkeys_source(const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < RECOMP_HK_SOURCES; i++)
        if (!strcasecmp(name, k_sources[i]))
            return i;
    for (size_t i = 0; i < sizeof k_short / sizeof k_short[0]; i++)
        if (!strcasecmp(name, k_short[i].name))
            return k_short[i].source;
    return -1;
}

const char *recomp_hotkeys_key_name(int key)
{
    static const char *const k_letters[26] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                              "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char *const k_digits[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char *const k_fn[12] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};
    if (key >= HID_A && key < HID_A + 26)
        return k_letters[key - HID_A];
    if (key >= HID_1 && key <= HID_0)
        return k_digits[key - HID_1];
    if (key >= HID_F1 && key <= HID_F12)
        return k_fn[key - HID_F1];
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].key == key)
            return k_keys[i].name;
    return NULL;
}

int recomp_hotkeys_key(const char *name)
{
    if (!name || !*name)
        return 0;
    if (!name[1] && isalpha((unsigned char)name[0]))
        return HID_A + tolower((unsigned char)name[0]) - 'a';
    if (!name[1] && isdigit((unsigned char)name[0]))
        return name[0] == '0' ? HID_0 : HID_1 + name[0] - '1';
    if ((name[0] == 'f' || name[0] == 'F') && isdigit((unsigned char)name[1])) {
        char *end;
        long n = strtol(name + 1, &end, 10);
        if (!*end && n >= 1 && n <= 12)
            return HID_F1 + (int)n - 1;
        return 0;
    }
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (!strcasecmp(name, k_keys[i].name))
            return k_keys[i].key;
    return 0;
}

void recomp_hotkeys_defaults(recomp_hotkeys *hk)
{
    if (!hk)
        return;
    memset(hk, 0, sizeof *hk);
    for (int i = 0; i < RECOMP_HK_COUNT; i++)
        hk->pad[i] = (recomp_pad_chord){-1, -1};
    hk->pad[RECOMP_HK_FF_HOLD] = (recomp_pad_chord){SRC_BACK, SRC_RTRIGGER};
    hk->pad[RECOMP_HK_QSAVE] = (recomp_pad_chord){SRC_BACK, SRC_RSHOULDER};
    hk->pad[RECOMP_HK_QLOAD] = (recomp_pad_chord){SRC_BACK, SRC_LSHOULDER};
    hk->pad[RECOMP_HK_MENU] = (recomp_pad_chord){SRC_BACK, SRC_START};
    hk->key[RECOMP_HK_FF_HOLD] = (recomp_key_chord){0, 43};
    for (int i = 0; i < 4; i++) {
        hk->key[RECOMP_HK_SAVE1 + i] = (recomp_key_chord){0, (uint16_t)(HID_F1 + i)};
        hk->key[RECOMP_HK_LOAD1 + i] = (recomp_key_chord){RECOMP_HK_SHIFT, (uint16_t)(HID_F1 + i)};
    }
}

/* Splits text into entries "<action>:<chord>" and hands each to one(); returns the entries rejected. */
static int parse_entries(recomp_hotkeys *hk, const char *text, int (*one)(recomp_hotkeys *, int, char *))
{
    if (!hk)
        return -1;
    if (!text)
        return 0;
    int bad = 0;
    const char *p = text;
    while (*p) {
        while (*p == ',' || *p == ';' || isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char entry[96];
        size_t n = 0;
        while (*p && *p != ',' && *p != ';' && !isspace((unsigned char)*p)) {
            if (n + 1 < sizeof entry)
                entry[n++] = *p;
            p++;
        }
        entry[n] = 0;
        char *sep = strpbrk(entry, ":=");
        if (!sep) {
            bad++;
            continue;
        }
        *sep = 0;
        int a = recomp_hotkeys_action(entry);
        if (a < 0 || one(hk, a, sep + 1) != 0)
            bad++;
    }
    return bad;
}

static int one_pad(recomp_hotkeys *hk, int a, char *chord)
{
    if (!strcasecmp(chord, "none") || !*chord) {
        hk->pad[a] = (recomp_pad_chord){-1, -1};
        return 0;
    }
    char *plus = strchr(chord, '+');
    int mod = -1, button;
    if (plus) {
        *plus = 0;
        mod = recomp_hotkeys_source(chord);
        button = recomp_hotkeys_source(plus + 1);
        if (mod < 0 || button < 0 || mod == button)
            return -1;
    } else {
        button = recomp_hotkeys_source(chord);
        if (button < 0)
            return -1;
    }
    hk->pad[a] = (recomp_pad_chord){(int8_t)mod, (int8_t)button};
    return 0;
}

static int one_key(recomp_hotkeys *hk, int a, char *chord)
{
    if (!strcasecmp(chord, "none") || !*chord) {
        hk->key[a] = (recomp_key_chord){0, 0};
        return 0;
    }
    uint8_t mods = 0;
    char *part = chord, *plus;
    while ((plus = strchr(part, '+')) != NULL) {
        *plus = 0;
        if (!strcasecmp(part, "shift"))
            mods |= RECOMP_HK_SHIFT;
        else if (!strcasecmp(part, "ctrl") || !strcasecmp(part, "control"))
            mods |= RECOMP_HK_CTRL;
        else if (!strcasecmp(part, "alt"))
            mods |= RECOMP_HK_ALT;
        else
            return -1;
        part = plus + 1;
    }
    int key = recomp_hotkeys_key(part);
    if (!key)
        return -1;
    hk->key[a] = (recomp_key_chord){mods, (uint16_t)key};
    return 0;
}

int recomp_hotkeys_parse_pad(recomp_hotkeys *hk, const char *text)
{
    return parse_entries(hk, text, one_pad);
}

int recomp_hotkeys_parse_keys(recomp_hotkeys *hk, const char *text)
{
    return parse_entries(hk, text, one_key);
}

const char *recomp_hotkeys_pad_chord_text(recomp_pad_chord c, char *buf, size_t size)
{
    if (!buf || !size)
        return "";
    if (c.button < 0)
        snprintf(buf, size, "none");
    else if (c.mod < 0)
        snprintf(buf, size, "%s", k_sources[c.button]);
    else
        snprintf(buf, size, "%s+%s", k_sources[c.mod], k_sources[c.button]);
    return buf;
}

const char *recomp_hotkeys_key_chord_text(recomp_key_chord c, char *buf, size_t size)
{
    if (!buf || !size)
        return "";
    const char *name = recomp_hotkeys_key_name(c.key);
    if (!name) {
        snprintf(buf, size, "none");
        return buf;
    }
    snprintf(buf, size, "%s%s%s%s", (c.mods & RECOMP_HK_CTRL) ? "ctrl+" : "", (c.mods & RECOMP_HK_ALT) ? "alt+" : "",
             (c.mods & RECOMP_HK_SHIFT) ? "shift+" : "", name);
    return buf;
}

static void append(char *buf, size_t size, size_t *len, const char *action, const char *chord)
{
    if (*len == (size_t)-1)   /* full: an entry did not fit */
        return;
    int n = snprintf(buf + *len, size - *len, "%s%s:%s", *len ? "," : "", action, chord);
    if (n > 0 && *len + (size_t)n < size) {
        *len += (size_t)n;
    } else {
        buf[*len] = 0;   /* whole entries only, and none after one that did not fit */
        *len = (size_t)-1;
    }
}

const char *recomp_hotkeys_format_pad(const recomp_hotkeys *hk, char *buf, size_t size)
{
    if (!buf || !size)
        return "";
    buf[0] = 0;
    if (!hk)
        return buf;
    recomp_hotkeys d;
    recomp_hotkeys_defaults(&d);
    size_t len = 0;
    for (int a = 0; a < RECOMP_HK_COUNT; a++) {
        if (hk->pad[a].button == d.pad[a].button && (hk->pad[a].mod == d.pad[a].mod || hk->pad[a].button < 0))
            continue;
        char c[64];
        append(buf, size, &len, k_actions[a], recomp_hotkeys_pad_chord_text(hk->pad[a], c, sizeof c));
    }
    return buf;
}

const char *recomp_hotkeys_format_keys(const recomp_hotkeys *hk, char *buf, size_t size)
{
    if (!buf || !size)
        return "";
    buf[0] = 0;
    if (!hk)
        return buf;
    recomp_hotkeys d;
    recomp_hotkeys_defaults(&d);
    size_t len = 0;
    for (int a = 0; a < RECOMP_HK_COUNT; a++) {
        if (hk->key[a].key == d.key[a].key && (hk->key[a].mods == d.key[a].mods || !hk->key[a].key))
            continue;
        char c[64];
        append(buf, size, &len, k_actions[a], recomp_hotkeys_key_chord_text(hk->key[a], c, sizeof c));
    }
    return buf;
}

/* ---- This run's ------------------------------------------------------------------------------------------------ */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static recomp_hotkeys s_hk;
static int s_loaded;

const recomp_hotkeys *recomp_hotkeys_get(void)
{
    pthread_mutex_lock(&s_mu);
    if (!s_loaded) {
        recomp_hotkeys_defaults(&s_hk);
        const char *pad = getenv("RECOMP_HOTKEYS"), *keys = getenv("RECOMP_HOTKEYS_KEYS");
        int bad = recomp_hotkeys_parse_pad(&s_hk, pad) + recomp_hotkeys_parse_keys(&s_hk, keys);
        if ((pad && *pad) || (keys && *keys)) {
            char a[512], b[512];
            recomp_hotkeys_format_pad(&s_hk, a, sizeof a);
            recomp_hotkeys_format_keys(&s_hk, b, sizeof b);
            printf("[INPUT] hotkeys: pad %s, keys %s", a[0] ? a : "(default)", b[0] ? b : "(default)");
            if (bad)
                printf(" (%d entr%s rejected)", bad, bad == 1 ? "y" : "ies");
            printf("\n");
            fflush(stdout);
        }
        s_loaded = 1;
    }
    pthread_mutex_unlock(&s_mu);
    return &s_hk;
}

void recomp_hotkeys_reset(void)
{
    pthread_mutex_lock(&s_mu);
    s_loaded = 0;
    pthread_mutex_unlock(&s_mu);
}

/* ---- Per frame ------------------------------------------------------------------------------------------------- */

uint32_t recomp_hotkeys_pad_update(const recomp_hotkeys *hk, recomp_hotkeys_state *st, uint32_t sources,
                                   uint32_t *pressed, uint32_t *swallow)
{
    uint32_t held = 0, eaten = 0, pair_buttons = 0;
    if (hk) {
        /* The two-button chords first: a button-alone chord on one of their buttons waits while they hold. */
        for (int a = 0; a < RECOMP_HK_COUNT; a++) {
            recomp_pad_chord c = hk->pad[a];
            if (a == RECOMP_HK_MENU || c.button < 0 || c.mod < 0)
                continue;
            uint32_t bits = (1u << c.mod) | (1u << c.button);
            if ((sources & bits) == bits) {
                held |= RECOMP_HK_BIT(a);
                eaten |= bits;
                pair_buttons |= 1u << c.button;
            }
        }
        for (int a = 0; a < RECOMP_HK_COUNT; a++) {
            recomp_pad_chord c = hk->pad[a];
            if (a == RECOMP_HK_MENU || c.button < 0 || c.mod >= 0)
                continue;
            uint32_t bit = 1u << c.button;
            if ((sources & bit) && !(pair_buttons & bit)) {
                held |= RECOMP_HK_BIT(a);
                eaten |= bit;
            }
        }
    }
    uint32_t was = st ? st->held : 0;
    if (st)
        st->held = held;
    if (pressed)
        *pressed = held & ~was;
    if (swallow)
        *swallow = eaten;
    return held;
}

uint32_t recomp_hotkeys_keys_update(const recomp_hotkeys *hk, recomp_hotkeys_state *st, const uint8_t *keys, int nkeys,
                                    uint32_t *pressed)
{
    uint32_t held = 0;
    if (hk && keys && nkeys > 0) {
#define KEY_DOWN(k) ((k) < nkeys && keys[(k)])
        uint8_t mods = (KEY_DOWN(HID_LSHIFT) || KEY_DOWN(HID_RSHIFT) ? RECOMP_HK_SHIFT : 0) |
                       (KEY_DOWN(HID_LCTRL) || KEY_DOWN(HID_RCTRL) ? RECOMP_HK_CTRL : 0) |
                       (KEY_DOWN(HID_LALT) || KEY_DOWN(HID_RALT) ? RECOMP_HK_ALT : 0);
        for (int a = 0; a < RECOMP_HK_COUNT; a++) {
            recomp_key_chord c = hk->key[a];
            if (a == RECOMP_HK_MENU || !c.key)
                continue;
            if (KEY_DOWN(c.key) && c.mods == mods)
                held |= RECOMP_HK_BIT(a);
        }
#undef KEY_DOWN
    }
    uint32_t was = st ? st->held : 0;
    if (st)
        st->held = held;
    if (pressed)
        *pressed = held & ~was;
    return held;
}

int recomp_hotkeys_run(uint32_t pressed)
{
    int n = 0;
    for (int a = 0; a < RECOMP_HK_COUNT; a++) {
        if (!(pressed & RECOMP_HK_BIT(a)))
            continue;
        switch (a) {
        case RECOMP_HK_FF_TOGGLE:
            recomp_ff_set(!recomp_ff_toggled());
            break;
        case RECOMP_HK_QSAVE:
            recomp_states_quick_save();
            break;
        case RECOMP_HK_QLOAD:
            recomp_states_quick_load();
            break;
        case RECOMP_HK_SLOT_NEXT:
            recomp_states_quick_step(1);
            break;
        case RECOMP_HK_SLOT_PREV:
            recomp_states_quick_step(-1);
            break;
        case RECOMP_HK_SAVE1: case RECOMP_HK_SAVE2: case RECOMP_HK_SAVE3: case RECOMP_HK_SAVE4:
            recomp_states_save(a - RECOMP_HK_SAVE1 + 1);
            break;
        case RECOMP_HK_LOAD1: case RECOMP_HK_LOAD2: case RECOMP_HK_LOAD3: case RECOMP_HK_LOAD4:
            recomp_states_load(a - RECOMP_HK_LOAD1 + 1);
            break;
        default:
            continue;   /* the hold and the menu are not run here */
        }
        n++;
    }
    return n;
}
