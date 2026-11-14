/* ps1_jni.c: Ps1Native.java (XBA-159), in libxba_import.so (the launcher's process, no SDL): the title key of a
 * PlayStation disc the Java reader cannot read itself, a .chd (compressed: zlib, LZMA, zstd, FLAC hunks), for the
 * library scan (Console.identify). recomp_pack_rom_identify does the reading, with the PS1 disc reader (ps1_disc.c and
 * the vendored libchdr) as its disc hook (ps1_identify.h), so the key is the one the game process computes. The same
 * reader also serves a .chd's raw sectors to a Prepare (XBA-168: Ps1Native.Disc, read by Ps1Disc.extract, which copies
 * the boot EXE's sectors into the sparse disc.iso tools.prepare reads). */
#include <jni.h>
#include <stdint.h>

#include "ps1_devices.h"
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

/* Ps1Native.discOpenNative(path): opens a disc image (a path or "fd:N", left open) with ps1_disc_open; a handle (0 on
 * failure). discSizeNative: its size in sectors. discReadNative(h, lba, out): the raw 2352-byte sector at absolute
 * LBA lba into out (0, or -1 past the end / a read error). discCloseNative: closes it. */
JNIEXPORT jlong JNICALL
Java_fr_adserver_xrecomp_Ps1Native_discOpenNative(JNIEnv *env, jclass cls, jstring jpath)
{
    (void)cls;
    if (!jpath)
        return 0;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path)
        return 0;
    char err[256];
    ps1_disc *d = ps1_disc_open(path, err, sizeof err);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (jlong)(intptr_t)d;
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_Ps1Native_discSizeNative(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    return h ? (jint)ps1_disc_size((ps1_disc *)(intptr_t)h) : 0;
}

JNIEXPORT jint JNICALL
Java_fr_adserver_xrecomp_Ps1Native_discReadNative(JNIEnv *env, jclass cls, jlong h, jint lba, jbyteArray out)
{
    (void)cls;
    if (!h || !out || lba < 0 || (*env)->GetArrayLength(env, out) < 2352)
        return -1;
    uint8_t sec[2352];
    if (ps1_disc_read_sector((ps1_disc *)(intptr_t)h, (uint32_t)lba, sec) != 0)
        return -1;
    (*env)->SetByteArrayRegion(env, out, 0, 2352, (const jbyte *)sec);
    return 0;
}

JNIEXPORT void JNICALL
Java_fr_adserver_xrecomp_Ps1Native_discCloseNative(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    if (h)
        ps1_disc_close((ps1_disc *)(intptr_t)h);
}
