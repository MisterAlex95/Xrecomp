/* The touch overlay pad (input_overlay.h) through the SDL backend's xbox_InputGetState, with a fake SDL: one
 * controller whose buttons and axes the test sets, and a keyboard stand-in the test can turn on. */
#include "xinput_xbox.h"
#include "input_overlay.h"
#include <SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SDL_Joystick { SDL_JoystickID instance_id; };
struct SDL_GameController { struct SDL_Joystick joystick; int attached; };

static struct SDL_GameController s_controller;
static int s_device_count;
static int s_buttons[32];
static Sint16 s_axes[8];
static pthread_mutex_t s_joystick_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_fail;

/* The keyboard stand-in (keyboard.c): off unless the test sets s_kb_on. */
static int s_kb_on;
static XBOX_INPUT_STATE s_kb;

#define CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
        s_fail = 1; \
    } \
} while (0)

int xbox_InputKeyboardPoll(XBOX_INPUT_STATE *state)
{
    if (!s_kb_on)
        return 0;
    *state = s_kb;
    return 1;
}

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

static void attach(int on)
{
    SDL_LockJoysticks();
    s_device_count = on ? 1 : 0;
    if (!on)
        s_controller.attached = 0;
    SDL_UnlockJoysticks();
}

static XBOX_GAMEPAD pad(WORD buttons, int white, int black)
{
    XBOX_GAMEPAD p;
    memset(&p, 0, sizeof p);
    p.wButtons = buttons;
    p.bAnalogButtons[XBOX_BUTTON_WHITE] = white ? 255 : 0;
    p.bAnalogButtons[XBOX_BUTTON_BLACK] = black ? 255 : 0;
    return p;
}

/* A writer flipping between two complete pads while a reader polls: the reader must only ever see one of the two,
 * never half of each. */
static atomic_int s_stop;

static void *writer(void *unused)
{
    XBOX_GAMEPAD a = pad(XBOX_GAMEPAD_START, 1, 0), b = pad(XBOX_GAMEPAD_BACK, 0, 1);
    (void)unused;
    a.sThumbLX = 1111;
    b.sThumbLX = -2222;
    for (int i = 0; i < 200000 && !atomic_load(&s_stop); i++)
        xbox_InputOverlaySet(0, (i & 1) ? &b : &a);
    return NULL;
}

static void no_pad(void)
{
    XBOX_INPUT_STATE st;
    XBOX_GAMEPAD p;
    DWORD seq0, seq1;

    attach(0);
    xbox_InputOverlaySet(0, NULL);
    memset(&st, 0xAA, sizeof st);
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED);

    /* Active with nothing held: a connected pad at rest (the panel is up, touch-only player). */
    p = pad(0, 0, 0);
    xbox_InputOverlaySet(0, &p);
    memset(&st, 0xAA, sizeof st);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == 0);
    for (int i = 0; i < 8; i++)
        CHECK(st.Gamepad.bAnalogButtons[i] == 0);
    CHECK(st.Gamepad.sThumbLX == 0 && st.Gamepad.sThumbRY == 0);
    seq0 = st.dwPacketNumber;

    /* Start and White held: in the state, with a new packet number. */
    p = pad(XBOX_GAMEPAD_START, 1, 0);
    xbox_InputOverlaySet(0, &p);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 0);
    seq1 = st.dwPacketNumber;
    CHECK(seq1 != seq0);
    /* Unchanged overlay: the same packet (the game reads that as no change). */
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.dwPacketNumber == seq1);

    /* The other ports do not see port 0's overlay. */
    CHECK(xbox_InputGetState(1, &st) == ERROR_DEVICE_NOT_CONNECTED);
    /* Out-of-range ports are ignored, not written. */
    xbox_InputOverlaySet(-1, &p);
    xbox_InputOverlaySet(XBOX_MAX_CONTROLLERS, &p);
    CHECK(!xbox_InputOverlayGet(XBOX_MAX_CONTROLLERS, &p, NULL));

    /* Released (panel gone): no pad again. */
    xbox_InputOverlaySet(0, NULL);
    CHECK(!xbox_InputOverlayGet(0, &p, NULL));
    CHECK(p.wButtons == 0 && p.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);
    CHECK(xbox_InputGetState(0, &st) == ERROR_DEVICE_NOT_CONNECTED);
}

static void merge_over_pad(void)
{
    XBOX_INPUT_STATE st;
    XBOX_GAMEPAD p;

    memset(s_buttons, 0, sizeof s_buttons);
    memset(s_axes, 0, sizeof s_axes);
    s_buttons[SDL_CONTROLLER_BUTTON_A] = 1;
    s_buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 1;   /* L1 = Black */
    s_axes[SDL_CONTROLLER_AXIS_LEFTX] = 9000;
    attach(1);

    xbox_InputOverlaySet(0, NULL);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);

    /* Overlay: White and Back, a smaller stick deflection; the pad's buttons and stick stay. */
    p = pad(XBOX_GAMEPAD_BACK, 1, 0);
    p.sThumbLX = 100;
    xbox_InputOverlaySet(0, &p);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_BACK);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255);
    CHECK(st.Gamepad.sThumbLX == 9000);

    /* The pad's Start and the overlay's Start together: still Start (ORed), nothing doubled. */
    s_buttons[SDL_CONTROLLER_BUTTON_START] = 1;
    p = pad(XBOX_GAMEPAD_START, 0, 0);
    xbox_InputOverlaySet(0, &p);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_START);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);

    /* Released: only the pad. */
    s_buttons[SDL_CONTROLLER_BUTTON_START] = 0;
    xbox_InputOverlaySet(0, NULL);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == 0);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    attach(0);
}

static void keyboard_stand_in(void)
{
    XBOX_INPUT_STATE st;
    XBOX_GAMEPAD p;
    DWORD before;

    attach(0);
    memset(&s_kb, 0, sizeof s_kb);
    s_kb.dwPacketNumber = 40;
    s_kb.Gamepad.wButtons = XBOX_GAMEPAD_DPAD_UP;
    s_kb_on = 1;
    p = pad(0, 0, 1);
    xbox_InputOverlaySet(0, &p);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.wButtons == XBOX_GAMEPAD_DPAD_UP);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255);
    before = st.dwPacketNumber;
    p = pad(0, 0, 0);
    xbox_InputOverlaySet(0, &p);
    CHECK(xbox_InputGetState(0, &st) == ERROR_SUCCESS);
    CHECK(st.dwPacketNumber != before);
    CHECK(st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 0);
    s_kb_on = 0;
    xbox_InputOverlaySet(0, NULL);
}

static void concurrent(void)
{
    pthread_t t;
    XBOX_INPUT_STATE st;

    attach(0);
    CHECK(pthread_create(&t, NULL, writer, NULL) == 0);
    for (int i = 0; i < 200000; i++) {
        if (xbox_InputGetState(0, &st) != ERROR_SUCCESS)
            continue;   /* before the writer's first Set */
        int a = st.Gamepad.wButtons == XBOX_GAMEPAD_START && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 255
                && st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 0 && st.Gamepad.sThumbLX == 1111;
        int b = st.Gamepad.wButtons == XBOX_GAMEPAD_BACK && st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] == 255
                && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] == 0 && st.Gamepad.sThumbLX == -2222;
        if (!a && !b) {
            CHECK(a || b);
            break;
        }
    }
    atomic_store(&s_stop, 1);
    CHECK(pthread_join(t, NULL) == 0);
    xbox_InputOverlaySet(0, NULL);
}

int main(void)
{
    xbox_InputInit();
    no_pad();
    merge_over_pad();
    keyboard_stand_in();
    concurrent();
    if (!s_fail)
        puts("input overlay: ok");
    return s_fail;
}
