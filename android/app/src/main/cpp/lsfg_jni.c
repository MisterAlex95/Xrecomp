/* libxba_import.so: the JNI side of "Choose Lossless.dll" (frame generation, docs/technical/framegen.md).
 *
 * The launcher hands the fd of the DLL the player picked (ACTION_OPEN_DOCUMENT, a ParcelFileDescriptor Java keeps
 * open and closes); this extracts the LSFG shaders into the app's private storage (lsfg_cache.h: <baseDir>/<key>),
 * translating the DXBC blobs when the APK was built with the translator (-PxbaLsfg=1 and a DXVK checkout). The DLL
 * itself is never stored; only counts go to the log, never shader contents.
 *
 * Java side: fr.adserver.xrecomp.LsfgNative. Result arrays (long[]):
 *   { status (0 or an LSFG_CACHE_* code), rcdata resources,
 *     dxbc[3.1], dxbc[3.1p], translated[3.1], translated[3.1p], fp16[3.1], fp16[3.1p], fp32[3.1], fp32[3.1p],
 *     needed[3.1], needed[3.1p], ready[3.1], ready[3.1p] }   ready: 1 when every shader of the variant is usable */
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include <android/log.h>

#include "lsfg_cache.h"
#ifdef XBA_LSFG_TRANSLATOR
#include "lsfg_dxbc.h"
#endif

#define TAG "xboxrecomp"
#define NVALS 14

static jlongArray result(JNIEnv *env, int status, const LsfgCacheInfo *info)
{
    jlong v[NVALS] = { status };
    if (info) {
        v[1] = info->rcdata;
        for (int k = 0; k < 2; k++) {
            v[2 + k] = info->dxbc[k];
            v[4 + k] = info->spv[k];
            v[6 + k] = info->fp16[k];
            v[8 + k] = info->fp32[k];
            v[10 + k] = info->needed[k];
            v[12 + k] = lsfg_cache_variant_ready(info, k, NULL, NULL, NULL);
        }
    }
    jlongArray out = (*env)->NewLongArray(env, NVALS);
    if (out)
        (*env)->SetLongArrayRegion(env, out, 0, NVALS, v);
    return out;
}

static void set_string(JNIEnv *env, jobjectArray arr, const char *s)
{
    if (!arr || (*env)->GetArrayLength(env, arr) < 1)
        return;
    jstring j = (*env)->NewStringUTF(env, s);
    (*env)->SetObjectArrayElement(env, arr, 0, j);
    (*env)->DeleteLocalRef(env, j);
}

/* LsfgNative.importDll(int fd, String baseDir, String[] dirOut, String[] errOut) -> long[] (above) */
JNIEXPORT jlongArray JNICALL
Java_fr_adserver_xrecomp_LsfgNative_importDll(JNIEnv *env, jclass cls, jint fd, jstring base_dir,
                                                 jobjectArray dir_out, jobjectArray err_out)
{
    (void)cls;
    if (!base_dir)
        return result(env, LSFG_CACHE_EINVAL, NULL);
    const char *base = (*env)->GetStringUTFChars(env, base_dir, NULL);
    if (!base)
        return result(env, LSFG_CACHE_ENOMEM, NULL);
    char dir[4096] = "", err[512] = "";
    LsfgCacheInfo info;
    memset(&info, 0, sizeof info);
#ifdef XBA_LSFG_TRANSLATOR
    int rc = lsfg_cache_import_fd(fd, base, lsfg_dxbc_translator_name(), lsfg_dxbc_translate, NULL, &info, dir,
                                  sizeof dir, err, sizeof err);
#else
    int rc = lsfg_cache_import_fd(fd, base, NULL, NULL, NULL, &info, dir, sizeof dir, err, sizeof err);
#endif
    (*env)->ReleaseStringUTFChars(env, base_dir, base);
    __android_log_print(rc ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, TAG,
                        "[LSFG] import: %d (%s)%s%s; %u resources; LSFG 3.1 %u/%u DXBC %u translated; "
                        "performance %u/%u DXBC %u translated", rc, lsfg_cache_strerror(rc), err[0] ? ": " : "", err,
                        info.rcdata, info.dxbc[0], info.needed[0], info.spv[0], info.dxbc[1], info.needed[1],
                        info.spv[1]);
    set_string(env, dir_out, dir);
    set_string(env, err_out, err[0] ? err : lsfg_cache_strerror(rc));
    return result(env, rc, info.needed[0] ? &info : NULL);
}

/* LsfgNative.readManifest(String dir) -> long[] (above): the state of an existing cache directory. */
JNIEXPORT jlongArray JNICALL
Java_fr_adserver_xrecomp_LsfgNative_readManifest(JNIEnv *env, jclass cls, jstring dir)
{
    (void)cls;
    if (!dir)
        return result(env, LSFG_CACHE_EINVAL, NULL);
    const char *d = (*env)->GetStringUTFChars(env, dir, NULL);
    if (!d)
        return result(env, LSFG_CACHE_ENOMEM, NULL);
    LsfgCacheInfo info;
    int rc = lsfg_cache_read_manifest(d, &info);
    (*env)->ReleaseStringUTFChars(env, dir, d);
    return result(env, rc, rc == 0 ? &info : NULL);
}

/* LsfgNative.currentDir(String baseDir) -> String or null: <baseDir>/<key> of the last import. */
JNIEXPORT jstring JNICALL
Java_fr_adserver_xrecomp_LsfgNative_currentDir(JNIEnv *env, jclass cls, jstring base_dir)
{
    (void)cls;
    if (!base_dir)
        return NULL;
    const char *base = (*env)->GetStringUTFChars(env, base_dir, NULL);
    if (!base)
        return NULL;
    char dir[4096];
    int rc = lsfg_cache_current(base, dir, sizeof dir);
    (*env)->ReleaseStringUTFChars(env, base_dir, base);
    return rc == 0 ? (*env)->NewStringUTF(env, dir) : NULL;
}

JNIEXPORT jboolean JNICALL
Java_fr_adserver_xrecomp_LsfgNative_hasTranslator(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
#ifdef XBA_LSFG_TRANSLATOR
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jstring JNICALL
Java_fr_adserver_xrecomp_LsfgNative_strerror(JNIEnv *env, jclass cls, jint err)
{
    (void)cls;
    return (*env)->NewStringUTF(env, lsfg_cache_strerror(err));
}
