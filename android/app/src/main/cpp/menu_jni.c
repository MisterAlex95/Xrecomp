/* The in-game menu's JNI (fr.adserver.xrecomp.MenuNative, docs/android/app-shell.md « In-game menu »): hold the
 * game while the drawer is open (the lifecycle's MENU reason: the flip parks and the audio device pauses, as in the
 * background), the output volume, quit, and a PlayStation game's disc change (XBA-166). Called on the UI thread. */
#include <jni.h>
#include <stdio.h>
#include <unistd.h>

#include "apu_volume.h"
#include "host_audio.h"
#include "ps1_runtime.h"
#include "platform/recomp_lifecycle.h"
#include "platform/recomp_quit.h"

#define JNI_FN(name) Java_fr_adserver_xrecomp_MenuNative_##name

JNIEXPORT void JNICALL JNI_FN(nativeHold)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    if (on)
        recomp_lifecycle_pause_for(RECOMP_HOLD_MENU);
    else
        recomp_lifecycle_resume_for(RECOMP_HOLD_MENU);
}

JNIEXPORT jboolean JNICALL JNI_FN(nativeHeld)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (recomp_lifecycle_reasons() & RECOMP_HOLD_MENU) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FN(nativeSetVolume)(JNIEnv *env, jclass cls, jint percent)
{
    (void)env;
    (void)cls;
    xbox_ApuSetOutputVolume((int)percent);   /* an Xbox game: the APU's output */
    host_audio_set_volume((int)percent);     /* a GBA / PS1 / N64 game: host_audio (XBA-181) */
}

/* Quit from the menu: the guest is let go (a flip parked by the menu must not hold anything), the saves the title
 * wrote are pushed to the disk (sync: they are plain files under <internal files>/save), then recomp_quit(0) as
 * Back twice did (exit hook, « clean exit », log drain, _exit). Does not return. */
JNIEXPORT void JNICALL JNI_FN(nativeQuit)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    printf("xboxrecomp: quit from the in-game menu\n");
    recomp_lifecycle_resume_for(RECOMP_HOLD_ALL);
    sync();
    recomp_quit(0);
}

/* Change disc (XBA-166): the PS1 game being played gets another disc (ps1_runtime_change_disc: the lid opens, a
 * second later the disc is in and the lid closes; the runtime's thread does it between frames). path: a file path or
 * "fd:N" (a document's detached descriptor, left open). False when no PS1 game runs or path is empty. */
JNIEXPORT jboolean JNICALL JNI_FN(nativePs1ChangeDisc)(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    if (!jpath)
        return JNI_FALSE;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path)
        return JNI_FALSE;
    int rc = ps1_runtime_change_disc(path);
    printf("xboxrecomp: change disc to %s from the in-game menu: %s\n", path, rc == 0 ? "queued" : "no PS1 game");
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}
