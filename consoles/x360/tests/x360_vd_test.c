/* x360_vd_test -- the display and audio calls at a title's start, the variable exports (XBA-296). */
#include "x360_hle_util.h"

static uint32_t var_of(unsigned ordinal) { return X->vars[X360_LIB_XBOXKRNL][ordinal]; }

static float f_at(uint32_t a)
{
    uint32_t b = x360_r32(X, a);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void test_vd(void)
{
    uint32_t p = galloc(0x100), q = galloc(8);
    CHECK(K(X360_xboxkrnl_VdGetGraphicsAsicID, 0) == 0x11, "the ASIC id");
    CHECK(K(X360_xboxkrnl_VdQueryVideoCapabilities, 0) == 3, "capabilities");
    K(X360_xboxkrnl_VdQueryRealVideoMode, 1, p);
    CHECK(x360_r32(X, p) == 1280 && x360_r32(X, p + 4) == 720, "the real mode is the mode: %ux%u", x360_r32(X, p),
          x360_r32(X, p + 4));
    x360_w32(X, q, 0x1234);
    CHECK(K(X360_xboxkrnl_VdGetOption, 2, 3, q) == 0 && x360_r32(X, q) == 0, "an option reads as off");
    CHECK(K(X360_xboxkrnl_VdQuerySystemCommandBuffer, 2, p, q) == 0 && x360_r32(X, p) == 0xBEEF0000u, "command buffer");
    CHECK(K(X360_xboxkrnl_VdSetWSSOption, 1, 1) == 0 && K(X360_xboxkrnl_VdTurnDisplayOn, 0) == 0, "accepted");
    CHECK(K(X360_xboxkrnl_VdReadEEDIDBlock, 0) == 0xC00000BBu, "no display to read an EDID from");
}

static void test_audio(void)
{
    uint32_t p = galloc(0x40);
    CHECK(K(X360_xboxkrnl_XAudioIsDuckerEnabled, 0) == 0, "the ducker starts off");
    K(X360_xboxkrnl_XAudioEnableDucker, 1, 1);
    CHECK(K(X360_xboxkrnl_XAudioIsDuckerEnabled, 0) == 1, "enabled");
    cpu()->fpr[1] = 0.5;
    K(X360_xboxkrnl_XAudioSetDuckerLevel, 0);
    CHECK(K(X360_xboxkrnl_XAudioGetDuckerLevel, 1, p) == 0 && f_at(p) == 0.5f, "the level comes back: %f", f_at(p));
    K(X360_xboxkrnl_XAudioSetDuckerHoldTime, 1, 250);
    K(X360_xboxkrnl_XAudioGetDuckerHoldTime, 1, p);
    CHECK(x360_r32(X, p) == 250, "the hold time comes back");
    x360_w32(X, p, 7);
    CHECK(K(X360_xboxkrnl_XAudioGetUnderrunCount, 1, p) == 0 && x360_r32(X, p) == 0, "no underruns");
    X->time_skip += 53333 * 10;
    K(X360_xboxkrnl_XAudioGetRenderDriverTic, 1, p);
    CHECK(x360_r32(X, p) >= 10, "the tic follows virtual time: %u", x360_r32(X, p));
    CHECK(K(X360_xboxkrnl_XAudioSubmitDigitalPacket, 0) == 0xC00000BBu, "digital bypass is refused");
}

static void test_vars(void)
{
    static const unsigned all[] = {
        X360_xboxkrnl_ExMutantObjectType, X360_xboxkrnl_ExTimerObjectType, X360_xboxkrnl_IoCompletionObjectType,
        X360_xboxkrnl_IoDeviceObjectType, X360_xboxkrnl_IoFileObjectType, X360_xboxkrnl_ObDirectoryObjectType,
        X360_xboxkrnl_ObSymbolicLinkObjectType, X360_xboxkrnl_KiBugCheckData, X360_xboxkrnl_Nls844UnicodeCaseTable,
        X360_xboxkrnl_XboxKrnlBaseVersion, X360_xboxkrnl_KeCertMonitorData, X360_xboxkrnl_UsbdBootEnumerationDoneEvent,
        X360_xboxkrnl_StfsDeviceErrorEvent, X360_xboxkrnl_VdGlobalDevice, X360_xboxkrnl_VdGlobalXamDevice,
    };
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++)
        CHECK(var_of(all[k]) != 0, "variable export %zu has storage", k);
    CHECK(x360_r32(X, var_of(X360_xboxkrnl_VdGpuClockInMHz)) == 500, "the GPU clock");
    CHECK(x360_r16(X, var_of(X360_xboxkrnl_XboxKrnlBaseVersion) + 4) == 17559, "the base version's build");
    CHECK(x360_r32(X, var_of(X360_xboxkrnl_UsbdBootEnumerationDoneEvent) + 4) == 1, "USB enumeration is over");
    uint32_t cs = var_of(X360_xboxkrnl_VdHSIOCalibrationLock);
    K(X360_xboxkrnl_RtlEnterCriticalSection, 1, cs);
    CHECK(x360_r32(X, cs + 0x18) == x360_cur(X)->kthread, "the calibration lock is a working critical section");
    K(X360_xboxkrnl_RtlLeaveCriticalSection, 1, cs);
}

int main(void)
{
    if (hle_util_start())
        return 1;
    test_vd();
    test_audio();
    test_vars();
    return hle_util_end("x360_vd");
}
