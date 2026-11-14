/* ps1_jni.c: Ps1Native.java (XBA-159), in libxba_import.so (the launcher's process, no SDL): the title key of a
 * PlayStation disc the Java reader cannot read itself, a .chd (compressed: zlib, LZMA, zstd, FLAC hunks), for the
 * library scan (Console.identify). recomp_pack_rom_identify does the reading, with the PS1 disc reader (ps1_disc.c and
 * the vendored libchdr) as its disc hook (ps1_identify.h), so the key is the one the game process computes. */
#include <jni.h>

#include "ps1_identify.h"
#include "recomp_pack.h"

/* Ps1Native.titleKeyNative(path): path is a file path or "fd:N" (a document's descriptor, left open); the PS1 title key
 * (« SLUS-00594 », « PSX-<crc> »), or null when it is no PlayStation disc. */
JNIEXPORT jstring JNICALL
Java_fr_adserver_xrecomp_Ps1Native_titleKeyNative(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    if (!jpath)
        return NULL;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path)
        return NULL;
    char key[64];
    int console = RECOMP_CONSOLE_UNKNOWN;
    ps1_identify_install();
    int rc = recomp_pack_rom_identify(path, &console, key, sizeof key);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (rc != 0 || console != RECOMP_CONSOLE_PS1)
        return NULL;
    return (*env)->NewStringUTF(env, key);
}
