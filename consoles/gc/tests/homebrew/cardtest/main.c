/* cardtest: the GameCube memory card test (XBA-264). libogc's CARD library on slot A, the system's RTC and SRAM:
 *
 *   1. waits for a card in slot A (CARD_ProbeEx), mounts it; a blank or broken card is formatted (CARD_Format) and
 *      mounted again
 *   2. opens the file "xbacardtest" or creates it (one block); a file found must hold the pattern of the run that
 *      wrote it
 *   3. writes one block: the text "XBACARD1 runs=NNNN prev=ok|none|bad" then a pattern made from the run count, reads
 *      it back and compares
 *   4. reads the RTC (__SYS_GetRTC), the time base libogc set from it at boot, SRAM's language, video and sound modes
 *
 * Everything goes into `report` (magic "XBACARDT", the headless test finds it in memory: gc_cardtest.c) and on the
 * screen. Run it twice on the same card: the second run finds the first's file (runs=0002 prev=ok in the card's
 * data, which run_card.sh looks for in cardA.raw). Only our own code and libogc: nothing of Nintendo's. */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern u32 __SYS_GetRTC(u32 *gctime);

/* What the test reads back. Field order and sizes are part of the test (gc_cardtest.c's struct). */
struct report {
    char magic[8] __attribute__((nonstring));
    uint32_t done;               /* 1 when every step ran */
    int32_t probe, mem_size, sect_size;
    int32_t mount1, format, mount2;
    int32_t open, create, write, read;
    uint32_t prev_runs;          /* the run count in the file found (0: none) */
    uint32_t prev_ok;            /* 1: the file found held its pattern */
    uint32_t runs;               /* the run count written */
    uint32_t verify;             /* 1: what was written read back the same */
    uint32_t rtc;                /* seconds since 2000-01-01 */
    uint32_t tb_secs;            /* the time base in seconds (set from the RTC at boot) */
    uint32_t lang, video, sound;
    uint32_t free_blocks;
};
volatile struct report report __attribute__((aligned(32))) = {.magic = "XBACARDT"};

static u8 workarea[CARD_WORKAREA_SIZE] __attribute__((aligned(32)));
static u8 block[8192] __attribute__((aligned(32)));
static u8 back[8192] __attribute__((aligned(32)));

static void fill(u8 *b, uint32_t runs, const char *prev)
{
    memset(b, 0, 64);
    snprintf((char *)b, 64, "XBACARD1 runs=%04lu prev=%s", (unsigned long)runs, prev);
    for (int i = 64; i < 8192; i++)
        b[i] = (u8)(i * 31 + runs * 7);
}

static int pattern_ok(const u8 *b, uint32_t runs)
{
    for (int i = 64; i < 8192; i++)
        if (b[i] != (u8)(i * 31 + runs * 7))
            return 0;
    return 1;
}

int main(void)
{
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    printf("\x1b[2;0HXRecomp GameCube memory card test (XBA-264)\n\n");

    u32 rtc = 0;
    __SYS_GetRTC(&rtc);
    report.rtc = rtc;
    report.tb_secs = (uint32_t)ticks_to_secs(gettime());
    report.lang = SYS_GetLanguage();
    report.video = SYS_GetVideoMode();
    report.sound = SYS_GetSoundMode();
    printf("RTC %lu s since 2000, language %lu, video %lu, sound %lu\n", (unsigned long)rtc,
           (unsigned long)report.lang, (unsigned long)report.video, (unsigned long)report.sound);

    CARD_Init("XBAT", "XR");
    s32 mem = 0, sect = 0, r = CARD_ERROR_BUSY;
    for (int i = 0; i < 300 && (r = CARD_ProbeEx(0, &mem, &sect)) == CARD_ERROR_BUSY; i++)
        VIDEO_WaitVSync(); /* libogc waits 300 ms after it first sees the card */
    report.probe = r;
    report.mem_size = mem;
    report.sect_size = sect;
    printf("Slot A: probe %ld, %ld Mbit, sectors of %ld bytes\n", (long)r, (long)mem, (long)sect);
    if (r < 0)
        goto out;

    r = CARD_Mount(0, workarea, NULL);
    report.mount1 = r;
    printf("Mount %ld\n", (long)r);
    if (r == CARD_ERROR_BROKEN || r == CARD_ERROR_ENCODING) {
        report.format = CARD_Format(0);
        report.mount2 = r = CARD_Mount(0, workarea, NULL);
        printf("Format %ld, mount %ld\n", (long)report.format, (long)r);
    }
    if (r < 0)
        goto out;

    card_file f;
    const char *prev = "none";
    r = CARD_Open(0, "xbacardtest", &f);
    report.open = r;
    if (r == CARD_ERROR_NOFILE) {
        report.create = r = CARD_Create(0, "xbacardtest", (u32)sect, &f);
        printf("Created the file: %ld\n", (long)r);
    } else if (r == CARD_ERROR_READY) {
        r = CARD_Read(&f, back, sizeof back, 0);
        unsigned long n = 0;
        if (r == CARD_ERROR_READY && sscanf((const char *)back, "XBACARD1 runs=%lu", &n) == 1) {
            report.prev_runs = (uint32_t)n;
            report.prev_ok = pattern_ok(back, (uint32_t)n);
        }
        prev = report.prev_ok ? "ok" : "bad";
        printf("Found the file: run %lu, pattern %s\n", (unsigned long)report.prev_runs, prev);
    }
    if (r < 0)
        goto out;

    report.runs = report.prev_runs + 1;
    fill(block, report.runs, prev);
    report.write = CARD_Write(&f, block, sizeof block, 0);
    memset(back, 0, sizeof back);
    report.read = CARD_Read(&f, back, sizeof back, 0);
    report.verify = report.write == 0 && report.read == 0 && memcmp(block, back, sizeof block) == 0;
    CARD_Close(&f);
    u16 free_blocks = 0;
    CARD_GetFreeBlocks(0, &free_blocks);
    report.free_blocks = free_blocks;
    CARD_Unmount(0);
    printf("Run %lu written: write %ld, read %ld, %s; %u blocks free\n", (unsigned long)report.runs,
           (long)report.write, (long)report.read, report.verify ? "same" : "DIFFERENT", free_blocks);
    report.done = 1;
out:
    printf("\n%s\n", report.done && report.verify ? "PASS" : "FAIL");
    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
