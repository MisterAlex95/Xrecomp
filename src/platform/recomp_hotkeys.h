/* recomp_hotkeys -- the player's hotkeys: which pad chord or desktop key does fast-forward, quick save / load, the quick
 * slot, rewind (XBA-194), the in-game menu (XBA-190).
 *
 * An action takes a pad chord (a modifier held, then a button: « back+right_shoulder »; or a button alone, or none)
 * and a desktop key (« tab », « shift+f1 », or none). The defaults are the fixed chords of XBA-182:
 *
 *   action      pad (RECOMP_HOTKEYS)          key (RECOMP_HOTKEYS_KEYS)
 *   ff_hold     back+right_trigger            tab           fast-forward while held
 *   ff_toggle   none                          none          fast-forward on / off
 *   qsave       back+right_shoulder           none          quick save (the quick slot)
 *   qload       back+left_shoulder            none          quick load (the quick slot)
 *   slot_next   none                          none          the quick slot + 1 (4 -> 1)
 *   slot_prev   none                          none          the quick slot - 1 (1 -> 4)
 *   rewind      back+left_trigger             r             rewind while held (XBA-194, recomp_rewind.h)
 *   menu        back+start                    none          the in-game menu (the Android shell's, held ½ s)
 *   save1..4    none                          f1..f4        save slot N
 *   load1..4    none                          shift+f1..f4  load slot N
 *
 * Text form (RECOMP_HOTKEYS for the pad, RECOMP_HOTKEYS_KEYS for the keyboard, in recomp.cfg or the environment; the
 * Android app writes the pad's from its Controls screens, per game through the game's settings, XBA-188): entries
 * separated by commas, semicolons or spaces, each « <action>:<chord> » (or « = »), listing only what differs from the
 * defaults; « <action>:none » takes the action's chord away. Pad chord: <source>[+<source>], the sources of
 * console_pad.h (south east west north left_shoulder right_shoulder left_trigger right_trigger left_stick_click
 * right_stick_click back start dpad_up ... right_stick_right) or the short names a b x y lb rb lt rt l3 r3 select up
 * down left right; the first is the modifier. Key chord: [shift+|ctrl+|alt+]...<key>, keys a..z, 0..9, f1..f12, tab,
 * space, enter, escape, backspace, insert, delete, home, end, pageup, pagedown, up, down, left, right, minus, equals,
 * grave, comma, period, slash, semicolon, apostrophe, leftbracket, rightbracket, backslash. Example:
 * RECOMP_HOTKEYS=ff_toggle:back+north,qsave:back+dpad_right RECOMP_HOTKEYS_KEYS=ff_toggle:f9,qsave:f5,qload:f7.
 *
 * A pad chord fires once when both its buttons are down (in either order), holds while they are, and its buttons
 * reach the game as nothing while it is held (each alone still reaches the game). A button-alone chord does not fire
 * while a two-button chord holding that button does. A key chord fires when its key goes down with exactly its
 * modifiers held (Shift, Ctrl, Alt). The menu action is the Android shell's (MenuTriggers): the runtimes neither fire
 * nor swallow it.
 *
 * libc only (xbr_host_core); recomp_hotkeys_get may be called from any thread. */
#ifndef XBR_RECOMP_HOTKEYS_H
#define XBR_RECOMP_HOTKEYS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_HK_FF_HOLD, RECOMP_HK_FF_TOGGLE, RECOMP_HK_QSAVE, RECOMP_HK_QLOAD, RECOMP_HK_SLOT_NEXT, RECOMP_HK_SLOT_PREV,
    RECOMP_HK_REWIND, RECOMP_HK_MENU,
    RECOMP_HK_SAVE1, RECOMP_HK_SAVE2, RECOMP_HK_SAVE3, RECOMP_HK_SAVE4,
    RECOMP_HK_LOAD1, RECOMP_HK_LOAD2, RECOMP_HK_LOAD3, RECOMP_HK_LOAD4,
    RECOMP_HK_COUNT
};
#define RECOMP_HK_BIT(a) (1u << (a))

/* Pad sources: console_pad.h's CPS_* numbering (bit n of a source set), 24 of them. */
#define RECOMP_HK_SOURCES 24
/* A trigger counts as pressed past this (0..32767), as XBA-182's Back + right trigger. */
#define RECOMP_HK_TRIGGER 16384

/* Key modifiers. */
enum { RECOMP_HK_SHIFT = 1, RECOMP_HK_CTRL = 2, RECOMP_HK_ALT = 4 };

typedef struct recomp_pad_chord {
    int8_t mod;     /* a source, or -1: the button alone */
    int8_t button;  /* a source, or -1: no chord */
} recomp_pad_chord;

typedef struct recomp_key_chord {
    uint8_t mods;   /* RECOMP_HK_SHIFT | _CTRL | _ALT */
    uint16_t key;   /* a USB HID usage (= an SDL_Scancode), 0: no key */
} recomp_key_chord;

typedef struct recomp_hotkeys {
    recomp_pad_chord pad[RECOMP_HK_COUNT];
    recomp_key_chord key[RECOMP_HK_COUNT];
} recomp_hotkeys;

/* "ff_hold", ... or NULL; the action of a name, or -1. */
const char *recomp_hotkeys_action_name(int action);
int recomp_hotkeys_action(const char *name);
/* "back", ... (console_pad.h's names) or NULL; the source of a name (long or short), or -1. */
const char *recomp_hotkeys_source_name(int source);
int recomp_hotkeys_source(const char *name);
/* "f1", ... or NULL; the key (HID usage) of a name, or 0. */
const char *recomp_hotkeys_key_name(int key);
int recomp_hotkeys_key(const char *name);

void recomp_hotkeys_defaults(recomp_hotkeys *hk);
/* Apply a text over *hk (normally its defaults). Return the entries rejected (unknown action, source, key; a chord of
 * the same button twice), -1 without hk. NULL or "" changes nothing. */
int recomp_hotkeys_parse_pad(recomp_hotkeys *hk, const char *text);
int recomp_hotkeys_parse_keys(recomp_hotkeys *hk, const char *text);
/* What differs from the defaults, in the text form ("" for the defaults). */
const char *recomp_hotkeys_format_pad(const recomp_hotkeys *hk, char *buf, size_t size);
const char *recomp_hotkeys_format_keys(const recomp_hotkeys *hk, char *buf, size_t size);
/* One chord as text: "back+right_shoulder", "none"; "shift+f1", "none". */
const char *recomp_hotkeys_pad_chord_text(recomp_pad_chord c, char *buf, size_t size);
const char *recomp_hotkeys_key_chord_text(recomp_key_chord c, char *buf, size_t size);

/* The hotkeys of this run: the defaults, RECOMP_HOTKEYS and RECOMP_HOTKEYS_KEYS over them, read on the first call (one
 * log line "[INPUT] hotkeys: ..." when either is set). recomp_hotkeys_reset: read them again (tests). */
const recomp_hotkeys *recomp_hotkeys_get(void);
void recomp_hotkeys_reset(void);

/* One pad's or the keyboard's state between frames (zero it at the start). */
typedef struct recomp_hotkeys_state {
    uint32_t held;  /* the actions whose chord was held at the last update */
} recomp_hotkeys_state;

/* A pad, once per frame: sources = the CPS_* bits it holds now (console_pad_sources, triggers past
 * RECOMP_HK_TRIGGER). Returns the actions whose chord is held now (the menu's never); *pressed (may be NULL): those
 * whose chord went down this frame; *swallow (may be NULL): the sources the game must not see (the held chords'). */
uint32_t recomp_hotkeys_pad_update(const recomp_hotkeys *hk, recomp_hotkeys_state *st, uint32_t sources,
                                   uint32_t *pressed, uint32_t *swallow);

/* The keyboard, once per frame: keys[k] non-zero while HID usage k is down (SDL_GetKeyboardState), nkeys entries.
 * Returns the actions held now; *pressed (may be NULL): those whose key went down this frame. */
uint32_t recomp_hotkeys_keys_update(const recomp_hotkeys *hk, recomp_hotkeys_state *st, const uint8_t *keys, int nkeys,
                                    uint32_t *pressed);

/* Runs the actions pressed (recomp_states.h): quick save / load, the quick slot +/- 1, the fast-forward toggle,
 * save / load slot N. The holds are the caller's: recomp_ff_hold(held & RECOMP_HK_BIT(RECOMP_HK_FF_HOLD)), and
 * RECOMP_HK_REWIND for its rewinder (recomp_rewind.h).
 * Returns how many ran. */
int recomp_hotkeys_run(uint32_t pressed);

#ifdef __cplusplus
}
#endif

#endif /* XBR_RECOMP_HOTKEYS_H */
