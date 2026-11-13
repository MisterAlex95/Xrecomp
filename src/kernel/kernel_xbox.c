/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Console keys: zeros, the same on every host, on purpose.
 * On the console the HD key is per machine and the others are Microsoft's;
 * none is in the repo. XAPI keys save signatures with them (XcHMAC), so a
 * key generated per device would make a save copied from the player's Mac to
 * their phone fail its signature; a constant keeps saves portable between
 * hosts. A save from a real console does not verify either way. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[64] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof("\\Device\\CdRom0\\default.xbe") - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* $RECOMP_IMAGE names a secondary image of the disc (XBA-82): the host-side
 * copy follows, as the guest's does (kernel_bridge.c). */
void xbox_image_file_name_init(void)
{
    const char *n = xbox_image_file_name();
    snprintf(g_image_filename, sizeof g_image_filename, "%s", n);
    xbox_XeImageFileName.Length = (USHORT)strlen(g_image_filename);
}

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region and video standard: from the title's certificate (below)
 *   - Video: 60Hz on NTSC and, by default, on PAL (PAL 60; RECOMP_PAL60=0 is
 *     the console's own 50 Hz default), widescreen and 480p
 *     when the player turns them on (RECOMP_WIDESCREEN, RECOMP_480P). A
 *     title that honours widescreen draws anamorphic 16:9 frames; showing
 *     them wide is the presenter's aspect setting (src/video/aspect_policy.h)
 *   - Language: RECOMP_LANGUAGE, else English
 *   - Audio: Stereo, Dolby Digital
 *   - DVD Region: 1 for North America, 2 for Japan and the rest of the world
 *   - Time zone: the host's
 * ============================================================================ */

/* The console's time zone. The EEPROM holds the bias (UTC = local + bias,
 * in minutes), the standard and daylight biases added to it, four-character
 * names, and the dates daylight time starts and ends (month, week of the
 * month, day of the week, hour). XAPI turns the UTC clock into local time
 * with these, so a fixed zone stamps saves and clocks hours off anywhere
 * else: SSX 3 dated its saves two hours early in CEST.
 *
 * Windows has all of it (GetTimeZoneInformation). POSIX hosts give the
 * offset in effect now and the zone names, not the rule: the bias is the
 * current offset with no daylight dates, which is right until the next
 * change of clocks during a run. */
typedef struct { uint8_t month, day, dow, hour; } XboxTzDate;
static struct {
    int32_t bias, std_bias, dlt_bias;
    XboxTzDate std_date, dlt_date;
    char std_name[4], dlt_name[4];
} s_tz;

static void tz_name4(char out[4], const char *src)
{
    int k;
    for (k = 0; k < 4; k++) {
        char c = (src && *src) ? *src++ : 0;
        out[k] = (c >= 0x20 && c < 0x7F) ? c : (c ? '?' : 0);
    }
}

static void tz_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#ifdef _WIN32
    {
        TIME_ZONE_INFORMATION tzi;
        char sn[5], dn[5];
        int k;
        memset(&tzi, 0, sizeof tzi);
        GetTimeZoneInformation(&tzi);
        s_tz.bias = tzi.Bias;
        s_tz.std_bias = tzi.StandardBias;
        s_tz.dlt_bias = tzi.DaylightBias;
        s_tz.std_date.month = (uint8_t)tzi.StandardDate.wMonth;
        s_tz.std_date.day = (uint8_t)tzi.StandardDate.wDay;
        s_tz.std_date.dow = (uint8_t)tzi.StandardDate.wDayOfWeek;
        s_tz.std_date.hour = (uint8_t)tzi.StandardDate.wHour;
        s_tz.dlt_date.month = (uint8_t)tzi.DaylightDate.wMonth;
        s_tz.dlt_date.day = (uint8_t)tzi.DaylightDate.wDay;
        s_tz.dlt_date.dow = (uint8_t)tzi.DaylightDate.wDayOfWeek;
        s_tz.dlt_date.hour = (uint8_t)tzi.DaylightDate.wHour;
        for (k = 0; k < 4; k++) {
            sn[k] = (char)(tzi.StandardName[k] < 128 ? tzi.StandardName[k] : '?');
            dn[k] = (char)(tzi.DaylightName[k] < 128 ? tzi.DaylightName[k] : '?');
        }
        sn[4] = dn[4] = 0;
        tz_name4(s_tz.std_name, sn);
        tz_name4(s_tz.dlt_name, dn);
    }
#else
    {
        time_t now = time(NULL);
        struct tm lt;
        tzset();
        if (localtime_r(&now, &lt))
            s_tz.bias = (int32_t)(-lt.tm_gmtoff / 60);
        tz_name4(s_tz.std_name, tzname[0]);
        tz_name4(s_tz.dlt_name, tzname[1]);
    }
#endif
}

/* The console language (XC_LANGUAGE, and offset 0x30 of the XC_MAX_OS block):
 * RECOMP_LANGUAGE as a code (en, ja, de, fr, es, it, ko, zh, pt) or the Xbox
 * value 1..9; unset or unknown is English (1). Titles pick their text and
 * voice from it among the languages on their disc. Read on every query: XAPI
 * asks once or twice at boot, and a test can change it between queries. */
static ULONG eeprom_language(void)
{
    static const char *const codes[] = { "en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt" };
    const char *e = getenv("RECOMP_LANGUAGE");
    size_t i;
    if (!e || !*e)
        return 1;
    if (e[0] >= '1' && e[0] <= '9' && e[1] == 0)
        return (ULONG)(e[0] - '0');
    for (i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if ((e[0] | 0x20) == codes[i][0] && (e[1] | 0x20) == codes[i][1]
            && (e[2] == 0 || e[2] == '-' || e[2] == '_'))
            return (ULONG)(i + 1);
    {
        static int warned;
        if (!warned) {
            warned = 1;
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
                "RECOMP_LANGUAGE=%s is not a language code (en ja de fr es it ko zh pt) nor 1..9; English", e);
        }
    }
    return 1;
}

/* The console's region (XBA-121). A console plays the discs of its own
 * region: the XBE certificate lists the regions a title allows (+0xA0), and
 * a title may check XGetGameRegion against them or pick its video standard
 * from XGetVideoStandard. So the console here is one the title allows:
 * North America when it does (NTSC 60 Hz, the old fixed answer), then Japan,
 * then the rest of the world (PAL 50 Hz). RECOMP_GAME_REGION=na|jp|eu
 * chooses one (a multi-region disc as a PAL console sees it, or the old
 * behaviour for a PAL disc). Read on every query, like the language. */
static ULONG s_cert_regions;

void xbox_ConsoleSetTitleRegions(ULONG CertGameRegions)
{
    s_cert_regions = CertGameRegions;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "Certificate game regions 0x%08lX: console region %lu, AV region 0x%08lX, video flags 0x%08lX",
        (unsigned long)CertGameRegions, (unsigned long)xbox_ConsoleGameRegion(),
        (unsigned long)xbox_ConsoleAvRegion(), (unsigned long)xbox_ConsoleVideoFlags());
}

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && (!strcmp(e, "1") || !strcmp(e, "on") || !strcmp(e, "true") || !strcmp(e, "yes"));
}

ULONG xbox_ConsoleGameRegion(void)
{
    const char *e = getenv("RECOMP_GAME_REGION");
    if (e && *e) {
        char c = (char)(e[0] | 0x20);
        if (c == 'n' || c == 'u' || !strcmp(e, "1"))
            return XC_GAME_REGION_NA;
        if (c == 'j' || !strcmp(e, "2"))
            return XC_GAME_REGION_JAPAN;
        if (c == 'e' || c == 'p' || c == 'r' || !strcmp(e, "4"))
            return XC_GAME_REGION_RESTOFWORLD;
        {
            static int warned;
            if (!warned) {
                warned = 1;
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
                    "RECOMP_GAME_REGION=%s is not na, jp or eu; the title's region", e);
            }
        }
    }
    if (s_cert_regions & XC_GAME_REGION_NA)
        return XC_GAME_REGION_NA;
    if (s_cert_regions & XC_GAME_REGION_JAPAN)
        return XC_GAME_REGION_JAPAN;
    if (s_cert_regions & XC_GAME_REGION_RESTOFWORLD)
        return XC_GAME_REGION_RESTOFWORLD;
    return XC_GAME_REGION_NA;
}

ULONG xbox_ConsoleAvRegion(void)
{
    switch (xbox_ConsoleGameRegion()) {
    case XC_GAME_REGION_JAPAN:       return XC_AV_REGION_NTSC_J;
    case XC_GAME_REGION_RESTOFWORLD: return XC_AV_REGION_PAL_I;
    default:                         return XC_AV_REGION_NTSC_M;
    }
}

/* XC_VIDEO, the dashboard's video settings. On a PAL console 60Hz is the
 * "PAL 60" choice. A console leaves it off, but here it is on unless
 * RECOMP_PAL60=0: before XBA-121 every title saw an NTSC console, and a PAL
 * title that supports PAL 60 (Burnout 3's PAL release, for one) ran at
 * 60 fps; the default must not take that away. A title without PAL 60
 * support still sets a PAL-I mode and runs at 50 Hz, and the vblank pacing
 * follows the mode it sets. */
static int env_off(const char *name)
{
    const char *e = getenv(name);
    return e && (!strcmp(e, "0") || !strcmp(e, "off") || !strcmp(e, "false") || !strcmp(e, "no"));
}

ULONG xbox_ConsoleVideoFlags(void)
{
    ULONG v = 0;
    if (xbox_ConsoleAvRegion() != XC_AV_REGION_PAL_I || !env_off("RECOMP_PAL60"))
        v |= XC_VIDEO_FLAGS_60Hz;
    if (env_on("RECOMP_WIDESCREEN"))
        v |= XC_VIDEO_FLAGS_WIDESCREEN;
    if (env_on("RECOMP_480P"))
        v |= XC_VIDEO_FLAGS_HDTV_480p;
    return v;
}

static ULONG console_dvd_region(void)
{
    return xbox_ConsoleGameRegion() == XC_GAME_REGION_NA ? 1 : 2;
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;
    tz_init();

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = eeprom_language();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* The flags live in the high half-word: the old low-half-word value
         * read as 0 (#301). */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = xbox_ConsoleVideoFlags();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* Stereo PCM matches the two-channel host output. 1 means mono;
         * advertising AC3 requires an encoded-output path. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0x00000000;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = console_dvd_region();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_FACTORY_GAME_REGION:
    case XC_FACTORY_AV_REGION:
        /* Unanswered before: the default case zeroed them, a console of no
         * region and no video standard. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = ValueIndex == XC_FACTORY_GAME_REGION
                           ? xbox_ConsoleGameRegion() : xbox_ConsoleAvRegion();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_TIMEZONE_BIAS:
    case XC_TZ_STD_BIAS:
    case XC_TZ_DLT_BIAS:
        if (ValueLength >= sizeof(LONG)) {
            *(PLONG)Value = ValueIndex == XC_TIMEZONE_BIAS ? s_tz.bias
                          : ValueIndex == XC_TZ_STD_BIAS ? s_tz.std_bias : s_tz.dlt_bias;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(LONG);
        }
        break;

    case XC_TZ_STD_NAME:
    case XC_TZ_DLT_NAME:
        if (ValueLength >= 4) {
            memcpy(Value, ValueIndex == XC_TZ_STD_NAME ? s_tz.std_name : s_tz.dlt_name, 4);
            if (Type) *Type = 3;                 /* REG_BINARY */
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_TZ_STD_DATE:
    case XC_TZ_DLT_DATE:
        if (ValueLength >= 4) {
            memcpy(Value, ValueIndex == XC_TZ_STD_DATE ? &s_tz.std_date : &s_tz.dlt_date, 4);
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_MAX_OS: {
        /* The whole user-settings block (EEPROM 0x60..), which XAPI reads in
         * one go: checksum, bias, std/dlt names, 8 reserved, std/dlt dates,
         * 8 reserved, std/dlt bias, language, video, audio, parental x3,
         * online x4, misc, DVD region -- the same values as the single
         * queries above. Answering it with zeroes put every title in UTC. */
        uint8_t blk[0x60];
        memset(blk, 0, sizeof blk);
        memcpy(blk + 0x04, &s_tz.bias, 4);
        memcpy(blk + 0x08, s_tz.std_name, 4);
        memcpy(blk + 0x0C, s_tz.dlt_name, 4);
        memcpy(blk + 0x18, &s_tz.std_date, 4);
        memcpy(blk + 0x1C, &s_tz.dlt_date, 4);
        memcpy(blk + 0x28, &s_tz.std_bias, 4);
        memcpy(blk + 0x2C, &s_tz.dlt_bias, 4);
        { ULONG v = eeprom_language(); memcpy(blk + 0x30, &v, 4); }       /* RECOMP_LANGUAGE */
        { ULONG v = xbox_ConsoleVideoFlags(); memcpy(blk + 0x34, &v, 4); }
        /* 0x58 is MiscFlags (0); DvdRegion is the last field, at 0x5C. */
        { ULONG v = console_dvd_region(); memcpy(blk + 0x5C, &v, 4); }
        if (Type) *Type = 3;
        if (ResultLength) *ResultLength = sizeof blk;
        if (ValueLength < sizeof blk)
            return 0xC0000023L;                       /* STATUS_BUFFER_TOO_SMALL */
        memcpy(Value, blk, sizeof blk);
        if (ValueLength > sizeof blk)
            memset((uint8_t *)Value + sizeof blk, 0, ValueLength - sizeof blk);
        break;
    }

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    /* Return link up, 100 Mbps, full duplex */
    return 0x01; /* XNET_ETHERNET_LINK_ACTIVE */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
