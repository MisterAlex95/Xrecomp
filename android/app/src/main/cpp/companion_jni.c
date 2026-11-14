/* The companion panel's JNI (fr.adserver.xrecomp.CompanionNative, docs/android/app-shell.md « Dual screen »):
 * the touch overlay pad (src/input/input_overlay.h for an Xbox game, and the same pad as host_pad's overlay for the
 * GBA, PS1 and N64 runtimes, which read it through console_pad_read), the window's frame stats, a screenshot of the
 * last frame (both from the Xbox presenter, or from host_window while a GBA / PS1 / N64 game has it open, XBA-181),
 * and quit. Called on the UI thread (the overlay, quit) or a worker thread (the screenshot). */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_pad.h"
#include "host_window.h"
#include "input_overlay.h"
#include "platform/recomp_quit.h"
#include "video/video_player.h"

#define JNI_FN(name) Java_fr_adserver_xrecomp_CompanionNative_##name

static int16_t neg16(SHORT v)
{
    return (int16_t)(v <= -32767 ? 32767 : -v);
}

/* The Xbox-shaped overlay pad as a host pad (host_pad.h, SDL's layout): A B X Y by position, Black / White as the
 * shoulders (the Xbox mapping's L1 / R1), the triggers 0..32767, the sticks with +y down. A console runtime maps it
 * with its console's default mapping (console_pad_merge), so a GBA layout's « L » (Black) is the GBA's L. */
static void overlay_both(const XBOX_GAMEPAD *pad)
{
    static const struct { int analog; uint32_t host; } k_analog[6] = {
        {XBOX_BUTTON_A, HOST_PAD_A}, {XBOX_BUTTON_B, HOST_PAD_B}, {XBOX_BUTTON_X, HOST_PAD_X},
        {XBOX_BUTTON_Y, HOST_PAD_Y}, {XBOX_BUTTON_BLACK, HOST_PAD_LSHOULDER}, {XBOX_BUTTON_WHITE, HOST_PAD_RSHOULDER},
    };
    static const struct { WORD xbox; uint32_t host; } k_bits[8] = {
        {XBOX_GAMEPAD_DPAD_UP, HOST_PAD_DPAD_UP}, {XBOX_GAMEPAD_DPAD_DOWN, HOST_PAD_DPAD_DOWN},
        {XBOX_GAMEPAD_DPAD_LEFT, HOST_PAD_DPAD_LEFT}, {XBOX_GAMEPAD_DPAD_RIGHT, HOST_PAD_DPAD_RIGHT},
        {XBOX_GAMEPAD_START, HOST_PAD_START}, {XBOX_GAMEPAD_BACK, HOST_PAD_BACK},
        {XBOX_GAMEPAD_LEFT_THUMB, HOST_PAD_LSTICK}, {XBOX_GAMEPAD_RIGHT_THUMB, HOST_PAD_RSTICK},
    };
    HostPadState st;
    int i;

    xbox_InputOverlaySet(0, pad);
    memset(&st, 0, sizeof st);
    for (i = 0; i < 6; i++)
        if (pad->bAnalogButtons[k_analog[i].analog] >= 128)
            st.buttons |= k_analog[i].host;
    for (i = 0; i < 8; i++)
        if (pad->wButtons & k_bits[i].xbox)
            st.buttons |= k_bits[i].host;
    st.axis[HOST_PAD_AXIS_LT] = (int16_t)(pad->bAnalogButtons[XBOX_BUTTON_LTRIGGER] * 32767 / 255);
    st.axis[HOST_PAD_AXIS_RT] = (int16_t)(pad->bAnalogButtons[XBOX_BUTTON_RTRIGGER] * 32767 / 255);
    st.axis[HOST_PAD_AXIS_LX] = pad->sThumbLX;
    st.axis[HOST_PAD_AXIS_LY] = neg16(pad->sThumbLY);
    st.axis[HOST_PAD_AXIS_RX] = pad->sThumbRX;
    st.axis[HOST_PAD_AXIS_RY] = neg16(pad->sThumbRY);
    host_pad_overlay_set(&st);
}

JNIEXPORT void JNICALL JNI_FN(nativeOverlay)(JNIEnv *env, jclass cls, jint buttons, jint analog_mask)
{
    XBOX_GAMEPAD pad;
    int i;

    (void)env;
    (void)cls;
    memset(&pad, 0, sizeof pad);
    pad.wButtons = (WORD)(buttons & 0xFFFF);
    for (i = 0; i < 8; i++)
        pad.bAnalogButtons[i] = (analog_mask >> i) & 1 ? 255 : 0;
    overlay_both(&pad);
}

static SHORT clamp16(jint v)
{
    return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* The whole overlay pad (XBA-42, the on-screen pad, merged with the companion panel's buttons by OverlayMixer):
 * wButtons, the analog mask for A B X Y Black White (bit i = button i at 255), the triggers 0..255 and the sticks
 * (Y up, -32767..32767). */
JNIEXPORT void JNICALL JNI_FN(nativeOverlayPad)(JNIEnv *env, jclass cls, jint buttons, jint analog_mask, jint lt,
                                                jint rt, jint lx, jint ly, jint rx, jint ry)
{
    XBOX_GAMEPAD pad;
    int i;

    (void)env;
    (void)cls;
    memset(&pad, 0, sizeof pad);
    pad.wButtons = (WORD)(buttons & 0xFFFF);
    for (i = 0; i < 6; i++)
        pad.bAnalogButtons[i] = (analog_mask >> i) & 1 ? 255 : 0;
    pad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (BYTE)(lt < 0 ? 0 : lt > 255 ? 255 : lt);
    pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = (BYTE)(rt < 0 ? 0 : rt > 255 ? 255 : rt);
    pad.sThumbLX = clamp16(lx);
    pad.sThumbLY = clamp16(ly);
    pad.sThumbRX = clamp16(rx);
    pad.sThumbRY = clamp16(ry);
    overlay_both(&pad);
}

/* "Port 1: <name>" lines for the ports that have a host controller now (XBA-123), '\n' separated. */
JNIEXPORT jstring JNICALL JNI_FN(nativePorts)(JNIEnv *env, jclass cls)
{
    char out[512], name[96];
    size_t len = 0;
    DWORD p;

    (void)cls;
    out[0] = 0;
    for (p = 0; p < XBOX_MAX_CONTROLLERS; p++) {
        if (!xbox_InputPortName(p, name, (int)sizeof name))
            continue;
        len += (size_t)snprintf(out + len, sizeof out - len, "%s%lu\t%s", len ? "\n" : "", (unsigned long)p + 1,
                                name);
        if (len >= sizeof out)
            break;
    }
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT void JNICALL JNI_FN(nativeOverlayOff)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    xbox_InputOverlaySet(0, NULL);
    host_pad_overlay_set(NULL);
}

JNIEXPORT jintArray JNICALL JNI_FN(nativeStats)(JNIEnv *env, jclass cls)
{
    uint32_t fps_centi = 0, draws = 0;
    jint v[2];
    jintArray out;

    (void)cls;
    /* A console game presents through host_window (no draw count there); an Xbox game through fb_present_sdl. */
    if (!host_window_stats(&fps_centi, NULL))
        xbox_FramebufferWindowGetStats(&fps_centi, &draws);
    v[0] = (jint)fps_centi;
    v[1] = (jint)draws;
    out = (*env)->NewIntArray(env, 2);
    if (out)
        (*env)->SetIntArrayRegion(env, out, 0, 2, v);
    return out;
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeScreenshot)(JNIEnv *env, jclass cls, jstring path)
{
    const char *p;
    int rc;

    (void)cls;
    if (!path)
        return JNI_FALSE;
    p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p)
        return JNI_FALSE;
    rc = host_window_stats(NULL, NULL) ? host_window_screenshot_bmp(p) : xbox_FramebufferDumpBmp(p);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FN(nativeQuit)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    recomp_quit(0);
}
