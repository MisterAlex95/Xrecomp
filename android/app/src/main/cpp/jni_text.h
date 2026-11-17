/* C text handed to Java through NewStringUTF, which wants modified UTF-8: CheckJNI aborts the process on anything
 * else (a debuggable build), a release build shows garbage. Text from outside (an XISO's 8-bit names, a controller's
 * name cut by snprintf in the middle of a character) goes through one of these first. */
#ifndef XBA_JNI_TEXT_H
#define XBA_JNI_TEXT_H

#include <jni.h>
#include <stddef.h>

/* Copies s into out (size n > 0) keeping the well-formed UTF-8 sequences of one to three bytes (no overlong form, no
 * surrogate); every other byte (a cut or broken sequence, a four-byte character, which modified UTF-8 writes as two
 * surrogates) becomes '?'. With ascii_only, every byte outside printable ASCII becomes '?'. out always ends in NUL,
 * and a character never straddles the end of out. */
static inline void jni_text_clean(const char *s, char *out, size_t n, int ascii_only)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    while (*p && o + 1 < n) {
        unsigned c = p[0];
        size_t len = 0;
        if (ascii_only) {
            out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            p++;
            continue;
        }
        if (c < 0x80)
            len = 1;
        else if (c >= 0xc2 && c <= 0xdf && (p[1] & 0xc0) == 0x80)
            len = 2;
        else if ((c & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            unsigned cp = (c & 0x0fu) << 12 | (p[1] & 0x3fu) << 6 | (p[2] & 0x3fu);
            if (cp >= 0x800 && (cp < 0xd800 || cp > 0xdfff))
                len = 3;
        }
        if (!len) {
            out[o++] = '?';
            p++;
            continue;
        }
        if (o + len >= n)
            break;
        for (size_t i = 0; i < len; i++)
            out[o++] = (char)p[i];
        p += len;
    }
    out[o] = '\0';
}

/* s (may be NULL: "") as a Java string, at most 511 bytes of it. */
static inline jstring jni_utf8_string(JNIEnv *env, const char *s)
{
    char buf[512];
    jni_text_clean(s, buf, sizeof buf, 0);
    return (*env)->NewStringUTF(env, buf);
}

/* s as a Java string of printable ASCII only (anything else '?'), at most 511 bytes. */
static inline jstring jni_ascii_string(JNIEnv *env, const char *s)
{
    char buf[512];
    jni_text_clean(s, buf, sizeof buf, 1);
    return (*env)->NewStringUTF(env, buf);
}

#endif
