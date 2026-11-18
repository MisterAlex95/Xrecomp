/* gc_jni.c: GcNative.java (XBA-264), in libxba_import.so (the launcher's and the Prepare's process, no SDL): what a
 * GameCube Prepare reads from a disc, out of any image format (ISO / GCM, CISO, GCZ, RVZ, WIA) and from a document's
 * descriptor, through the readers the game itself uses (consoles/gc/runtime/gc_disc*.c): gc_extract_sparse writes the
 * sparse GCM tools.prepare reads (header, apploader, FST, main DOL, REL modules; the rest left as holes). */
#include <jni.h>
#include <stdio.h>

#include "gc_disc.h"
#include "gc_extract.h"
#include "jni_text.h"

/* GcNative.extractNative(path, out): path a file path or "fd:N" (left open). "OK <key> <rels> <dol bytes>" when out was
 * written, else "ERR <why>". */
JNIEXPORT jstring JNICALL
Java_fr_adserver_xrecomp_GcNative_extractNative(JNIEnv *env, jclass cls, jstring jpath, jstring jout)
{
    (void)cls;
    if (!jpath || !jout)
        return jni_utf8_string(env, "ERR no path");
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    const char *out = (*env)->GetStringUTFChars(env, jout, NULL);
    char res[400], why[300] = "", key[48] = "?";
    if (!path || !out) {
        snprintf(res, sizeof res, "ERR out of memory");
    } else {
        gc_disc *d = gc_disc_open(path, why, sizeof why);
        gc_extract_info info;
        if (!d)
            snprintf(res, sizeof res, "ERR %s", why[0] ? why : "not a GameCube disc image");
        else if (gc_extract_sparse(d, out, &info, why, sizeof why))
            snprintf(res, sizeof res, "ERR %s", why);
        else {
            gc_disc_title_key(d, key, sizeof key);
            snprintf(res, sizeof res, "OK %s %u %u", key, info.rels, info.dol_size);
        }
        if (d)
            gc_disc_close(d);
    }
    if (path)
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (out)
        (*env)->ReleaseStringUTFChars(env, jout, out);
    return jni_utf8_string(env, res); /* res holds a file name, maybe cut by snprintf */
}
