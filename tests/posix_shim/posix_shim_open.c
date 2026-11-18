/* open reads and writes the bytes of the file: ROMs and disc images hold 0x1A and CR LF, which a descriptor in the
 * Microsoft C runtime's text mode takes for the end of the file and for a line end. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

enum { SIZE = 4096 };
static const char PATH[] = "posix_shim_open.bin";
static uint8_t s_data[SIZE], s_back[SIZE];

/* A member called open still works next to the shim's open (recomp_pack.h's disc hook is one). */
struct hook {
    int (*open)(const char *path);
};
static int hook_open(const char *path)
{
    return path == PATH;
}

static size_t read_all(int fd, uint8_t *to, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = (int)read(fd, to + got, (unsigned)(n - got));
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    return got;
}

int main(void)
{
    for (int i = 0; i < SIZE; i++)
        s_data[i] = (uint8_t)(i * 7 + (i >> 8));
    s_data[16] = 0x1A; /* Ctrl+Z, early */
    memcpy(s_data + 100, "\r\n\r\n", 4);
    s_data[2000] = 0x1A;

    FILE *f = fopen(PATH, "wb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fwrite(s_data, 1, SIZE, f) == SIZE);
        fclose(f);
    }

    /* Read whole, as the ROM loaders do. */
    int fd = open(PATH, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(read_all(fd, s_back, SIZE) == SIZE);
        CHECK(!memcmp(s_back, s_data, SIZE));
        /* At an offset past the 0x1A and the CR LFs, as the disc readers do. */
        memset(s_back, 0, 64);
        CHECK(pread(fd, s_back, 64, 3000) == 64);
        CHECK(!memcmp(s_back, s_data + 3000, 64));
        close(fd);
    }

    /* Written with a mode (gc_extract.c): no LF grows a CR. */
    fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, s_data, SIZE) == SIZE);
        close(fd);
    }
    f = fopen(PATH, "rb");
    CHECK(f != NULL);
    if (f) {
        memset(s_back, 0, SIZE);
        CHECK(fread(s_back, 1, SIZE, f) == SIZE && fgetc(f) == EOF);
        CHECK(!memcmp(s_back, s_data, SIZE));
        fclose(f);
    }

    struct hook h = {.open = hook_open};
    CHECK(h.open(PATH) == 1);

    remove(PATH);
    if (!failures)
        puts("posix_shim_open: ok");
    return failures ? 1 : 0;
}
