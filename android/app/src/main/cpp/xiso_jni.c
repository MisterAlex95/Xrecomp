/* libxba_import.so (XBA-40, XBA-54): the JNI side of the game picker's ISO import, around src/xiso.
 *
 * Its own small library, not part of libmain.so: the picker (LauncherActivity) runs in the process ":launcher",
 * before SDL, and libmain.so needs libSDL2.so loaded first and pulls the whole runtime (--whole-archive toolkit, the
 * title). This one depends on libc, liblog and the static xiso target only.
 *
 * Java side: fr.adserver.xboxrecomp.XisoNative. The fd belongs to a ParcelFileDescriptor that Java keeps open (and
 * closes) around each call; nothing here closes it. */
#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <android/log.h>

#include "xiso.h"

#define TAG "xboxrecomp"
#define PROGRESS_MIN_NS 100000000LL   /* at most ~10 calls into Java per second, plus every file start */

/* Paths in an XISO are 8-bit names; NewStringUTF wants modified UTF-8. Anything outside ASCII becomes '?' (the path
 * is only shown in the progress line). */
static jstring ascii_string(JNIEnv *env, const char *s)
{
    char buf[512];
    size_t i = 0;

    for (; s && s[i] && i < sizeof buf - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        buf[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    buf[i] = '\0';
    return (*env)->NewStringUTF(env, buf);
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* fr.adserver.xboxrecomp.XisoNative.probe(int fd, String[] titleOut) -> long[] { status, totalBytes, hasDefaultXbe } */
JNIEXPORT jlongArray JNICALL
Java_fr_adserver_xboxrecomp_XisoNative_probe(JNIEnv *env, jclass cls, jint fd, jobjectArray title_out)
{
    char title[256] = "";
    uint64_t total = 0;
    int has_xbe = 0;
    (void)cls;

    int rc = xiso_probe_fd(fd, title, sizeof title, &total, &has_xbe);
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: xiso probe: %d (%s), title \"%s\", %llu bytes, default.xbe %s",
                        rc, rc == 0 ? "ok" : xiso_strerror(rc), title, (unsigned long long)total,
                        has_xbe ? "found" : "missing");
    if (title_out && (*env)->GetArrayLength(env, title_out) > 0) {
        jstring t = ascii_string(env, title);
        (*env)->SetObjectArrayElement(env, title_out, 0, t);
        (*env)->DeleteLocalRef(env, t);
    }
    jlong vals[3] = { rc, (jlong)total, has_xbe ? 1 : 0 };
    jlongArray out = (*env)->NewLongArray(env, 3);
    if (out)
        (*env)->SetLongArrayRegion(env, out, 0, 3, vals);
    return out;
}

struct progress_ctx {
    JNIEnv *env;
    jobject listener;
    jmethodID on_progress;   /* boolean onProgress(long done, long total, String path); true = cancel */
    int64_t last_ns;
};

static int progress_cb(uint64_t done, uint64_t total, const char *path, void *user)
{
    struct progress_ctx *ctx = user;
    JNIEnv *env = ctx->env;
    int64_t t = now_ns();
    int file_start = path && path[0] && done < total;

    /* Every file start (xiso calls then) and the final call go through; the 4 MB steps in between are thinned. */
    if (!file_start && done < total && t - ctx->last_ns < PROGRESS_MIN_NS)
        return 0;
    ctx->last_ns = t;

    jstring p = ascii_string(env, path ? path : "");
    jboolean cancel = (*env)->CallBooleanMethod(env, ctx->listener, ctx->on_progress, (jlong)done, (jlong)total, p);
    (*env)->DeleteLocalRef(env, p);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return cancel ? 1 : 0;
}

/* fr.adserver.xboxrecomp.XisoNative.extract(int fd, String outDir, Progress listener) -> int (0 or XISO_E*) */
JNIEXPORT jint JNICALL
Java_fr_adserver_xboxrecomp_XisoNative_extract(JNIEnv *env, jclass cls, jint fd, jstring out_dir, jobject listener)
{
    (void)cls;
    if (!out_dir || !listener)
        return XISO_EINVAL;
    jclass lc = (*env)->GetObjectClass(env, listener);
    struct progress_ctx ctx = {
        .env = env,
        .listener = listener,
        .on_progress = (*env)->GetMethodID(env, lc, "onProgress", "(JJLjava/lang/String;)Z"),
        .last_ns = 0,
    };
    (*env)->DeleteLocalRef(env, lc);
    if (!ctx.on_progress)
        return XISO_EINVAL;   /* NoSuchMethodError pending: Java sees it */

    const char *dir = (*env)->GetStringUTFChars(env, out_dir, NULL);
    if (!dir)
        return XISO_ENOMEM;
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: extracting the ISO into %s", dir);
    int rc = xiso_extract_fd(fd, dir, progress_cb, &ctx);
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: xiso extract: %d (%s)", rc,
                        rc == 0 ? "ok" : xiso_strerror(rc));
    (*env)->ReleaseStringUTFChars(env, out_dir, dir);
    return rc;
}

JNIEXPORT jstring JNICALL
Java_fr_adserver_xboxrecomp_XisoNative_strerror(JNIEnv *env, jclass cls, jint err)
{
    (void)cls;
    const char *s = xiso_strerror(err);
    return (*env)->NewStringUTF(env, s ? s : "?");
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xboxrecomp_XisoNative_canceledCode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return XISO_ECANCELED;
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xboxrecomp_XisoNative_noSpaceCode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return XISO_ENOSPC;
}
