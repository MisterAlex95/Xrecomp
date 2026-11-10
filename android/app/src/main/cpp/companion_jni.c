/* The companion panel's JNI (fr.adserver.xboxrecomp.CompanionNative, docs/android/app-shell.md « Dual screen »):
 * the touch overlay pad (src/input/input_overlay.h), the window's frame stats, a screenshot of the last frame, and
 * quit. Called on the UI thread (the overlay, quit) or a worker thread (the screenshot). */
#include <jni.h>
#include <stdint.h>
#include <string.h>

#include "input_overlay.h"
#include "platform/recomp_quit.h"
#include "video/video_player.h"

#define JNI_FN(name) Java_fr_adserver_xboxrecomp_CompanionNative_##name

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
