/* libxba_import.so (XBA-40, XBA-54): the JNI side of the game picker's ISO import, around src/xiso.
 *
 * Its own small library, not part of libmain.so: the picker (LauncherActivity) runs in the process ":launcher",
 * before SDL, and libmain.so needs libSDL2.so loaded first and pulls the whole runtime (--whole-archive toolkit, the
 * title). This one depends on libc, liblog and the static xiso target only.
 *
 * Java side: fr.adserver.xrecomp.XisoNative. The fd belongs to a ParcelFileDescriptor that Java keeps open (and
 * closes) around each call; nothing here closes it. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* fr.adserver.xrecomp.XisoNative.probe(int fd, String[] titleOut) -> long[] { status, totalBytes, hasDefaultXbe } */
JNIEXPORT jlongArray JNICALL
Java_fr_adserver_xrecomp_XisoNative_probe(JNIEnv *env, jclass cls, jint fd, jobjectArray title_out)
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

/* fr.adserver.xrecomp.XisoNative.extract(int fd, String outDir, Progress listener) -> int (0 or XISO_E*) */
JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_extract(JNIEnv *env, jclass cls, jint fd, jstring out_dir, jobject listener)
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

/* fr.adserver.xrecomp.XisoNative.extractFile(int fd, String pathInImage, String outFile, long maxBytes) -> int
 * (0 or XISO_E*): one file of the image (the game library takes default.xbe out of an ISO without extracting it).
 * outFile is created with O_EXCL and removed on failure; larger than maxBytes is XISO_EINVAL. */
JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_extractFile(JNIEnv *env, jclass cls, jint fd, jstring path_in_image,
                                                   jstring out_file, jlong max_bytes)
{
    (void)cls;
    if (!path_in_image || !out_file || max_bytes < 0)
        return XISO_EINVAL;
    const char *path = (*env)->GetStringUTFChars(env, path_in_image, NULL);
    if (!path)
        return XISO_ENOMEM;
    const char *out = (*env)->GetStringUTFChars(env, out_file, NULL);
    if (!out) {
        (*env)->ReleaseStringUTFChars(env, path_in_image, path);
        return XISO_ENOMEM;
    }
    int rc = xiso_extract_file_fd(fd, path, out, (uint64_t)max_bytes);
    if (rc)
        __android_log_print(ANDROID_LOG_INFO, TAG, "library: xiso extract %s: %d (%s)", path, rc, xiso_strerror(rc));
    (*env)->ReleaseStringUTFChars(env, out_file, out);
    (*env)->ReleaseStringUTFChars(env, path_in_image, path);
    return rc;
}

JNIEXPORT jstring JNICALL
Java_fr_adserver_xrecomp_XisoNative_strerror(JNIEnv *env, jclass cls, jint err)
{
    (void)cls;
    const char *s = xiso_strerror(err);
    return (*env)->NewStringUTF(env, s ? s : "?");
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_canceledCode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return XISO_ECANCELED;
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_noSpaceCode(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return XISO_ENOSPC;
}

/* ── Several fds: split parts, compressed images (XBA-126) ──────────────────────────────────────────────────────
 * The fds are the parts of one image in order (one for a plain or compressed file), owned by Java's
 * ParcelFileDescriptors as above: the source does not close them. */

static int open_parts(JNIEnv *env, jintArray fds, xiso_source **src)
{
    jint n = fds ? (*env)->GetArrayLength(env, fds) : 0;
    jint buf[XISO_SOURCE_MAX_PARTS];
    int ifds[XISO_SOURCE_MAX_PARTS];
    if (n < 1 || n > XISO_SOURCE_MAX_PARTS)
        return XISO_EINVAL;
    (*env)->GetIntArrayRegion(env, fds, 0, n, buf);
    for (jint i = 0; i < n; i++)
        ifds[i] = buf[i];
    return xiso_source_open_fds(ifds, n, 0, src);
}

/* XisoNative.probeParts(int[] fds, String[] titleOut) -> long[] { status, totalBytes, hasDefaultXbe, kind, stored } */
JNIEXPORT jlongArray JNICALL
Java_fr_adserver_xrecomp_XisoNative_probeParts(JNIEnv *env, jclass cls, jintArray fds, jobjectArray title_out)
{
    char title[256] = "";
    uint64_t total = 0, stored = 0;
    int has_xbe = 0, kind = 0;
    xiso_source *src = NULL;
    (void)cls;

    int rc = open_parts(env, fds, &src);
    if (!rc) {
        kind = xiso_source_kind(src);
        stored = xiso_source_stored_size(src);
        rc = xiso_probe_source(src, title, sizeof title, &total, &has_xbe);
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: xiso probe (%s): %d (%s), title \"%s\", %llu bytes, "
                        "default.xbe %s", kind ? xiso_source_kind_name(kind) : "?", rc,
                        rc == 0 ? "ok" : xiso_strerror(rc), title, (unsigned long long)total,
                        has_xbe ? "found" : "missing");
    xiso_source_close(src);
    if (title_out && (*env)->GetArrayLength(env, title_out) > 0) {
        jstring t = ascii_string(env, title);
        (*env)->SetObjectArrayElement(env, title_out, 0, t);
        (*env)->DeleteLocalRef(env, t);
    }
    jlong vals[5] = { rc, (jlong)total, has_xbe ? 1 : 0, kind, (jlong)stored };
    jlongArray out = (*env)->NewLongArray(env, 5);
    if (out)
        (*env)->SetLongArrayRegion(env, out, 0, 5, vals);
    return out;
}

/* XisoNative.extractParts(int[] fds, String outDir, Progress listener) -> int: xiso_extract over the parts. */
JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_extractParts(JNIEnv *env, jclass cls, jintArray fds, jstring out_dir,
                                                    jobject listener)
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
        return XISO_EINVAL;
    xiso_source *src = NULL;
    int rc = open_parts(env, fds, &src);
    if (rc)
        return rc;
    const char *dir = (*env)->GetStringUTFChars(env, out_dir, NULL);
    if (!dir) {
        xiso_source_close(src);
        return XISO_ENOMEM;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: extracting the image (%s) into %s",
                        xiso_source_kind_name(xiso_source_kind(src)), dir);
    rc = xiso_extract_source(src, dir, progress_cb, &ctx);
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: xiso extract: %d (%s)", rc, rc == 0 ? "ok" : xiso_strerror(rc));
    (*env)->ReleaseStringUTFChars(env, out_dir, dir);
    xiso_source_close(src);
    return rc;
}

/* XisoNative.extractFileParts(int[] fds, String pathInImage, String outFile, long maxBytes) -> int */
JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_extractFileParts(JNIEnv *env, jclass cls, jintArray fds, jstring path_in_image,
                                                        jstring out_file, jlong max_bytes)
{
    (void)cls;
    if (!path_in_image || !out_file || max_bytes < 0)
        return XISO_EINVAL;
    xiso_source *src = NULL;
    int rc = open_parts(env, fds, &src);
    if (rc)
        return rc;
    const char *path = (*env)->GetStringUTFChars(env, path_in_image, NULL);
    const char *out = path ? (*env)->GetStringUTFChars(env, out_file, NULL) : NULL;
    if (!path || !out) {
        if (path)
            (*env)->ReleaseStringUTFChars(env, path_in_image, path);
        xiso_source_close(src);
        return XISO_ENOMEM;
    }
    rc = xiso_extract_file_source(src, path, out, (uint64_t)max_bytes);
    if (rc)
        __android_log_print(ANDROID_LOG_INFO, TAG, "library: xiso extract %s: %d (%s)", path, rc, xiso_strerror(rc));
    (*env)->ReleaseStringUTFChars(env, out_file, out);
    (*env)->ReleaseStringUTFChars(env, path_in_image, path);
    xiso_source_close(src);
    return rc;
}

/* XisoNative.extractXbes(int[] fds, String outDir, long maxBytes) -> int: every *.xbe of the image (any directory,
 * at most 16, each at most maxBytes) copied into outDir with its directory, nothing else: the companion folder of an
 * image read in place (default.xbe for the art, hash and Prepare, the other images for XBA-82). Returns the count,
 * or a negative XISO_* code. */
static int xbes_walk(xiso_image *img, const xiso_stat *dir, const char *out_dir, const char *rel, int depth,
                     uint64_t max_bytes, xiso_source *src, int *count)
{
    xiso_dirent *d = NULL;
    size_t n = 0;
    int rc = xiso_list(img, dir, &d, &n);
    if (rc)
        return rc;
    for (size_t i = 0; i < n && !rc; i++) {
        char sub[1024];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", d[i].name);
        if (d[i].is_dir) {
            if (depth < 8) {
                xiso_stat st = { d[i].offset, d[i].size, 1 };
                rc = xbes_walk(img, &st, out_dir, sub, depth + 1, max_bytes, src, count);
            }
            continue;
        }
        size_t nl = strlen(d[i].name);
        if (nl < 4 || strcasecmp(d[i].name + nl - 4, ".xbe") != 0 || d[i].size > max_bytes || *count >= 16)
            continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", out_dir, sub);
        for (char *p = path + strlen(out_dir) + 1; *p; p++) {   /* the directories on the way */
            if (*p != '/')
                continue;
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
        unlink(path);
        rc = xiso_extract_file_source(src, sub, path, max_bytes);
        if (!rc)
            (*count)++;
    }
    free(d);
    return rc;
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_XisoNative_extractXbes(JNIEnv *env, jclass cls, jintArray fds, jstring out_dir,
                                                   jlong max_bytes)
{
    (void)cls;
    if (!out_dir || max_bytes < 0)
        return XISO_EINVAL;
    xiso_source *src = NULL;
    xiso_image *img = NULL;
    int rc = open_parts(env, fds, &src), count = 0;
    if (!rc)
        rc = xiso_open_source(src, &img);
    const char *dir = rc ? NULL : (*env)->GetStringUTFChars(env, out_dir, NULL);
    if (!rc && !dir)
        rc = XISO_ENOMEM;
    if (!rc)
        rc = xbes_walk(img, NULL, dir, "", 0, (uint64_t)max_bytes, src, &count);
    __android_log_print(ANDROID_LOG_INFO, TAG, "import: %d XBE(s) copied out of the image: %d (%s)", count, rc,
                        rc == 0 ? "ok" : xiso_strerror(rc));
    if (dir)
        (*env)->ReleaseStringUTFChars(env, out_dir, dir);
    xiso_close(img);
    xiso_source_close(src);
    return rc ? rc : count;
}
