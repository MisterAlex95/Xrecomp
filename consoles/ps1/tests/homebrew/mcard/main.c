/* PS1 mcard (XBA-167): a save file on memory card 1 through the BIOS memory card functions, the way games save.
 *   InitCARD(1), StartCARD(), _bu_init(); then open("bu00:XBAMCARD", FREAD):
 *   - missing: the card is formatted first when it is not one (format("bu00:"), B(41h)), then the file is created
 *     (open FCREAT, one 8 KB block), written with a known pattern (open FWRITE, write 8192), read back, compared;
 *   - there (a card an earlier run saved): read and compared, nothing written.
 *   firstfile("bu00:*") must list it with its size. Result protocol: README.md. Our own code. */
#include <stdint.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>

#include "../common/xba_result.h"
#include "../common/screen.h"

#define FILE_NAME "bu00:XBAMCARD"
#define SIZE 8192
#define F_READ 0x0001
#define F_WRITE 0x0002
#define F_CREAT 0x0200
#define FORMAT_FAILED 0xEEEEEEEEu

static uint8_t buf[SIZE];

static uint8_t pattern(uint32_t i) { return (uint8_t)((i * 7u + 0x5Au) ^ (i >> 8)); }

static void fill(void)
{
    for (uint32_t i = 0; i < SIZE; i++)
        buf[i] = pattern(i);
}

static uint32_t compare(void)
{
    for (uint32_t i = 0; i < SIZE; i++)
        if (buf[i] != pattern(i))
            return 0;
    return 1;
}

/* B(41h) format(device): PSn00bSDK has no wrapper for it. Returns 1 on success. */
static int bios_format(const char *dev)
{
    register int r __asm__("v0");
    register const char *a0 __asm__("a0") = dev;
    __asm__ volatile(".set push\n\t.set noreorder\n\t"
                     "li $t2, 0xB0\n\t"
                     "jalr $t2\n\t"
                     "li $t1, 0x41\n\t"
                     ".set pop\n"
                     : "=r"(r), "+r"(a0)
                     :
                     : "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "v1", "ra",
                       "memory");
    return r;
}

int main(void)
{
    xba_begin(XBA_ID_MCARD);
    screen_init(0, 0, 64);

    EnterCriticalSection();
    InitCARD(1);
    StartCARD();
    _bu_init();
    ExitCriticalSection();

    uint32_t mode, formatted = 0, opened = 0, wrote = 0, got = 0;
    int fd = open(FILE_NAME, F_READ);
    if (fd >= 0) {
        mode = 2; /* saved by an earlier run: read only */
        opened = 1;
        wrote = SIZE; /* nothing to write */
        memset(buf, 0, SIZE);
        got = (uint32_t)read(fd, buf, SIZE);
        close(fd);
    } else {
        mode = 1;
        fd = open(FILE_NAME, F_CREAT | (1 << 16));
        if (fd < 0) {
            /* not a formatted card (OpenBIOS has no _get_errno, B(54h), to tell): format it, then create */
            formatted = bios_format("bu00:") == 1 ? 1 : FORMAT_FAILED;
            fd = open(FILE_NAME, F_CREAT | (1 << 16));
        }
        if (fd >= 0) {
            close(fd);
            fd = open(FILE_NAME, F_WRITE);
        }
        if (fd >= 0) {
            opened = 1;
            fill();
            wrote = (uint32_t)write(fd, buf, SIZE);
            close(fd);
        }
        memset(buf, 0, SIZE);
        fd = open(FILE_NAME, F_READ);
        if (fd >= 0) {
            got = (uint32_t)read(fd, buf, SIZE);
            close(fd);
        }
    }
    uint32_t same = compare();

    struct DIRENTRY e;
    uint32_t listed = 0, files = 0;
    for (struct DIRENTRY *d = firstfile("bu00:*", &e); d && files < 16; d = nextfile(&e)) {
        files++;
        if (!strcmp(e.name, "XBAMCARD") && e.size == SIZE)
            listed = 1;
    }

    uint32_t i = 0;
    xba_check(i++, mode, mode);                                            /* 1 created, 2 found */
    xba_check(i++, formatted, formatted == FORMAT_FAILED ? 1 : formatted); /* 1: the card had to be formatted */
    xba_check(i++, opened, 1);
    xba_check(i++, wrote, SIZE);
    xba_check(i++, got, SIZE);
    xba_check(i++, same, 1);
    xba_check(i++, listed, 1);
    xba_check(i++, files, files);
    XBA_RESULT->checksum = xba_fnv1a((const volatile uint32_t *)buf, SIZE / 4);
    XBA_RESULT->stage = mode;

    screen_verdict(XBA_RESULT->detail == 0);
    xba_finish();
}
