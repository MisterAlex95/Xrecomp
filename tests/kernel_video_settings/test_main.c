/* The console's region and video settings agree everywhere a title reads them
 * (XBA-121): ExQueryNonVolatileSetting (XC_VIDEO, XC_DVD_REGION, the factory
 * game and AV regions, the XC_MAX_OS block), the AV capabilities word of
 * AvSendTVEncoderOption, the SMC's AV pack, and the vblank rate.
 *
 *  - the region follows the XBE certificate's allowed regions (NA first,
 *    then Japan, then rest of world), RECOMP_GAME_REGION overrides it;
 *  - NTSC-M / NTSC-J are 60 Hz; PAL-I offers PAL 60 by default and is 50 Hz
 *    with RECOMP_PAL60=0 (the console's own default);
 *  - RECOMP_WIDESCREEN / RECOMP_480P set their XC_VIDEO bits, and the same
 *    bits appear in the AV capabilities word;
 *  - a PAL-I display mode makes the vblank 50 Hz, a PAL 60 or NTSC mode 60.
 */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int failures;

static void env(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

static ULONG query(ULONG index)
{
    ULONG v = 0xDEADBEEF, type = 0, len = 0;
    if (xbox_ExQueryNonVolatileSetting(index, &type, &v, sizeof v, &len) || len != 4) {
        fprintf(stderr, "FAIL: index 0x%03lX: no 4-byte answer\n", (unsigned long)index);
        failures++;
    }
    return v;
}

static ULONG avcaps(void)
{
    ULONG r = 0xDEADBEEF;
    xbox_AvSendTVEncoderOption(NULL, AV_OPTION_QUERY_AVPACK, 0, &r);
    return r;
}

/* Every place agrees with the expected region, AV region and video flags. */
static void expect(const char *what, ULONG region, ULONG av_region, ULONG flags, ULONG hz)
{
    uint8_t blk[0x60];
    ULONG type, len, blk_video, blk_dvd, caps = avcaps(), smc = 0xFF;
    ULONG dvd = region == XC_GAME_REGION_NA ? 1 : 2;
    xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, blk, sizeof blk, &len);
    memcpy(&blk_video, blk + 0x34, 4);
    memcpy(&blk_dvd, blk + 0x5C, 4);
    xbox_HalReadSMBusValue(SMC_SLAVE_ADDRESS, SMC_CMD_AV_PACK, FALSE, &smc);
    if (query(XC_FACTORY_GAME_REGION) != region || query(XC_FACTORY_AV_REGION) != av_region
        || query(XC_VIDEO) != flags || blk_video != flags || query(XC_DVD_REGION) != dvd
        || blk_dvd != dvd
        || caps != (AV_PACK_HDTV | (av_region & 0x00C0FF00u) | flags)
        || smc != 0x01 || xbox_AvRefreshHz() != hz) {
        fprintf(stderr,
                "FAIL: %s: region %lu AV 0x%08lX video 0x%08lX/0x%08lX dvd %lu/%lu caps 0x%08lX smc %lu hz %lu;"
                " want region %lu AV 0x%08lX video 0x%08lX dvd %lu caps 0x%08lX smc 1 hz %lu\n",
                what, (unsigned long)query(XC_FACTORY_GAME_REGION),
                (unsigned long)query(XC_FACTORY_AV_REGION), (unsigned long)query(XC_VIDEO),
                (unsigned long)blk_video, (unsigned long)query(XC_DVD_REGION), (unsigned long)blk_dvd,
                (unsigned long)caps, (unsigned long)smc, (unsigned long)xbox_AvRefreshHz(),
                (unsigned long)region, (unsigned long)av_region, (unsigned long)flags,
                (unsigned long)dvd, (unsigned long)(AV_PACK_HDTV | (av_region & 0x00C0FF00u) | flags),
                (unsigned long)hz);
        failures++;
    }
}

int main(void)
{
    const ULONG F60 = XC_VIDEO_FLAGS_60Hz;

    /* No certificate yet (and the old fixed answer): North America. */
    expect("no certificate", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M, F60, 60);
    if (avcaps() != 0x00400104u) {
        fprintf(stderr, "FAIL: NA caps 0x%08lX, the old word was 0x00400104\n", (unsigned long)avcaps());
        failures++;
    }

    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_NA | XC_GAME_REGION_RESTOFWORLD);
    expect("NA + rest of world disc", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M, F60, 60);
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_JAPAN);
    expect("Japan disc", XC_GAME_REGION_JAPAN, XC_AV_REGION_NTSC_J, F60, 60);
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_RESTOFWORLD);
    expect("PAL disc (PAL 60 by default)", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I, F60, 60);
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_JAPAN | XC_GAME_REGION_RESTOFWORLD);
    expect("Japan + rest of world disc", XC_GAME_REGION_JAPAN, XC_AV_REGION_NTSC_J, F60, 60);
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_MANUFACTURING);
    expect("manufacturing only", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M, F60, 60);

    /* PAL 60 and the player's flags. */
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_RESTOFWORLD);
    env("RECOMP_PAL60", "1");
    expect("PAL disc, RECOMP_PAL60=1", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I, F60, 60);
    env("RECOMP_PAL60", "0");
    expect("PAL disc, RECOMP_PAL60=0 (50 Hz)", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I, 0, 50);
    env("RECOMP_WIDESCREEN", "1");
    expect("PAL disc, 50 Hz, widescreen", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I,
           XC_VIDEO_FLAGS_WIDESCREEN, 50);
    env("RECOMP_PAL60", NULL);
    expect("PAL disc, widescreen", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I,
           F60 | XC_VIDEO_FLAGS_WIDESCREEN, 60);
    env("RECOMP_480P", "1");
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_NA);
    expect("NA disc, widescreen + 480p", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M,
           F60 | XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV_480p, 60);
    env("RECOMP_WIDESCREEN", "0");
    expect("NA disc, 480p", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M, F60 | XC_VIDEO_FLAGS_HDTV_480p, 60);
    env("RECOMP_480P", NULL);
    env("RECOMP_WIDESCREEN", NULL);

    /* RECOMP_GAME_REGION wins over the certificate. */
    env("RECOMP_GAME_REGION", "eu");
    expect("NA disc, RECOMP_GAME_REGION=eu", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I, F60, 60);
    env("RECOMP_GAME_REGION", "jp");
    expect("NA disc, RECOMP_GAME_REGION=jp", XC_GAME_REGION_JAPAN, XC_AV_REGION_NTSC_J, F60, 60);
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_RESTOFWORLD);
    env("RECOMP_GAME_REGION", "na");
    expect("PAL disc, RECOMP_GAME_REGION=na", XC_GAME_REGION_NA, XC_AV_REGION_NTSC_M, F60, 60);
    env("RECOMP_GAME_REGION", "mars");
    expect("PAL disc, unknown RECOMP_GAME_REGION", XC_GAME_REGION_RESTOFWORLD, XC_AV_REGION_PAL_I, F60, 60);
    env("RECOMP_GAME_REGION", NULL);

    /* The display mode the title sets decides the vblank rate. */
    xbox_ConsoleSetTitleRegions(XC_GAME_REGION_RESTOFWORLD);
    env("RECOMP_PAL60", "1");
    xbox_AvSetDisplayMode(NULL, 0, 0x44030307u /* 640x480 to PAL-I */, 0, 2560, 0);
    if (xbox_AvRefreshHz() != 50) { fprintf(stderr, "FAIL: PAL-I mode is not 50 Hz\n"); failures++; }
    xbox_AvSetDisplayMode(NULL, 0, 0x0401010Bu /* 640x480 to PAL 60 */, 0, 2560, 0);
    if (xbox_AvRefreshHz() != 60) { fprintf(stderr, "FAIL: PAL 60 mode is not 60 Hz\n"); failures++; }
    xbox_AvSetDisplayMode(NULL, 0, 0x88070701u /* 640x480 to 480p */, 0, 2560, 0);
    if (xbox_AvRefreshHz() != 60) { fprintf(stderr, "FAIL: 480p mode is not 60 Hz\n"); failures++; }
    env("RECOMP_PAL60", NULL);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: region from the certificate, video standard, PAL 60, widescreen and 480p agree across "
           "the EEPROM, the AV capabilities, the SMC and the vblank rate.\n");
    return 0;
}
