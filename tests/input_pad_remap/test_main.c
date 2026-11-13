/* RECOMP_PAD_MAP (XBA-131) through the SDL backend's xbox_InputGetState, with a fake SDL: one controller whose
 * buttons and axes the test sets. Without the variable the fixed mapping is unchanged; with it, the remapped controls,
 * an unbound one, swapped sticks, a trigger as a button and a stick direction as a button. */
#define _POSIX_C_SOURCE 200809L
#include "xinput_xbox.h"
#include <SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SDL_Joystick { SDL_JoystickID instance_id; };
struct SDL_GameController { struct SDL_Joystick joystick; int attached; };

static struct SDL_GameController s_controller;
static int s_device_count = 1;
static int s_buttons[32];
static Sint16 s_axes[8];
static pthread_mutex_t s_joystick_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fail;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        s_fail = 1; \
    } \
} while (0)

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state) { (void)state; return 0; }
int SDL_SetHint(const char *n, const char *v) { (void)n; (void)v; return 1; }
Uint32 SDL_WasInit(Uint32 f) { (void)f; return SDL_INIT_GAMECONTROLLER; }
int SDL_InitSubSystem(Uint32 f) { (void)f; return 0; }
int SDL_NumJoysticks(void) { return s_device_count; }
int SDL_IsGameController(int i) { return i >= 0 && i < s_device_count; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int i) { return SDL_IsGameController(i) ? 7 : -1; }

SDL_GameController *SDL_GameControllerOpen(int i)
{
    if (!SDL_IsGameController(i))
        return NULL;
    s_controller.joystick.instance_id = 7;
    s_controller.attached = 1;
    return &s_controller;
}

void SDL_GameControllerClose(SDL_GameController *c) { if (c) c->attached = 0; }
int SDL_GameControllerGetAttached(SDL_GameController *c) { return c && c->attached; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *c) { return c ? &c->joystick : NULL; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { return j ? j->instance_id : -1; }
void SDL_GameControllerUpdate(void) {}
Uint8 SDL_GameControllerGetButton(SDL_GameController *c, int b) { (void)c; return (Uint8)s_buttons[b]; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *c, int a) { (void)c; return s_axes[a]; }

int SDL_GameControllerRumble(SDL_GameController *c, uint16_t l, uint16_t h, Uint32 d)
{
    (void)c; (void)l; (void)h; (void)d;
    return 0;
}

void SDL_LockJoysticks(void) { pthread_mutex_lock(&s_joystick_lock); }
void SDL_UnlockJoysticks(void) { pthread_mutex_unlock(&s_joystick_lock); }
int SDL_AtomicGet(SDL_atomic_t *v) { return atomic_load(&v->value); }
int SDL_AtomicSet(SDL_atomic_t *v, int d) { return atomic_exchange(&v->value, d); }

static void release_all(void)
{
    memset(s_buttons, 0, sizeof s_buttons);
    memset(s_axes, 0, sizeof s_axes);
}

static XBOX_GAMEPAD read_pad(void)
{
    XBOX_INPUT_STATE st;
    memset(&st, 0xAA, sizeof st);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    return st.Gamepad;
}

/* A fixed scene: A, L1, Start and D-pad left held, the left trigger half way, both sticks pushed. */
static void scene(void)
{
    release_all();
    s_buttons[SDL_CONTROLLER_BUTTON_A] = 1;
    s_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 1;
    s_buttons[SDL_CONTROLLER_BUTTON_START] = 1;
    s_buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = 1;
    s_axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 16384;
    s_axes[SDL_CONTROLLER_AXIS_LEFTX] = 32767;
    s_axes[SDL_CONTROLLER_AXIS_LEFTY] = -32768;     /* up */
    s_axes[SDL_CONTROLLER_AXIS_RIGHTX] = -1000;
    s_axes[SDL_CONTROLLER_AXIS_RIGHTY] = 2000;      /* down */
}

static int same_pad(const XBOX_GAMEPAD *a, const XBOX_GAMEPAD *b)
{
    return a->wButtons == b->wButtons && memcmp(a->bAnalogButtons, b->bAnalogButtons, 8) == 0
           && a->sThumbLX == b->sThumbLX && a->sThumbLY == b->sThumbLY
           && a->sThumbRX == b->sThumbRX && a->sThumbRY == b->sThumbRY;
}

int main(void)
{
    XBOX_GAMEPAD fixed, mapped, p;

    /* No RECOMP_PAD_MAP: the fixed mapping. */
    unsetenv("RECOMP_PAD_MAP");
    xbox_InputInit();
    scene();
    fixed = read_pad();
    CHECK(fixed.bAnalogButtons[XBOX_BUTTON_A] == 255 && fixed.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);
    CHECK(fixed.wButtons == (XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_LEFT));
    CHECK(fixed.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 128);
    CHECK(fixed.sThumbLX == 32767 && fixed.sThumbLY == 32767 && fixed.sThumbRX == -1000 && fixed.sThumbRY == -2001);

    /* An identity map (only what the defaults are) reads exactly the same, a full diagonal included. */
    setenv("RECOMP_PAD_MAP", "a=south,left_stick=left", 1);
    xbox_InputInit();
    scene();
    mapped = read_pad();
    CHECK(same_pad(&fixed, &mapped));
    s_axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 32767;
    s_axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 300;
    p = read_pad();
    CHECK(p.bAnalogButtons[XBOX_BUTTON_LTRIGGER] == 255 && p.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 2);

    /* A and B swapped, Start unbound, Black from the left trigger, White from the right stick pushed up, the
     * sticks swapped. */
    setenv("RECOMP_PAD_MAP", "a=east,b=south,start=none,black=left_trigger,white=right_stick_up,"
                             "left_stick=right,right_stick=left,bogus=1", 1);
    xbox_InputInit();
    scene();
    p = read_pad();
    CHECK(p.bAnalogButtons[XBOX_BUTTON_A] == 0 && p.bAnalogButtons[XBOX_BUTTON_B] == 255);
    CHECK(p.wButtons == XBOX_GAMEPAD_DPAD_LEFT);                  /* Start is unbound */
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);            /* the trigger at 50 % counts as pressed */
    CHECK(p.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);              /* the right stick points down */
    CHECK(p.sThumbLX == -1000 && p.sThumbLY == -2001);            /* the right stick drives the left */
    CHECK(p.sThumbRX == 32767 && p.sThumbRY == 32767);
    s_axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 8000;               /* under half: not pressed */
    s_axes[SDL_CONTROLLER_AXIS_RIGHTY] = -30000;                  /* right stick up: White */
    p = read_pad();
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 0 && p.bAnalogButtons[XBOX_BUTTON_WHITE] == 255);
    /* L1 is no longer Black (Black comes from the trigger now). */
    release_all();
    s_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 1;
    p = read_pad();
    CHECK(p.bAnalogButtons[XBOX_BUTTON_BLACK] == 0 && p.wButtons == 0);

    /* Empty again: back to the fixed mapping. */
    setenv("RECOMP_PAD_MAP", "", 1);
    xbox_InputInit();
    scene();
    p = read_pad();
    CHECK(same_pad(&fixed, &p));

    if (s_fail)
        return 1;
    puts("input_pad_remap: ok");
    return 0;
}
