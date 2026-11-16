/* xbox_cheats tests (XBA-241): see CMakeLists.txt. */
#include "xbox_cheats.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int main(void)
{
    printf("xbox_cheats: POSIX only, skipped\n");
    return 77;
}
#else

#include <sys/mman.h>
#include <unistd.h>

static int failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        if (!(c)) {                                                                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                                               \
    do {                                                                                                             \
        unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);                               \
        if (a_ != b_) {                                                                                              \
            printf("FAIL %s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, a_, b_);                    \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

/* The lifted title's lookups, which the runtime links against: no lifted code here. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static uint8_t *G(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + va;
}

static uint32_t R32(uint32_t va)
{
    uint32_t v;
    memcpy(&v, G(va), 4);
    return v;
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    unsigned char *b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

#define TEXT_VA 0x00011000u /* test.xbe's .text (mkxbe.py) */

int main(int argc, char **argv)
{
    size_t len = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned char *xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &len);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, len)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }
    const char *dir = argc > 2 ? argv[2] : ".";
    uint32_t h = xbox_HeapAlloc(0x1000, 4096);
    CHECK(h >= 0x00010000u && h + 0x1000 <= xbox_GetMappedSize());
    memset(G(h), 0, 0x1000);
    uint8_t text0[16];
    memcpy(text0, G(TEXT_VA), sizeof text0);

    /* 1. Codes change values in guest memory: each size, a raw line, a conditional. */
    char text[2048];
    snprintf(text, sizeof text,
             "[Lives]\nenabled=1\n0%07X 00000063\n"
             "[Money]\nenabled=1\n2%07X 000F423F\n"
             "[Raw]\nenabled=1\n%08X:BEEF\n"
             "[Off]\nenabled=0\n0%07X 00000001\n"
             "[If]\nenabled=1\n4%07X 00000007\n1%07X 00001234\n",
             h, h + 4, h + 8, h + 12, h + 16, h + 20);
    CHECK_EQ(xbox_CheatsLoadText(text), 5);
    CHECK_EQ(xbox_CheatsApply(), 4);
    CHECK_EQ(G(h)[0], 0x63);
    CHECK_EQ(R32(h + 4), 999999);
    CHECK_EQ(R32(h + 8), 0xBEEF);
    CHECK_EQ(G(h + 12)[0], 0);       /* off */
    CHECK_EQ(R32(h + 20), 0);        /* condition false */
    G(h)[0] = 3;                     /* the game spends a life */
    G(h + 16)[0] = 7;
    xbox_CheatsApply();
    CHECK_EQ(G(h)[0], 0x63);         /* back at the next vblank */
    CHECK_EQ(R32(h + 20), 0x1234);   /* condition true */

    /* 2. Refused without a fault: the XBE's code section, the low page, past the mapping, the top of the space. A
     *    conditional on an unmapped address fails. */
    unsigned refused0 = xbox_CheatsRefused();
    snprintf(text, sizeof text,
             "[Code]\nenabled=1\n2%07X 90909090\n"
             "[Low]\nenabled=1\n00000100 00000001\n"
             "[Past]\nenabled=1\n0FFFFFF0 00000001\n"
             "[Top]\nenabled=1\nFFFFFFF0:01020304\n"
             "[Mirror]\nenabled=1\n%08X:01\n"
             "[If unmapped]\nenabled=1\n4FFFFFF0 00000000\n0%07X 00000001\n"
             "[If code]\nenabled=1\n4%07X %08X\n0%07X 00000001\n",
             TEXT_VA, h + 0x04000000u, h + 32, TEXT_VA, (unsigned)text0[0], h + 33);
    CHECK_EQ(xbox_CheatsLoadText(text), 7);
    xbox_CheatsApply();
    CHECK(memcmp(G(TEXT_VA), text0, sizeof text0) == 0);
    CHECK(xbox_CheatsRefused() >= refused0 + 6);
    CHECK_EQ(G(h + 32)[0], 0);       /* unmapped condition: fails */
    CHECK_EQ(G(h + 33)[0], 1);       /* code can be read: its condition holds */
    CHECK(!xbox_CheatsGuestOk(TEXT_VA, 4, 1));
    CHECK(xbox_CheatsGuestOk(TEXT_VA, 4, 0));
    CHECK(xbox_CheatsGuestOk(h, 4, 1));
    CHECK(!xbox_CheatsGuestOk(0x100, 1, 1));
    CHECK(!xbox_CheatsGuestOk((uint32_t)xbox_GetMappedSize() - 2, 4, 1));
    CHECK(!xbox_CheatsGuestOk(0x80000000u, 4, 0));

    /* 2b. A page the title protected (NtProtectVirtualMemory): read-only refuses the write, no access also fails a
     *     conditional, and neither faults the caller (the vblank's timer thread). */
    long hp = sysconf(_SC_PAGESIZE);
    uint32_t pg = xbox_HeapAlloc((uint32_t)hp, (uint32_t)hp);
    CHECK(pg != 0);
    memset(G(pg), 0x11, 8);
    snprintf(text, sizeof text,
             "[Ro]\nenabled=1\n2%07X 22222222\n"
             "[If ro]\nenabled=1\n4%07X 00000011\n0%07X 00000001\n",
             pg, pg + 4, h + 40);
    CHECK_EQ(xbox_CheatsLoadText(text), 2);
    CHECK(mprotect(G(pg), (size_t)hp, PROT_READ) == 0);
    unsigned before = xbox_CheatsRefused();
    xbox_CheatsApply();
    CHECK_EQ(R32(pg), 0x11111111u);   /* not written */
    CHECK_EQ(G(h + 40)[0], 1);        /* readable: its conditional holds */
    CHECK(xbox_CheatsRefused() > before);
    G(h + 40)[0] = 0;
    CHECK(mprotect(G(pg), (size_t)hp, PROT_NONE) == 0);
    xbox_CheatsApply();
    CHECK_EQ(G(h + 40)[0], 0);        /* unreadable: its conditional fails */
    CHECK(mprotect(G(pg), (size_t)hp, PROT_READ | PROT_WRITE) == 0);
    xbox_CheatsApply();
    CHECK_EQ(R32(pg), 0x22222222u);   /* writable again: written */

    /* 3. The cheat file: RECOMP_CHEATS_FILE at the first vblank, read again when it changes (the app's toggle). */
    char path[1024];
    snprintf(path, sizeof path, "%s/xbox_cheats_test.cheats", dir);
    snprintf(text, sizeof text, "# test\n[A]\nenabled=1\n2%07X 11111111\n[B]\nenabled=0\n2%07X 22222222\n", h + 64,
             h + 68);
    write_file(path, text);
    setenv("RECOMP_CHEATS_FILE", path, 1);
    xbox_CheatsReset();
    xbox_CheatsVblank();
    CHECK_EQ(R32(h + 64), 0x11111111u);
    CHECK_EQ(R32(h + 68), 0);
    snprintf(text, sizeof text, "# test, changed\n[A]\nenabled=0\n2%07X 11111111\n[B]\nenabled=1\n2%07X 22222222\n",
             h + 64, h + 68);
    write_file(path, text);
    for (int i = 0; i < 30; i++)
        xbox_CheatsVblank();
    CHECK_EQ(R32(h + 68), 0x22222222u);
    unlink(path);

    /* 4. No file: nothing happens. */
    unsetenv("RECOMP_CHEATS_FILE");
    xbox_CheatsReset();
    G(h + 64)[0] = 0;
    xbox_CheatsVblank();
    CHECK_EQ(G(h + 64)[0], 0);
    xbox_CheatsReset();

    printf("\nxbox_cheats: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#endif
