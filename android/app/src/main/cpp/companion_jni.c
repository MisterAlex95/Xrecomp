/* The companion panel's JNI (fr.adserver.xrecomp.CompanionNative, docs/android/app-shell.md « Dual screen »):
 * the touch overlay pad (src/input/input_overlay.h), the window's frame stats, a screenshot of the last frame, and
 * quit. Called on the UI thread (the overlay, quit) or a worker thread (the screenshot). */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input_overlay.h"
#include "platform/recomp_quit.h"
#include "video/video_player.h"

#define JNI_FN(name) Java_fr_adserver_xrecomp_CompanionNative_##name

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
    xbox_InputOverlaySet(0, &pad);
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
    xbox_InputOverlaySet(0, &pad);
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
}

JNIEXPORT jintArray JNICALL JNI_FN(nativeStats)(JNIEnv *env, jclass cls)
{
    uint32_t fps_centi = 0, draws = 0;
    jint v[2];
    jintArray out;

    (void)cls;
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
    rc = xbox_FramebufferDumpBmp(p);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FN(nativeQuit)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    recomp_quit(0);
}
