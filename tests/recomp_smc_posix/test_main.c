/*
 * Code written or run at run time (XBA-118, src/kernel/recomp_smc.h), on a
 * synthetic XBE of our own:
 *
 *   .text   0x11000-0x16000  PRE|X      five pages, write-protected
 *   .data   0x16000-0x18000  W|PRE      half BSS: never protected
 *   .wx     0x18000-0x1A000  W|PRE|X    writable+executable: left writable
 *                                       (RECOMP_SMC_WX=1 protects it)
 *   .text2  0x1A000-0x1B800  PRE|X      one whole page; the half page past
 *                                       it is not protected
 *
 * The first store into a code page goes through the fault handler once,
 * becomes a code_write record and goes on; a second store costs nothing; a
 * read() into a code page is opened first; an indirect call into the heap,
 * the stack or the image's data is an exec_outside_image record with the
 * first bytes at the target, a null or wild pointer is not; RECOMP_SMC=0
 * protects nothing.
 */
#include "xbox_memory_layout.h"
#include "recomp_discovery.h"
#include "recomp_smc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

extern int xbox_PosixInstallMmioFaultHandler(void);

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } \
                      else printf("  ok  %s\n", #c); } while (0)

static void put32(unsigned char *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }

static const struct { const char *name; uint32_t va, vsize, raw, flags; } k_sec[] = {
    {".text",  0x11000, 0x5000, 0x5000, 0x6},
    {".data",  0x16000, 0x2000, 0x1000, 0x3},
    {".wx",    0x18000, 0x2000, 0x2000, 0x7},
    {".text2", 0x1A000, 0x1800, 0x1800, 0x6},
};
#define NSEC (sizeof k_sec / sizeof k_sec[0])

static unsigned char *make_xbe(size_t *n)
{
    size_t raw = 0x1000, size;
    unsigned char *b;
    for (size_t i = 0; i < NSEC; i++)
        raw += k_sec[i].raw;
    size = raw;
    b = calloc(1, size);
    memcpy(b, "XBEH", 4);
    put32(b, 0x104, 0x10000);                   /* base */
    put32(b, 0x108, 0x1000);                    /* headers */
    put32(b, 0x10C, 0x1B800 - 0x10000);         /* image */
    put32(b, 0x110, 0x178);
    put32(b, 0x118, 0x10200);                   /* certificate */
    put32(b, 0x11C, (uint32_t)NSEC);
    put32(b, 0x120, 0x10400);                   /* section headers */
    put32(b, 0x128, 0x11000);                   /* entry */
    put32(b, 0x208, 0x4D530118);                /* title ID */
    raw = 0x1000;
    for (size_t i = 0; i < NSEC; i++) {
        size_t h = 0x400 + i * 56, nm = 0x700 + i * 16;
        put32(b, h + 0x00, k_sec[i].flags);
        put32(b, h + 0x04, k_sec[i].va);
        put32(b, h + 0x08, k_sec[i].vsize);
        put32(b, h + 0x0C, (uint32_t)raw);
        put32(b, h + 0x10, k_sec[i].raw);
        put32(b, h + 0x14, 0x10000 + (uint32_t)nm);
        memcpy(b + nm, k_sec[i].name, strlen(k_sec[i].name));
        memset(b + raw, 0x90 + (int)i, k_sec[i].raw);     /* our own filler bytes */
        raw += k_sec[i].raw;
    }
    *n = size;
    return b;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    static char buf[16384];
    size_t n;
    if (!f)
        return strcpy(buf, "");
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

int main(void)
{
    char disc[256];
    size_t n;
    unsigned char *xbe = make_xbe(&n), *mem;
    long hp = sysconf(_SC_PAGESIZE);
    int pages, fd[2];
    const char *text;

    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(disc, sizeof disc, "/tmp/recomp_smc_test_%d.jsonl", (int)getpid());
    unlink(disc);
    setenv("RECOMP_DISCOVERY", disc, 1);
    recomp_discovery_reset();
    if (xbox_PosixInstallMmioFaultHandler() != 0 || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("no memory layout on this host\n");
        return 77;
    }
    mem = (unsigned char *)xbox_GetMemoryBase();

    /* Before arming: nothing protected, a store is plain. */
    mem[0x11000] = 0xCC;
    CHECK(xbox_SmcProtectedPages() == 0);

    pages = xbox_SmcArm();
    printf("host page %ld: %d page(s) protected\n", hp, pages);
    if (hp == 4096)
        CHECK(pages == 6);                      /* .text x5 + .text2 x1 */
    else
        CHECK(pages >= 0);                      /* 16 KB pages: what lies wholly in code */
    CHECK(xbox_SmcArm() == pages);              /* once */

    /* Data, BSS, W+X and the half page after .text2: no fault, no record. */
    mem[0x16010] = 1; mem[0x17010] = 2; mem[0x18010] = 3; mem[0x1B900] = 4;
    CHECK(xbox_SmcCount() == 0);

    if (hp == 4096) {
        /* The first store into a code page: recorded, page opened, store done. */
        mem[0x12345] = 0xC3;
        CHECK(mem[0x12345] == 0xC3);
        CHECK(xbox_SmcCount() == 1 && xbox_SmcProtectedPages() == 5);
        /* The same page again: nothing more. */
        mem[0x12346] = 0xC3;
        CHECK(xbox_SmcCount() == 1);
        /* A 32-bit store into .text2. */
        *(volatile uint32_t *)(mem + 0x1A100) = 0xDEADBEEF;
        CHECK(*(volatile uint32_t *)(mem + 0x1A100) == 0xDEADBEEF && xbox_SmcCount() == 2);

        /* read() into a code page: opened first, then read. */
        if (pipe(fd) == 0) {
            CHECK(write(fd[1], "patched!", 8) == 8);
            xbox_SmcHostWrite(mem + 0x13FFC, 8);              /* straddles 0x13000 and 0x14000 */
            CHECK(read(fd[0], mem + 0x13FFC, 8) == 8 && !memcmp(mem + 0x13FFC, "patched!", 8));
            CHECK(xbox_SmcCount() == 4 && xbox_SmcProtectedPages() == 2);
            close(fd[0]); close(fd[1]);
        }
    }

    /* Running code that is not in the image. */
    memcpy(mem + 0x00400000, "\x55\x89\xE5\x83\xEC\x10\xC3\x90", 8);    /* our own bytes */
    {
        unsigned before = xbox_SmcCount();
        CHECK(xbox_SmcExecOutside(0x00400000, 0x00011234) == 1);         /* heap */
        CHECK(xbox_SmcExecOutside(0x00400000, 0x00011234) == 1);         /* named once */
        CHECK(xbox_SmcExecOutside(0x00016100, 0x00011234) == 1);         /* the image's data */
        CHECK(xbox_SmcExecOutside(0x00000010, 0x00011234) == 0);         /* null page */
        CHECK(xbox_SmcExecOutside(0x00011010, 0x00011234) == 0);         /* the image's code */
        CHECK(xbox_SmcExecOutside(0xFE000123, 0x00011234) == 0);         /* kernel thunk */
        CHECK(xbox_SmcExecOutside(0x7F000000, 0x00011234) == 0);         /* nothing mapped there */
        CHECK(xbox_SmcCount() == before + 2);
    }

    text = slurp(disc);
    printf("discovery file:\n%s", text);
    CHECK(count_of(text, "\"kind\":\"exec_outside_image\"") == 2);
    CHECK(strstr(text, "{\"t\":\"smc\",\"va\":\"0x00400000\",\"kind\":\"exec_outside_image\",\"ret\":\"0x00011234\","
                       "\"where\":\"heap\",\"bytes\":\"5589E583EC10C390\"}") != NULL);
    CHECK(strstr(text, "\"where\":\"image_data\"") != NULL);
    if (hp == 4096) {
        CHECK(count_of(text, "\"kind\":\"code_write\"") == 4);
        CHECK(strstr(text, "{\"t\":\"smc\",\"va\":\"0x00012000\",\"kind\":\"code_write\",\"at\":\"0x00012345\","
                           "\"section\":\".text\",\"by\":\"a store\"") != NULL);
        CHECK(strstr(text, "\"va\":\"0x0001A000\",\"kind\":\"code_write\",\"at\":\"0x0001A100\",\"section\":\".text2\"")
              != NULL);
        CHECK(strstr(text, "\"at\":\"0x00013FFC\",\"section\":\".text\",\"by\":\"a system call") != NULL);
    }

    /* A second load (the next image) opens every page; RECOMP_SMC=0 protects none. */
    xbox_MemoryLayoutShutdown();
    setenv("RECOMP_SMC", "0", 1);
    CHECK(xbox_MemoryLayoutInit(xbe, n));
    CHECK(xbox_SmcProtectedPages() == 0 && xbox_SmcArm() == 0);
    mem = (unsigned char *)xbox_GetMemoryBase();
    mem[0x12000] = 1;                           /* plain memory again */
    /* And on again, with the W+X section too. */
    xbox_MemoryLayoutShutdown();
    unsetenv("RECOMP_SMC");
    setenv("RECOMP_SMC_WX", "1", 1);
    CHECK(xbox_MemoryLayoutInit(xbe, n));
    pages = xbox_SmcArm();
    if (hp == 4096)
        CHECK(pages == 8);                      /* + .wx x2 */
    xbox_MemoryLayoutShutdown();

    recomp_discovery_reset();
    unlink(disc);
    free(xbe);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("recomp_smc_posix: ok\n");
    return 0;
}
