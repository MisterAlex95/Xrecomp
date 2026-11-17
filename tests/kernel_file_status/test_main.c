/*
 * What a failed open returns, on every host.
 *
 * The console, and the Win32 backend through ERROR_PATH_NOT_FOUND, tell a
 * missing file (STATUS_OBJECT_NAME_NOT_FOUND) from a file under a missing
 * directory (STATUS_OBJECT_PATH_NOT_FOUND). The POSIX backend used to answer
 * NAME_NOT_FOUND for both, since ENOENT covers both, and left the Win32 error
 * the bridge prints at 0. A title can branch on the status, so the two hosts
 * must agree. Attribute queries answer NAME_NOT_FOUND for both on both
 * backends today, and stay that way.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* kernel_file.c; the bridge declares it the same way. */
uint32_t xbox_LastFileError(void);

static int failures;

static void check(int ok, const char *what, unsigned got)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok) printf(" -- got 0x%08X", got);
    putchar('\n');
    if (!ok) failures++;
}

static void ansi(XBOX_ANSI_STRING *s, XBOX_OBJECT_ATTRIBUTES *oa, const char *path)
{
    s->Length = (USHORT)strlen(path);
    s->MaximumLength = (USHORT)(s->Length + 1);
    s->Buffer = (PCHAR)path;
    oa->RootDirectory = NULL;
    oa->ObjectName = s;
    oa->Attributes = 0x40;
}

static NTSTATUS open_read_h(const char *path, HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    NTSTATUS st;
    ansi(&name, &oa, path);
    st = xbox_NtCreateFile(&h, 0x80100080u /* GENERIC_READ|SYNC|READ_ATTR */, &oa, &ios,
                           NULL, 0, 1 /* share read */, XBOX_FILE_OPEN, 0x60);
    *out = h;
    return st;
}

static NTSTATUS create_info(const char *path, ULONG disposition, ULONG options, HANDLE *out, ULONG *info)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;
    ansi(&name, &oa, path);
    ios.Information = 0xFF;
    *out = NULL;
    st = xbox_NtCreateFile(out, 0xC0100080u /* GENERIC_READ|WRITE|SYNC|READ_ATTR */, &oa, &ios,
                           NULL, 0, 7, disposition, options | 0x20);
    *info = (ULONG)ios.Information;
    return st;
}

static NTSTATUS open_read(const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE h = NULL;
    ansi(&name, &oa, path);
    return xbox_NtCreateFile(&h, 0x80100080u /* GENERIC_READ|SYNC|READ_ATTR */, &oa, &ios,
                             NULL, 0, 1 /* share read */, XBOX_FILE_OPEN, 0x60);
}

static NTSTATUS query(const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_FILE_NETWORK_OPEN_INFORMATION info;
    ansi(&name, &oa, path);
    return xbox_NtQueryFullAttributesFile(&oa, &info);
}

int main(int argc, char **argv)
{
    char root[512], adx[600];
    NTSTATUS st;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        unsigned n = 0;
        /* Wine reuses process ids from one run to the next, so the id
         * alone can name a directory an earlier run left behind. */
        do snprintf(root, sizeof root, "%sfilestatus-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
        snprintf(adx, sizeof adx, "%s\\adx", root);
        CreateDirectoryA(adx, NULL);
    }
#else
    {
        char tmpl[] = "/tmp/filestatusXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(adx, sizeof adx, "%s/adx", root);
        mkdir(adx, 0755);
    }
#endif
    xbox_path_init(root, root);

    printf("failed open status\n");
    st = open_read("D:\\adx\\music_TITLE.adx");
    check((uint32_t)st == 0xC0000034u, "missing file, directory present: NAME_NOT_FOUND", (unsigned)st);
    check(xbox_LastFileError() == 2u, "... and win32 err 2 (ERROR_FILE_NOT_FOUND)", xbox_LastFileError());
    st = open_read("D:\\media\\event\\CLIP0001.BIN");
    check((uint32_t)st == 0xC000003Au, "missing directory: PATH_NOT_FOUND", (unsigned)st);
    check(xbox_LastFileError() == 3u, "... and win32 err 3 (ERROR_PATH_NOT_FOUND)", xbox_LastFileError());

    printf("attribute query status (unchanged)\n");
    st = query("D:\\adx\\music_TITLE.adx");
    check((uint32_t)st == 0xC0000034u, "missing file: NAME_NOT_FOUND", (unsigned)st);
    st = query("D:\\media\\event\\CLIP0001.BIN");
    check((uint32_t)st == 0xC0000034u, "missing directory: NAME_NOT_FOUND too", (unsigned)st);

    /* What XFONT asks of a bitmap font it just opened: FileFsVolumeInformation,
     * then FileInternalInformation. STATUS_NOT_IMPLEMENTED for either and it
     * never reads the font (SpikeOut: Battle Street drew no text). */
    printf("volume and file queries on an open file\n");
    {
        char host[700];
        FILE *f;
        HANDLE h = NULL;
        XBOX_IO_STATUS_BLOCK ios;
        uint8_t buf[64];
        snprintf(host, sizeof host, "%s/Arial10.bmf", adx);
#if defined(_WIN32)
        snprintf(host, sizeof host, "%s\\Arial10.bmf", adx);
#endif
        f = fopen(host, "wb");
        if (f) { fputs("font", f); fclose(f); }
        st = open_read_h("D:\\adx\\Arial10.bmf", &h);
        check(st == 0, "a file opens", (unsigned)st);
        memset(&ios, 0, sizeof ios);
        st = xbox_NtQueryVolumeInformationFile(h, &ios, buf, sizeof buf, XboxFileFsVolumeInformation);
        check(st == 0 && ios.Information >= 17, "FileFsVolumeInformation answers", (unsigned)st);
        memset(&ios, 0, sizeof ios);
        st = xbox_NtQueryVolumeInformationFile(h, &ios, buf, sizeof buf, XboxFileFsDeviceInformation);
        check(st == 0 && ios.Information == 8, "FileFsDeviceInformation answers", (unsigned)st);
        memset(&ios, 0, sizeof ios);
        st = xbox_NtQueryVolumeInformationFile(h, &ios, buf, sizeof buf, XboxFileFsAttributeInformation);
        check(st == 0 && ios.Information == 16 && !memcmp(buf + 12, "FATX", 4),
              "FileFsAttributeInformation answers FATX", (unsigned)st);
        memset(&ios, 0, sizeof ios);
        st = xbox_NtQueryInformationFile(h, &ios, buf, sizeof buf, XboxFileInternalInformation);
        check(st == 0 && ios.Information == 8, "FileInternalInformation answers", (unsigned)st);
        st = xbox_NtQueryVolumeInformationFile(h, &ios, buf, 8, XboxFileFsVolumeInformation);
        check((uint32_t)st == 0xC0000004u, "a short buffer: INFO_LENGTH_MISMATCH", (unsigned)st);
        if (h)
            xbox_NtClose(h);
    }

    /* IoStatusBlock.Information after a create says what happened (KRN-4):
     * XAPI's CreateFile turns FILE_OPENED under OPEN_ALWAYS and
     * FILE_OVERWRITTEN under CREATE_ALWAYS into ERROR_ALREADY_EXISTS. */
    printf("create information\n");
    {
        static const struct { ULONG disp; const char *name; ULONG missing, existing; } c[] = {
            { XBOX_FILE_SUPERSEDE,    "SUPERSEDE",    2, 0 },
            { XBOX_FILE_OPEN_IF,      "OPEN_IF",      2, 1 },
            { XBOX_FILE_OVERWRITE_IF, "OVERWRITE_IF", 2, 3 },
        };
        unsigned i;
        char what[96], path[64];
        for (i = 0; i < sizeof c / sizeof c[0]; i++) {
            HANDLE h = NULL;
            ULONG info = 0xFF;
            snprintf(path, sizeof path, "D:\\adx\\create%u.bin", i);
            st = create_info(path, c[i].disp, 0, &h, &info);
            snprintf(what, sizeof what, "%s on a missing file: %lu", c[i].name, (unsigned long)c[i].missing);
            check(st == 0 && info == c[i].missing, what, (unsigned)info);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info(path, c[i].disp, 0, &h, &info);
            snprintf(what, sizeof what, "%s on an existing file: %lu", c[i].name, (unsigned long)c[i].existing);
            check(st == 0 && info == c[i].existing, what, (unsigned)info);
            if (h) xbox_NtClose(h);
        }
        {
            HANDLE h = NULL;
            ULONG info = 0xFF;
            st = create_info("D:\\adx\\create0.bin", XBOX_FILE_OPEN, 0, &h, &info);
            check(st == 0 && info == 1, "OPEN: 1", (unsigned)info);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\create0.bin", XBOX_FILE_OVERWRITE, 0, &h, &info);
            check(st == 0 && info == 3, "OVERWRITE: 3", (unsigned)info);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\new.bin", XBOX_FILE_CREATE, 0, &h, &info);
            check(st == 0 && info == 2, "CREATE: 2", (unsigned)info);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\new.bin", XBOX_FILE_CREATE, 0, &h, &info);
            check((uint32_t)st == 0xC0000035u, "CREATE on an existing file: NAME_COLLISION", (unsigned)st);
            if (h) xbox_NtClose(h);
        }
        {
            HANDLE h = NULL;
            ULONG info = 0xFF;
            st = create_info("D:\\adx\\save1", XBOX_FILE_CREATE, XBOX_FILE_DIRECTORY_FILE, &h, &info);
            check(st == 0 && info == 2, "directory CREATE: created (2)", (unsigned)st);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\save1", XBOX_FILE_CREATE, XBOX_FILE_DIRECTORY_FILE, &h, &info);
            check((uint32_t)st == 0xC0000035u, "directory CREATE again: NAME_COLLISION", (unsigned)st);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\save1", XBOX_FILE_OPEN_IF, XBOX_FILE_DIRECTORY_FILE, &h, &info);
            check(st == 0 && info == 1, "directory OPEN_IF on it: opened (1)", (unsigned)st);
            if (h) xbox_NtClose(h);
            h = NULL;
            st = create_info("D:\\adx\\save2", XBOX_FILE_OPEN_IF, XBOX_FILE_DIRECTORY_FILE, &h, &info);
            check(st == 0 && info == 2, "directory OPEN_IF on a new one: created (2)", (unsigned)st);
            if (h) xbox_NtClose(h);
        }
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
