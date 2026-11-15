/* The N64 shader warm-up's JNI (fr.adserver.xrecomp.WarmupNative, ShaderWarmupService): n64_rdp_vk_warmup in the
 * ":warmup" process, which loads libmain.so for it and never runs a game (docs/android/app-shell.md « Shader
 * warm-up »). The environment (RECOMP_VK_DRIVER_*, RECOMP_VK_CACHE_DIR) is the service's, set as the game's would be. */
#include <jni.h>
#include <stdio.h>

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
