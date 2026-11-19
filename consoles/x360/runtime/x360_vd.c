/* x360_vd.c -- what the D3D library and the audio driver ask the kernel at start, and the variable exports (XBA-296).
 *
 * Vd: the display is one fixed 1280 x 720 mode (x360_kernel.c's VdQueryVideoMode), so a display option is accepted and
 * has no effect (copy protection flags, closed captions, colour adjustments, power saving). What only real display
 * hardware could answer (EDID, the video encoder's registers, the discovery data), and the queries whose output layout
 * is not publicly documented (the video mode list, the 3D format, the mode override), are refused.
 *
 * Audio: one render driver (x360_more.c). The ducker's settings are kept and answered (section KOBJ of a save state);
 * nothing ducks the mix. There is no driver thread: XAudioGetRenderDriverThread gives no handle. Digital bypass and the
 * echo-cancellation clients are refused.
 *
 * XAudio's ducker signatures are not publicly documented: levels are read as floats in f1, times as integers in r3. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define X_STATUS_NOT_SUPPORTED 0xC00000BBu
#define AUDIO_FRAME_100NS 53333u /* 256 samples at 48 kHz */

static void k_ok(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_SUCCESS);
}

static void k_refused(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_hle_refused(x);
    RET(X_STATUS_NOT_SUPPORTED);
}

/* ---- Vd ---- */

static void k_VdGetGraphicsAsicID(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0x11);
}

static void k_VdQueryVideoCapabilities(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1 | 2); /* widescreen, at least 1024 wide: VdQueryVideoFlags' answer */
}

static void k_VdQueryRealVideoMode(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t, (void)c;
    x360_thunk th = {0, X360_xboxkrnl_VdQueryVideoMode, X360_LIB_XBOXKRNL};
    x360_hle_call(x, &th);
}

static void k_VdGetOption(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(1)) /* (option, value*): every option is off */
        x360_w32(x, A(1), 0);
    RET(X_STATUS_SUCCESS);
}

static void k_VdDisplayFatalError(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_fail(x, "the title reported a fatal display error (VdDisplayFatalError %08X)", A(0));
}

/* ---- audio ---- */

static uint32_t f32_bits(double v)
{
    float f = (float)v;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

void x360_vd_defaults(x360_machine *x)
{
    x360_kobj *k = &x->kobj;
    k->duck_enabled = 0;
    k->duck_level = f32_bits(0.25), k->duck_threshold = f32_bits(0.1);
    k->duck_attack = 10, k->duck_hold = 100, k->duck_release = 100;
}

#define DUCK_GET(name, field)                                                                                          \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        if (A(0))                                                                                                      \
            x360_w32(x, A(0), x->kobj.field);                                                                          \
        RET(X_STATUS_SUCCESS);                                                                                         \
    }
#define DUCK_SET_F(name, field)                                                                                        \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        x->kobj.field = f32_bits(c->fpr[1]);                                                                           \
        RET(X_STATUS_SUCCESS);                                                                                         \
    }
#define DUCK_SET_I(name, field)                                                                                        \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        x->kobj.field = A(0);                                                                                          \
        RET(X_STATUS_SUCCESS);                                                                                         \
    }
DUCK_GET(XAudioGetDuckerLevel, duck_level)
DUCK_GET(XAudioGetDuckerThreshold, duck_threshold)
DUCK_GET(XAudioGetDuckerAttackTime, duck_attack)
DUCK_GET(XAudioGetDuckerHoldTime, duck_hold)
DUCK_GET(XAudioGetDuckerReleaseTime, duck_release)
DUCK_SET_F(XAudioSetDuckerLevel, duck_level)
DUCK_SET_F(XAudioSetDuckerThreshold, duck_threshold)
DUCK_SET_I(XAudioSetDuckerAttackTime, duck_attack)
DUCK_SET_I(XAudioSetDuckerHoldTime, duck_hold)
DUCK_SET_I(XAudioSetDuckerReleaseTime, duck_release)

static void k_XAudioEnableDucker(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x->kobj.duck_enabled = A(0) != 0;
    RET(X_STATUS_SUCCESS);
}

static void k_XAudioIsDuckerEnabled(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x->kobj.duck_enabled);
}

/* an output the driver does not have: zero */
static void k_out_zero(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), 0);
    RET(X_STATUS_SUCCESS);
}

static void k_XAudioGetRenderDriverTic(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), (uint32_t)(x360_now(x) / AUDIO_FRAME_100NS));
    RET(X_STATUS_SUCCESS);
}

static void k_XAudioQueryDriverPerformance(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        memset(x360_p(x, A(0)), 0, 0x20);
    RET(X_STATUS_SUCCESS);
}

/* ---- the variable exports ---- */

void x360_vd_vars(x360_machine *x)
{
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExMutantObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExTimerObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_IoCompletionObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_IoDeviceObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_IoFileObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ObDirectoryObjectType, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ObSymbolicLinkObjectType, 16);
    uint32_t a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_ExConsoleGameRegion, 4);
    x360_w32(x, a, 0xFFFF); /* every region, as XGetGameRegion answers */
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_HalFsbInterruptCount, 4);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_HalFsbResetCount, 4);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_KeCertMonitorData, 4); /* no monitor */
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_KiBugCheckData, 20);
    /* NLS 8:4:4 case table: all zero offsets and deltas, so a title that reads it sees no character change case */
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_Nls844UnicodeCaseTable, 0x1000);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_RgcBindInfo, 0x100);
    /* the events of devices that never report: a notification event, not signalled */
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_StfsDeviceErrorEvent, 16);
    a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_UsbdBootEnumerationDoneEvent, 16);
    x360_w32(x, a + 4, 1); /* the USB enumeration at boot is over */
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_UsbdDriverLoadRequiredEvent, 16);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdGlobalDevice, 4);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdGlobalXamDevice, 4);
    a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdGpuClockInMHz, 4);
    x360_w32(x, a, 500);
    a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdHSIOCalibrationLock, 28); /* a free critical section */
    x360_w8(x, a, X_OBJ_EVENT_SYNC);
    x360_w32(x, a + 8, a + 8);
    x360_w32(x, a + 12, a + 8);
    x360_w32(x, a + 0x10, 0xFFFFFFFFu);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdHSIOTrainCount, 4);
    x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_VdHSIOTrainingStatus, 4);
    a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_XboxKrnlBaseVersion, 8); /* as XboxKrnlVersion: 2.0.17559.0 */
    x360_w16(x, a, 2);
    x360_w16(x, a + 4, 17559);
    a = x360_var(x, X360_LIB_XBOXKRNL, X360_xboxkrnl_XboxKrnlVersion4Digit, 4);
    x360_w32(x, a, 2u << 28 | 17559u << 8);
    x360_vd_defaults(x);
}

const x360_hle_row x360_tab_vd[] = {
    X360_HK(VdInitializeScaler, k_ok), X360_HK(VdRegisterGraphicsNotification, k_ok),
    X360_HK(VdRegisterXamGraphicsNotification, k_ok), X360_HK(VdRegisterHDCPNotification, k_ok),
    X360_HK(VdQuerySystemCommandBuffer, x360_k_VdGetSystemCommandBuffer), X360_HK(VdSetSystemCommandBuffer, k_ok),
    X360_HK(VdGetGraphicsAsicID, k_VdGetGraphicsAsicID), X360_HK(VdQueryVideoCapabilities, k_VdQueryVideoCapabilities),
    X360_HK(VdEnumerateVideoModes, k_refused), X360_HK(VdQueryRealVideoMode, k_VdQueryRealVideoMode),
    X360_HK(VdGetOption, k_VdGetOption), X360_HK(VdSetOption, k_ok), X360_HK(VdSetCGMSOption, k_ok),
    X360_HK(VdSetCGMSState, k_ok), X360_HK(VdSetWSSOption, k_ok), X360_HK(VdSetWSSData, k_ok),
    X360_HK(VdSetWSS2Data, k_ok), X360_HK(VdSetSCMSState, k_ok), X360_HK(VdSetMacrovisionOption, k_ok),
    X360_HK(VdSetHDCPOption, k_ok), X360_HK(VdSetHDCPRevocationList, k_ok), X360_HK(VdEnableHDCP, k_ok),
    X360_HK(VdSetStudioRGBMode, k_ok), X360_HK(VdSetColorProfileAdjustment, k_ok),
    X360_HK(VdSetCscMatricesOverride, k_ok), X360_HK(VdSet3dVideoFormat, k_ok), X360_HK(VdGet3dVideoFormat, k_refused),
    X360_HK(VdTurnDisplayOn, k_ok), X360_HK(VdTurnDisplayOff, k_ok), X360_HK(VdInitializeEDRAM, k_ok),
    X360_HK(VdEnableClosedCaption, k_ok), X360_HK(VdSendClosedCaptionData, k_ok),
    X360_HK(VdGetClosedCaptionReadyStatus, k_ok), X360_HK(VdEnableDisablePowerSavingMode, k_ok),
    X360_HK(VdEnableWMAProOverHDMI, k_ok), X360_HK(VdBlockUntilGUIIdle, k_ok),
    X360_HK(VdSetDisplayModeOverride, k_ok), X360_HK(VdGetDisplayModeOverride, k_refused),
    X360_HK(VdStartDisplayDiscovery, k_ok), X360_HK(VdReadWriteHSIOCalibrationFlag, k_ok),
    X360_HK(VdDisplayFatalError, k_VdDisplayFatalError), X360_HK(VdReadEEDIDBlock, k_refused),
    X360_HK(VdGetDisplayDiscoveryData, k_refused), X360_HK(VdGenerateGPUCSCCoefficients, k_refused),
    X360_HK(VdGetWSS2Data, k_refused), X360_HK(VdReadDVERegisterUlong, k_refused),
    X360_HK(VdWriteDVERegisterUlong, k_refused),

    X360_HK(XAudioRenderDriverInitialize, k_ok), X360_HK(XAudioRenderDriverLock, k_ok),
    X360_HK(XAudioSetVoiceCategoryVolume, k_ok), X360_HK(XAudioGetRenderDriverThread, k_out_zero),
    X360_HK(XAudioSetSpeakerConfig, k_ok), X360_HK(XAudioOverrideSpeakerConfig, k_ok),
    X360_HK(XAudioQueryDriverPerformance, k_XAudioQueryDriverPerformance),
    X360_HK(XAudioSuspendRenderDriverClients, k_ok), X360_HK(XAudioGetUnderrunCount, k_out_zero),
    X360_HK(XAudioGetRenderDriverTic, k_XAudioGetRenderDriverTic), X360_HK(XAudioSetProcessFrameCallback, k_ok),
    X360_HK(XAudioEnableDucker, k_XAudioEnableDucker), X360_HK(XAudioIsDuckerEnabled, k_XAudioIsDuckerEnabled),
    X360_HK(XAudioGetDuckerLevel, k_XAudioGetDuckerLevel), X360_HK(XAudioSetDuckerLevel, k_XAudioSetDuckerLevel),
    X360_HK(XAudioGetDuckerThreshold, k_XAudioGetDuckerThreshold),
    X360_HK(XAudioSetDuckerThreshold, k_XAudioSetDuckerThreshold),
    X360_HK(XAudioGetDuckerAttackTime, k_XAudioGetDuckerAttackTime),
    X360_HK(XAudioSetDuckerAttackTime, k_XAudioSetDuckerAttackTime),
    X360_HK(XAudioGetDuckerHoldTime, k_XAudioGetDuckerHoldTime),
    X360_HK(XAudioSetDuckerHoldTime, k_XAudioSetDuckerHoldTime),
    X360_HK(XAudioGetDuckerReleaseTime, k_XAudioGetDuckerReleaseTime),
    X360_HK(XAudioSetDuckerReleaseTime, k_XAudioSetDuckerReleaseTime),
    X360_HK(XAudioBeginDigitalBypassMode, k_refused), X360_HK(XAudioEndDigitalBypassMode, k_refused),
    X360_HK(XAudioSubmitDigitalPacket, k_refused), X360_HK(XAudioCaptureRenderDriverFrame, k_refused),
    X360_HK(XAudioRegisterRenderDriverMECClient, k_refused), X360_HK(XAudioUnregisterRenderDriverMECClient, k_refused),
};
const unsigned x360_tab_vd_n = sizeof x360_tab_vd / sizeof x360_tab_vd[0];
