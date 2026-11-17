/*
 * Where the path layer puts a title's writes on E: (kernel_path.c).
 *
 * E: is \Device\Harddisk0\Partition1. Most of it maps to the game dir, since
 * a title may read its own assets through it, but the directories a title
 * writes -- UDATA and TDATA (saves) and CACHE (scratch) -- must land under
 * the user-data root. Burnout 3 opens E:\CACHE\LocalCache00.bin and
 * LocalCache08.bin with FILE_OPEN_IF; while CACHE fell through to the
 * Partition1 rule, every run wrote them into the user's dump. CACHE maps to
 * TitleCache, not CACHE, since Z: and Partition3-5 (the cache partitions
 * behind X:, Y: and Z:) use <save>/Cache and the two names are one folder on
 * a case-insensitive host.
 *
 * The utility drive (Z:, \??\Z:, Partition3-5) is per title, at
 * <user root>/Cache/<title id>, and follows RECOMP_SAVE_DIR like the saves:
 * one shared <save>/Cache mixed KOTOR's and Fable's files, and a scripted run
 * with a fresh RECOMP_SAVE_DIR still saw the last run's Z:.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere, so
 * both forms of each rule (sub_win, sub_posix) are checked by the same cases.
 */
#ifndef _WIN32
#define _GNU_SOURCE     /* nftw, pthread_getaffinity_np under -std=c11 */
#endif
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <shellapi.h>
#else
#include <ftw.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

static int failures;
static char g_root[512], g_game[600], g_save[600], g_user[600];

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void set_env(const char *k, const char *v)
{
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static int is_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Remove the temporary root. xbox_path_init sizes five partition images in
 * the save dir (sparse, but ~2.6 GB apparent per run), so leaving it behind
 * fills /tmp or %TEMP% one run at a time. */
#if !defined(_WIN32)
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p);
}
#endif

static void remove_root(void)
{
    if (!g_root[0])
        return;
#if defined(_WIN32)
    {
        char from[sizeof g_root + 2];
        SHFILEOPSTRUCTA op;
        size_t n = strlen(g_root);
        memcpy(from, g_root, n);
        from[n] = from[n + 1] = '\0';      /* the list is double-NUL ended */
        memset(&op, 0, sizeof op);
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        SHFileOperationA(&op);
    }
#else
    nftw(g_root, rm_one, 16, FTW_DEPTH | FTW_PHYS);
#endif
}

/* Translate a guest path and return the host path as narrow text. */
static const char *host(const char *guest)
{
    static char out[MAX_PATH * 2];
    xbox_host_char buf[MAX_PATH];

    out[0] = '\0';
    if (!xbox_translate_path(guest, buf, MAX_PATH))
        return out;
#if defined(_WIN32)
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)sizeof out, NULL, NULL);
#else
    snprintf(out, sizeof out, "%s", buf);
#endif
    /* The rules with a sub-directory join it to a remainder that keeps its
     * leading separator ("UDATA//x"); the host accepts that, so compare the
     * collapsed form. */
    {
        char *r = out, *w = out;
        for (; *r; r++)
            if (!(*r == SEP[0] && w > out && w[-1] == SEP[0]))
                *w++ = *r;
        *w = '\0';
    }
    return out;
}

static int same(const char *a, const char *b)
{
#if defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
#if defined(_WIN32)
    if (_strnicmp(path, dir, n) != 0) return 0;
#else
    if (strncmp(path, dir, n) != 0) return 0;
#endif
    return path[n] == SEP[0];
}

/* The directory part of a host path, folded to lower case: Windows, Proton
 * and macOS treat names that differ only in case as one folder. */
static void folded_dir(const char *path, char *out, size_t n)
{
    char *slash;
    size_t i;

    snprintf(out, n, "%s", path);
    slash = strrchr(out, SEP[0]);
    if (slash) *slash = '\0';
    for (i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
}

static void expect(const char *guest, const char *want, const char *what)
{
    const char *got = host(guest);
    check(same(got, want), what);
    if (!same(got, want))
        printf("    %s\n    got  %s\n    want %s\n", guest, got, want);
}

/* With RECOMP_SAVE_DIR unset the user-data root is the save dir. */
static void test_default_root(void)
{
    char want[1200];

    printf("user-data root = save dir\n");
    set_env("RECOMP_SAVE_DIR", NULL);
    xbox_path_init(g_game, g_save);

    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_save);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin", want,
           "Partition1\\CACHE goes to <save>/TitleCache");
    check(!under(host("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin"), g_game),
          "Partition1\\CACHE is not under the game dir");

    /* E:\CACHE and the utility-drive caches share the save root here, so
     * they must differ by more than case: <save>/CACHE was <save>/Cache on
     * a case-insensitive host, and the title's files mixed with Z:'s. */
    {
        static const char *const others[][2] = {
            { "Z:\\LocalCache08.bin",                               "Z:\\"         },
            { "\\Device\\Harddisk0\\Partition3\\LocalCache08.bin", "Partition3" },
            { "\\Device\\Harddisk0\\Partition5\\LocalCache08.bin", "Partition5" },
        };
        char title[1200], other[1200], what[128];
        size_t i;
        folded_dir(host("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin"),
                   title, sizeof title);
        for (i = 0; i < sizeof others / sizeof others[0]; i++) {
            folded_dir(host(others[i][0]), other, sizeof other);
            snprintf(what, sizeof what, "E:\\CACHE and %s are different folders, ignoring case",
                     others[i][1]);
            check(title[0] && other[0] && strcmp(title, other) != 0, what);
        }
    }
}

/* Saves only in the game dir stay there (a build before the UDATA/TDATA
 * rules put them there); the cache never does, even with a CACHE left in
 * the game dir by such a build. */
static void test_legacy_saves(void)
{
    char want[1200], dir[700];

    printf("saves left in the game dir\n");
    set_env("RECOMP_SAVE_DIR", NULL);
    snprintf(dir, sizeof dir, "%s" SEP "UDATA", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "TDATA", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "CACHE", g_game);
    make_dir(dir);
    xbox_path_init(g_game, g_save);

    snprintf(want, sizeof want, "%s" SEP "UDATA" SEP "4541005b", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\4541005b", want,
           "UDATA only in the game dir stays there");
    snprintf(want, sizeof want, "%s" SEP "TDATA" SEP "4541005b", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\TDATA\\4541005b", want,
           "TDATA only in the game dir stays there");
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_save);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin", want,
           "CACHE still goes to <save>/TitleCache");
}

/* RECOMP_SAVE_DIR moves CACHE with UDATA and TDATA. */
static void test_save_dir_root(void)
{
    char want[1200], dir[700];
    XBOX_ANSI_STRING l, t;
    const char *p;

    printf("user-data root = RECOMP_SAVE_DIR\n");
    set_env("RECOMP_SAVE_DIR", g_user);
    xbox_path_init(g_game, g_save);

    /* Burnout 3's exact spelling: lower-case "partition1". */
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache00.bin", g_user);
    expect("\\Device\\Harddisk0\\partition1\\CACHE\\LocalCache00.bin", want,
           "partition1\\CACHE (B3's case) goes to <user>/TitleCache");
    p = host("\\Device\\Harddisk0\\partition1\\CACHE\\LocalCache08.bin");
    check(!under(p, g_game) && !under(p, g_save), "it is under neither the game dir nor the save dir");

    /* The title opens its file with FILE_OPEN_IF, which needs the parent. */
    snprintf(dir, sizeof dir, "%s" SEP "TitleCache", g_user);
    check(is_dir(dir), "translating it made <user>/TitleCache");

    snprintf(want, sizeof want, "%s" SEP "TitleCache", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE", want, "the bare CACHE directory too");
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\", want, "and with a trailing separator");

    snprintf(want, sizeof want, "%s" SEP "UDATA" SEP "4541005b" SEP "SaveMeta.xbx", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\4541005b\\SaveMeta.xbx", want,
           "UDATA still goes to <user>/UDATA");
    snprintf(want, sizeof want, "%s" SEP "TDATA" SEP "4541005b", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\TDATA\\4541005b", want,
           "TDATA still goes to <user>/TDATA");

    /* Asset reads through E: stay in the game dir (Wreckless loads every
     * asset through z:, which it links to Partition1\). */
    snprintf(want, sizeof want, "%s" SEP "Data" SEP "x.dat", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\Data\\x.dat", want,
           "the rest of Partition1 stays in the game dir");

    l.Buffer = (PCHAR)"\\??\\Q:"; l.Length = 6; l.MaximumLength = 7;
    t.Buffer = (PCHAR)"\\Device\\Harddisk0\\Partition1\\";
    t.Length = (USHORT)strlen(t.Buffer);
    t.MaximumLength = (USHORT)(t.Length + 1);
    xbox_IoCreateSymbolicLink(&l, &t);
    snprintf(want, sizeof want, "%s" SEP "media" SEP "a.ipk", g_game);
    expect("q:\\media\\a.ipk", want, "a title link to Partition1\\ still reads the game dir");
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_user);
    expect("q:\\CACHE\\LocalCache08.bin", want, "and its CACHE goes to <user>/TitleCache");

    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\CACHE\\x") == XBOX_TREE_USER,
          "xbox_path_tree: CACHE is the user tree");
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\x") == XBOX_TREE_GAME,
          "xbox_path_tree: the rest of Partition1 is the game tree");

    /* A rule without a trailing separator needs a boundary after it, or
     * any asset name that starts with CACHE/UDATA/TDATA moved too. */
    {
        static const char *const names[][2] = {
            { "CACHEX\\a",       "CACHEX" SEP "a"      },
            { "cached.dat",      "cached.dat"          },
            { "CACHEDATA\\a.bin", "CACHEDATA" SEP "a.bin" },
            { "UDATAX\\a",       "UDATAX" SEP "a"      },
            { "TDATA2\\a",       "TDATA2" SEP "a"      },
        };
        char guest[256], what[128];
        size_t i;
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            snprintf(guest, sizeof guest, "\\Device\\Harddisk0\\Partition1\\%s", names[i][0]);
            snprintf(want, sizeof want, "%s" SEP "%s", g_game, names[i][1]);
            snprintf(what, sizeof what, "Partition1\\%s stays in the game dir", names[i][0]);
            expect(guest, want, what);
            snprintf(what, sizeof what, "xbox_path_tree: Partition1\\%s is the game tree", names[i][0]);
            check(xbox_path_tree(guest) == XBOX_TREE_GAME, what);
        }
    }
}

static int file_exists(const char *p)
{
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

static void touch(const char *p)
{
    FILE *f = fopen(p, "wb");
    if (f) fclose(f);
}

/* Z: per title, under the user-data root. */
static void test_utility_drive(void)
{
    char want[1200], dir[1200], old[1200];
    static const char *const forms[] = {
        "Z:\\cache\\items.bif",
        "\\??\\Z:\\cache\\items.bif",
        "\\Device\\Harddisk0\\Partition3\\cache\\items.bif",
        "\\Device\\Harddisk0\\Partition4\\cache\\items.bif",
        "\\Device\\Harddisk0\\partition5\\cache\\items.bif",
    };
    char what[128];
    size_t i;

    printf("utility drive Z: per title\n");
    set_env("RECOMP_SAVE_DIR", NULL);
    xbox_path_set_title_id(0);
    xbox_path_init(g_game, g_save);

    /* No XBE mapped yet: the old shared folder. */
    snprintf(want, sizeof want, "%s" SEP "Cache" SEP "a.bin", g_save);
    expect("Z:\\a.bin", want, "no title ID: Z: is <save>/Cache, as before");

    /* A file an earlier build left in the shared folder. */
    snprintf(old, sizeof old, "%s" SEP "Cache" SEP "PageFile.bin", g_save);
    touch(old);

    xbox_path_set_title_id(0x4C410003u);
    snprintf(want, sizeof want, "%s" SEP "Cache" SEP "4c410003" SEP "cache" SEP "items.bif",
             g_save);
    for (i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        snprintf(what, sizeof what, "%s -> <save>/Cache/4c410003", forms[i]);
        expect(forms[i], want, what);
    }
    snprintf(dir, sizeof dir, "%s" SEP "Cache" SEP "4c410003", g_save);
    check(is_dir(dir), "translating it made <save>/Cache/4c410003");
    expect("Z:\\", dir, "the bare Z:\\ is the title's folder");
    check(same(xbox_path_utility_dir_utf8(), dir), "xbox_path_utility_dir_utf8 names it");
    check(xbox_path_tree("Z:\\cache\\items.bif") == XBOX_TREE_HDD,
          "xbox_path_tree: Z: is still the HDD tree");
    check(xbox_path_tree("\\??\\Z:\\x") == XBOX_TREE_HDD, "xbox_path_tree: \\??\\Z: too");
    check(file_exists(old), "the old shared <save>/Cache is left as it was");
    /* A name that only starts like a partition is not one. */
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition30\\x") == XBOX_TREE_UNKNOWN,
          "Partition30 is not Partition3");
    /* The bare devices keep their images. */
    snprintf(want, sizeof want, "%s" SEP "Partition3.img", g_save);
    expect("\\Device\\Harddisk0\\Partition3", want, "the bare Partition3 is still its image");

    /* Another title: another folder. */
    xbox_path_set_title_id(0x4D5300D1u);
    snprintf(want, sizeof want, "%s" SEP "Cache" SEP "4d5300d1" SEP "cache" SEP "items.bif",
             g_save);
    expect("Z:\\cache\\items.bif", want, "another title's Z: is <save>/Cache/4d5300d1");

    /* A title that maps Z: itself to a partition: the link then the rule. */
    {
        XBOX_ANSI_STRING l, t;
        l.Buffer = (PCHAR)"\\??\\Z:"; l.Length = 6; l.MaximumLength = 7;
        t.Buffer = (PCHAR)"\\Device\\Harddisk0\\Partition4";
        t.Length = (USHORT)strlen(t.Buffer);
        t.MaximumLength = (USHORT)(t.Length + 1);
        xbox_IoCreateSymbolicLink(&l, &t);
        expect("z:\\cache\\items.bif", want, "Z: linked to Partition4 lands there too");
        xbox_IoDeleteSymbolicLink(&l);
    }

    /* RECOMP_SAVE_DIR: a fresh root starts with an empty Z:. */
    snprintf(old, sizeof old, "%s" SEP "Cache" SEP "4d5300d1" SEP "cache", g_save);
    make_dir(old);
    snprintf(old, sizeof old, "%s" SEP "Cache" SEP "4d5300d1" SEP "cache" SEP "items.bif", g_save);
    touch(old);
    set_env("RECOMP_SAVE_DIR", g_user);
    xbox_path_init(g_game, g_save);
    snprintf(want, sizeof want, "%s" SEP "Cache" SEP "4d5300d1" SEP "cache" SEP "items.bif",
             g_user);
    expect("Z:\\cache\\items.bif", want, "with RECOMP_SAVE_DIR: <user>/Cache/4d5300d1");
    expect("\\Device\\Harddisk0\\Partition3\\cache\\items.bif", want,
           "and Partition3 with it");
    check(!file_exists(want), "the save dir's Z: file is not there: Z: starts empty");
    snprintf(dir, sizeof dir, "%s" SEP "Cache" SEP "4d5300d1", g_user);
    check(same(xbox_path_utility_dir_utf8(), dir), "xbox_path_utility_dir_utf8 follows it");

    set_env("RECOMP_SAVE_DIR", NULL);
    xbox_path_set_title_id(0);
}

#if !defined(_WIN32)
/* The Xbox's file systems ignore case; titles rely on it (a title asks for
 * Data\movies\INTRO.bik where the disc has Data/Movies/intro.bik). On a POSIX
 * host a component that is not there as spelled takes the spelling of the one
 * directory entry that matches it ignoring case. Components nothing matches
 * stay as asked, so a file about to be created is created where the title said. */
static void test_case_folding(void)
{
    char dir[700], want[1200];
    FILE *f;

    printf("\nfile names ignore case (POSIX hosts)\n");
    set_env("RECOMP_SAVE_DIR", NULL);
    xbox_path_init(g_game, g_save);
    snprintf(dir, sizeof dir, "%s" SEP "Data", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "Data" SEP "Movies", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "Data" SEP "Movies" SEP "intro.bik", g_game);
    if ((f = fopen(dir, "wb")) != NULL) fclose(f);

    snprintf(want, sizeof want, "%s" SEP "Data" SEP "Movies" SEP "intro.bik", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\Data\\Movies\\intro.bik", want,
           "a path that exists as spelled is unchanged");
    expect("\\Device\\Harddisk0\\Partition1\\Data\\movies\\INTRO.bik", want,
           "directory and file take the spelling the host has");
    expect("\\Device\\Harddisk0\\Partition1\\DATA\\MOVIES\\Intro.BIK", want,
           "every component folds");
    snprintf(want, sizeof want, "%s" SEP "Data" SEP "Movies" SEP "New.bin", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\data\\movies\\New.bin", want,
           "existing directories fold, a file about to be created keeps its name");
    snprintf(want, sizeof want, "%s" SEP "Nope" SEP "Sub" SEP "x.bin", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\Nope\\Sub\\x.bin", want,
           "nothing matches: the path stays as asked");
}
#endif

/* Nothing leaves the game and save trees (KRN-7), and links are matched
 * ignoring case and followed a bounded number of times (KRN-14). */
static int translates(const char *guest)
{
    xbox_host_char buf[MAX_PATH];
    return xbox_translate_path(guest, buf, MAX_PATH) ? 1 : 0;
}

static void link_set(const char *name, const char *target, int create)
{
    XBOX_ANSI_STRING l, t;
    l.Buffer = (PCHAR)name; l.Length = (USHORT)strlen(name); l.MaximumLength = (USHORT)(l.Length + 1);
    t.Buffer = (PCHAR)target; t.Length = (USHORT)strlen(target); t.MaximumLength = (USHORT)(t.Length + 1);
    if (create)
        xbox_IoCreateSymbolicLink(&l, &t);
    else
        xbox_IoDeleteSymbolicLink(&l);
}

static void test_refusals_and_links(void)
{
    char want[1200];
    XBOX_ANSI_STRING l, t;

    printf("\npaths that leave the sandbox, and links\n");
    set_env("RECOMP_SAVE_DIR", NULL);
    xbox_path_init(g_game, g_save);

    check(!translates("D:\\..\\x"), "D:\\..\\x is refused");
    check(!translates("T:\\a\\..\\..\\x"), "T:\\a\\..\\..\\x is refused");
    check(!translates("\\Device\\Harddisk0\\Partition1\\..\\..\\x"), "Partition1\\..\\..\\x is refused");
    check(!translates("D:\\a/../../x"), "a '..' after a forward slash is refused");
    check(!translates("\\Nope\\x"), "an unrecognized device path is refused");
    check(!translates("x.dat"), "a bare name is refused");
    snprintf(want, sizeof want, "%s" SEP "a..b" SEP "..x", g_game);
    expect("D:\\a..b\\..x", want, "names that only contain dots still translate");

    /* "\??\z:" and "\??\Z:" are one link. */
    link_set("\\??\\w:", "\\Device\\Harddisk0\\Partition1\\", 1);
    snprintf(want, sizeof want, "%s" SEP "media" SEP "a.ipk", g_game);
    expect("W:\\media\\a.ipk", want, "a link registered in lower case is found");
    l.Buffer = (PCHAR)"\\??\\W:"; l.Length = 6; l.MaximumLength = 7;
    t.Buffer = (PCHAR)"\\Device\\CdRom0\\"; t.Length = (USHORT)strlen(t.Buffer);
    t.MaximumLength = (USHORT)(t.Length + 1);
    check(xbox_IoCreateSymbolicLink(&l, &t) == STATUS_OBJECT_NAME_COLLISION,
          "the upper-case spelling collides with it");
    check(xbox_IoDeleteSymbolicLink(&l) == STATUS_SUCCESS, "and deletes it");

    /* A link to its own letter: bounded, not a host stack overflow. */
    link_set("\\??\\Z:", "Z:\\", 1);
    check(!translates("Z:\\loop.bin"), "a link to itself is refused, not followed for ever");
    link_set("\\??\\Z:", "", 0);
}

int main(int argc, char **argv)
{
    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        /* Wine reuses process ids from one run to the next. */
        do snprintf(g_root, sizeof g_root, "%skpath-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(g_root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/kpathXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(g_root, sizeof g_root, "%s", tmpl);
    }
#endif
    snprintf(g_game, sizeof g_game, "%s" SEP "game", g_root);
    snprintf(g_save, sizeof g_save, "%s" SEP "save", g_root);
    snprintf(g_user, sizeof g_user, "%s" SEP "user", g_root);
    make_dir(g_game);

    test_default_root();
    test_save_dir_root();
    test_legacy_saves();
    test_utility_drive();
    test_refusals_and_links();
#if !defined(_WIN32)
    test_case_folding();
#endif
    remove_root();

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
