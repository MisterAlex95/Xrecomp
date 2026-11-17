/* The N64 shader warm-up's JNI (fr.adserver.xrecomp.WarmupNative, ShaderWarmupService): n64_rdp_vk_warmup in the
 * ":warmup" process, which loads libmain.so for it and never runs a game (docs/android/app-shell.md « Shader
 * warm-up »). The environment (RECOMP_VK_DRIVER_*, RECOMP_VK_CACHE_DIR) is the service's, set as the game's would be.
 * Also the GameCube's (XBA-264): gx_warmup_headless, the GX program binaries of a title. */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

#include "gx/gx.h"
#include "n64_rdp.h"
#include "platform/android_log.h"

/* { rc, factor, startup, extra, compiled, cache KB, ms } (n64_rdp.h n64_rdp_warmup_result; rc 0 done, 1 the Vulkan RDP
 * cannot run on this driver, 2 no cache directory or memory, 3 no Vulkan RDP in this build). Blocks: a thread of the
 * service's own. */
JNIEXPORT jintArray JNICALL Java_fr_adserver_xrecomp_WarmupNative_nativeWarmup(JNIEnv *env, jclass cls, jint factor)
{
    (void)cls;
    xbox_android_log_init(); /* stdout / stderr into logcat (tag xboxrecomp), as in the game */
    n64_rdp_warmup_result r;
    const int rc = n64_rdp_vk_warmup((int)factor, &r);
    fflush(stdout);
    const jint v[7] = { rc, r.factor, r.startup, r.extra, r.compiled, (jint)r.cache_kb, (jint)(r.seconds * 1000.0) };
    jintArray out = (*env)->NewIntArray(env, 7);
    if (out)
        (*env)->SetIntArrayRegion(env, out, 0, 7, v);
    return out;
}

/* The GameCube shader warm-up (XBA-264, GcShaderWarmup): the TEV states of the title's shader lists compiled on a
 * headless EGL context of this process and their program binaries written to the cache file the game loads
 * (consoles/gc/runtime/gx/gx.h gx_warmup_headless). { rc, states, already cached, compiled, failed, ms }; rc 0 done,
 * 1 the driver has no program binaries, 2 no GLES 3 context, 3 the cache cannot be written, 4 bad arguments. */
JNIEXPORT jintArray JNICALL Java_fr_adserver_xrecomp_WarmupNative_nativeGcWarmup(JNIEnv *env, jclass cls,
                                                                                   jobjectArray lists, jstring cache)
{
    (void)cls;
    xbox_android_log_init();
    jint v[6] = { 4, 0, 0, 0, 0, 0 };
    const char *cpath = cache ? (*env)->GetStringUTFChars(env, cache, NULL) : NULL;
    jsize n = lists ? (*env)->GetArrayLength(env, lists) : 0;
    const char **paths = n > 0 ? calloc((size_t)n, sizeof *paths) : NULL;
    jstring *js = n > 0 ? calloc((size_t)n, sizeof *js) : NULL;
    if (cpath && (n == 0 || (paths && js))) {
        for (jsize i = 0; i < n; i++) {
            js[i] = (jstring)(*env)->GetObjectArrayElement(env, lists, i);
            paths[i] = js[i] ? (*env)->GetStringUTFChars(env, js[i], NULL) : NULL;
        }
        gx_warmup_result r;
        v[0] = gx_warmup_headless(paths, (size_t)n, cpath, false, &r);
        v[1] = r.states, v[2] = r.cached, v[3] = r.compiled, v[4] = r.failed, v[5] = (jint)(r.seconds * 1000.0);
        for (jsize i = 0; i < n; i++)
            if (js[i]) {
                if (paths[i])
                    (*env)->ReleaseStringUTFChars(env, js[i], paths[i]);
                (*env)->DeleteLocalRef(env, js[i]);
            }
    }
    free(paths);
    free(js);
    if (cpath)
        (*env)->ReleaseStringUTFChars(env, cache, cpath);
    fflush(stdout);
    fflush(stderr);
    jintArray out = (*env)->NewIntArray(env, 6);
    if (out)
        (*env)->SetIntArrayRegion(env, out, 0, 6, v);
    return out;
}
