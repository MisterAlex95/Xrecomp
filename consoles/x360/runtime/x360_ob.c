/* x360_ob.c -- the object manager's calls, and every export a title cannot usefully have here (XBA-296).
 *
 * An import without an HLE answers 0, which most callers read as success. The kernel exports below get an answer that
 * says what is true on this machine instead:
 *   k_accept         nothing to do, and success is the truth: a registration for an event that never comes, a TLB or
 *                    cache maintenance call, a query whose answer is 0
 *   k_no_device      STATUS_DEVICE_NOT_CONNECTED: USB, network adapters, Wi-Fi, cameras, Kinect, memory units
 *   k_not_connected  ERROR_DEVICE_NOT_CONNECTED: the controller driver's own calls (XamInput* is the title's way in)
 *   k_refused        STATUS_NOT_SUPPORTED: console keys and fuses (XeKeys*: no key material exists here), IPTV, debug
 *                    and trace facilities, IRP-level I/O, code loading and patching, power management, firmware
 *   k_refused_zero   the same refusal for an export that returns a BOOL, a pointer or a value: FALSE / NULL / 0, since
 *                    an error status would read as TRUE or as a pointer
 * A refusal is logged once and counted like a missing import, and strict imports stop on it (x360_hle_refused): the title
 * asked for something it will not get. An export whose return type is not known has no row here: it stays missing. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define X_STATUS_OBJECT_NAME_NOT_FOUND_ 0xC0000034u
#define X_STATUS_NOT_SUPPORTED 0xC00000BBu
#define X_STATUS_DEVICE_NOT_CONNECTED 0xC000009Du
#define X_STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define X_STATUS_PRIVILEGE_NOT_HELD 0xC0000061u
#define X_STATUS_NO_MORE_ENTRIES 0x8000001Au
#define X_ERROR_DEVICE_NOT_CONNECTED 0x48Fu

static void k_accept(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_SUCCESS);
}

static void k_no_device(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_hle_refused(x);
    RET(X_STATUS_DEVICE_NOT_CONNECTED);
}

static void k_not_connected(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_hle_refused(x);
    RET(X_ERROR_DEVICE_NOT_CONNECTED);
}

static void k_refused(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_hle_refused(x);
    RET(X_STATUS_NOT_SUPPORTED);
}

static void k_refused_zero(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_hle_refused(x);
    RET(0);
}

static void k_name_not_found(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_OBJECT_NAME_NOT_FOUND_);
}

/* ---- objects ---- */

static void k_ObCreateObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (type, attributes, body size, object*): a zeroed body */
    uint32_t o = x360_kalloc(x, A(2) ? A(2) : 16);
    if (!o) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    x360_w32(x, A(3), o);
    RET(X_STATUS_SUCCESS);
}

static void k_ObInsertObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (object, access state, additional references, handle*) */
    uint32_t h = x360_handle_new(x, A(0));
    if (!h) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    if (A(3))
        x360_w32(x, A(3), h);
    RET(X_STATUS_SUCCESS);
}

static void k_ObOpenObjectByPointer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (object, type, handle*) */
    uint32_t h = A(0) ? x360_handle_new(x, A(0)) : 0;
    if (!h) {
        RET(A(0) ? X_STATUS_NO_MEMORY : X_STATUS_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, A(2), h);
    RET(X_STATUS_SUCCESS);
}

static void k_ObLookupAnyThreadByThreadId(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    for (int k = 0; k < x->nthreads; k++)
        if (x->threads[k].state != TH_FREE && x->threads[k].id == A(0)) {
            if (A(1))
                x360_w32(x, A(1), x->threads[k].kthread);
            RET(X_STATUS_SUCCESS);
            return;
        }
    RET(X_STATUS_INVALID_PARAMETER);
}

static void k_ObGetWaitableObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(A(0)); /* every object a title holds is its own dispatcher object */
}

static void k_NtQueryDirectoryObject(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t, (void)c;
    RET(X_STATUS_NO_MORE_ENTRIES); /* the object namespace is not browsable */
}

static void k_NtDeviceIoControlFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_INVALID_DEVICE_REQUEST); /* files on the disc and the data directory have no device controls */
}

static void k_NtSetSystemTime(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_PRIVILEGE_NOT_HELD); /* the clock is virtual: a title does not move it */
}

/* NtWriteFile from a list of 4 KB pages (FILE_SEGMENT_ELEMENT: 64 bits each), through a bounce buffer */
static void k_NtWriteFileGather(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (handle, event, apc, apc context, iosb, segments, length, offset*) */
    uint32_t segs = A(5), len = A(6);
    uint32_t tmp = x360_heap_alloc(x, &x->heap_v4k, len ? len : 1, 0);
    if (!tmp) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    for (uint32_t o = 0; o < len; o += 0x1000) {
        uint32_t page = x360_r32(x, segs + 8 * (o / 0x1000));
        if (!page)
            page = x360_r32(x, segs + 8 * (o / 0x1000) + 4);
        memmove(x->base + tmp + o, x->base + page, len - o < 0x1000 ? len - o : 0x1000);
    }
    c->gpr[8] = tmp;
    x360_thunk th = {0, X360_xboxkrnl_NtWriteFile, X360_LIB_XBOXKRNL};
    x360_hle_call(x, &th);
    x360_heap_free(x, &x->heap_v4k, tmp);
}

static void k_ExTerminateTitleProcess(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x->status = X360_EXITED;
    x->exit_code = A(0);
}

static void k_ExQueryPoolBlockSize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x360_heap_size_of(&x->heap_v4k, A(0)));
}

static void k_ExFreeDebugPool(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_heap_free(x, &x->heap_v4k, A(0));
    RET(0);
}

static void k_KeSetEventBoostPriority(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_w32(x, A(0) + 4, 1);
    RET(0);
}

static void k_HalGetCurrentAVPack(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_x_XGetAVPack(x, t, c);
}

const x360_hle_row x360_tab_ob[] = {
    X360_HK(ObCreateObject, k_ObCreateObject), X360_HK(ObInsertObject, k_ObInsertObject),
    X360_HK(ObOpenObjectByPointer, k_ObOpenObjectByPointer), X360_HK(ObOpenObjectByName, k_name_not_found),
    X360_HK(ObReferenceObjectByName, k_name_not_found),
    X360_HK(ObLookupAnyThreadByThreadId, k_ObLookupAnyThreadByThreadId),
    X360_HK(ObGetWaitableObject, k_ObGetWaitableObject), X360_HK(NtCreateDirectoryObject, k_refused),
    X360_HK(NtOpenDirectoryObject, k_name_not_found), X360_HK(NtQueryDirectoryObject, k_NtQueryDirectoryObject),
    X360_HK(NtDeviceIoControlFile, k_NtDeviceIoControlFile), X360_HK(NtWriteFileGather, k_NtWriteFileGather),
    X360_HK(NtSetSystemTime, k_NtSetSystemTime), X360_HK(ExTerminateTitleProcess, k_ExTerminateTitleProcess),
    X360_HK(ExQueryPoolBlockSize, k_ExQueryPoolBlockSize), X360_HK(ExFreeDebugPool, k_ExFreeDebugPool),
    X360_HK(KeSetEventBoostPriority, k_KeSetEventBoostPriority), X360_HK(HalGetCurrentAVPack, k_HalGetCurrentAVPack),
    /* k_accept */
    X360_HK(DbgLoadImageSymbols, k_accept), X360_HK(DbgUnLoadImageSymbols, k_accept),
    X360_HK(DevAuthShouldAlwaysEnforce, k_accept), X360_HK(DrvSetAudioLatencyCallback, k_accept),
    X360_HK(DrvSetAutobind, k_accept), X360_HK(DrvSetContentStorageCallback, k_accept),
    X360_HK(DrvSetDeviceConfigChangeCallback, k_accept), X360_HK(DrvSetMicArrayStartCallback, k_accept),
    X360_HK(DrvSetSysReqCallback, k_accept), X360_HK(DrvSetUserBindingCallback, k_accept),
    X360_HK(ExIsBetaFeatureEnabled, k_accept), X360_HK(ExRegisterXConfigNotification, k_accept),
    X360_HK(FscGetCacheElementCount, k_accept), X360_HK(HalClampUnclampOutputDACs, k_accept),
    X360_HK(HalGetNotedArgonErrors, k_accept), X360_HK(HalGetPowerUpCause, k_accept),
    X360_HK(HalIsExecutingPowerDownDpc, k_accept), X360_HK(HalNotifyAddRemoveBackgroundTask, k_accept),
    X360_HK(HalNotifyBackgroundModeTransitionComplete, k_accept),
    X360_HK(HalRegisterBackgroundModeTransitionCallback, k_accept),
    X360_HK(HalRegisterHdDvdRomNotification, k_accept), X360_HK(HalRegisterPowerDownCallback, k_accept),
    X360_HK(HalRegisterPowerDownNotification, k_accept), X360_HK(HalRegisterSMCNotification, k_accept),
    X360_HK(HalRegisterXamPowerDownCallback, k_accept), X360_HK(HalSetAudioEnable, k_accept),
    X360_HK(KeFlushCurrentEntireTb, k_accept), X360_HK(KeFlushEntireTb, k_accept),
    X360_HK(KeFlushMultipleTb, k_accept), X360_HK(KeFlushUserModeCurrentTb, k_accept),
    X360_HK(KeFlushUserModeTb, k_accept), X360_HK(KeQueryBackgroundProcessors, k_accept),
    X360_HK(KeRegisterDriverNotification, k_accept), X360_HK(KeRegisterSwapNotification, k_accept),
    X360_HK(KeRetireDpcList, k_accept), X360_HK(KeSetBackgroundProcessors, k_accept),
    X360_HK(KeSetCurrentProcessType, k_accept), X360_HK(KeSetPageRelocationCallback, k_accept),
    X360_HK(KeSetPowerMode, k_accept), X360_HK(KiApcNormalRoutineNop_, k_accept),
    X360_HK(MmGetPoolPagesType, k_accept), X360_HK(MmLockUnlockBufferPages, k_accept),
    X360_HK(MmPersistPhysicalMemoryAllocation, k_accept), X360_HK(NtCancelIoFile, k_accept),
    X360_HK(NtCancelIoFileEx, k_accept), X360_HK(NtMakeTemporaryObject, k_accept),
    X360_HK(ObMakeTemporaryObject, k_accept), X360_HK(VeSetHandlers, k_accept),
    X360_HK(VgcHandler_SetHandlers, k_accept), X360_HK(XInputdSetFailedConnectionOrBindCallback, k_accept),
    X360_HK(XexDisableVerboseDbgPrint, k_accept), X360_HK(XexImportTraceEnable, k_accept),
    X360_HK(XexSendDeferredNotifications, k_accept), X360_HK(XexSetExecutablePrivilege, k_accept),
    /* k_no_device */
    X360_HK(DetroitDeviceRequest, k_no_device), X360_HK(HidGetCapabilities, k_no_device),
    X360_HK(HidReadKeys, k_no_device), X360_HK(HidReadMouseChanges, k_no_device),
    X360_HK(McaDeviceRequest, k_no_device), X360_HK(MicDeviceRequest, k_no_device),
    X360_HK(MtpdBeginTransaction, k_no_device), X360_HK(MtpdCancelTransaction, k_no_device),
    X360_HK(MtpdEndTransaction, k_no_device), X360_HK(MtpdGetCurrentDevices, k_no_device),
    X360_HK(MtpdReadData, k_no_device), X360_HK(MtpdReadEvent, k_no_device), X360_HK(MtpdResetDevice, k_no_device),
    X360_HK(MtpdSendData, k_no_device), X360_HK(MtpdVerifyProximity, k_no_device), X360_HK(NicAttach, k_no_device),
    X360_HK(NicDetach, k_no_device), X360_HK(NicFlushXmitQueue, k_no_device), X360_HK(NicGetOpt, k_no_device),
    X360_HK(NicGetStats, k_no_device), X360_HK(NicRegisterDevice, k_no_device), X360_HK(NicSetOpt, k_no_device),
    X360_HK(NicSetUnicastAddress, k_no_device), X360_HK(NicShutdown, k_no_device),
    X360_HK(NicUnregisterDevice, k_no_device), X360_HK(NicUpdateMcastMembership, k_no_device),
    X360_HK(NicXmit, k_no_device), X360_HK(NomnilGetExtension, k_no_device), X360_HK(NomnilSetLed, k_no_device),
    X360_HK(NomnilStartCloseDevice, k_no_device), X360_HK(NullCableRequest, k_no_device),
    X360_HK(PsCamDeviceRequest, k_no_device), X360_HK(RmcDeviceRequest, k_no_device),
    X360_HK(SataCdRomRecordReset, k_no_device), X360_HK(SvodCreateDevice, k_no_device),
    X360_HK(TidDeviceRequest, k_no_device), X360_HK(TitleDeviceAuthRequest, k_no_device),
    X360_HK(UsbdAddDeviceComplete, k_no_device), X360_HK(UsbdCallAndBlockOnDpcRoutine, k_no_device),
    X360_HK(UsbdCancelAsyncTransfer, k_no_device), X360_HK(UsbdCancelTimer, k_no_device),
    X360_HK(UsbdEnableDisableRootHubPort, k_no_device),
    X360_HK(UsbdNatalHubRegisterNotificationCallback, k_no_device), X360_HK(UsbdOpenDefaultEndpoint, k_no_device),
    X360_HK(UsbdOpenEndpoint, k_no_device), X360_HK(UsbdQueueAsyncTransfer, k_no_device),
    X360_HK(UsbdQueueCloseDefaultEndpoint, k_no_device), X360_HK(UsbdQueueCloseEndpoint, k_no_device),
    X360_HK(UsbdQueueIsochTransfer, k_no_device), X360_HK(UsbdRegisterDriverObject, k_no_device),
    X360_HK(UsbdRemoveDeviceComplete, k_no_device), X360_HK(UsbdResetDevice, k_no_device),
    X360_HK(UsbdResetEndpoint, k_no_device), X360_HK(UsbdSetTimer, k_no_device),
    X360_HK(UsbdTitleDriverResetAllUnrecognizedPorts, k_no_device),
    X360_HK(UsbdTitleDriverSetUnrecognizedPort, k_no_device), X360_HK(UsbdUnregisterDriverObject, k_no_device),
    X360_HK(WifiBeginAuthentication, k_no_device), X360_HK(WifiCheckCounterMeasures, k_no_device),
    X360_HK(WifiChooseAuthenCipherSetFromBSSID, k_no_device), X360_HK(WifiCompleteAuthentication, k_no_device),
    X360_HK(WifiOnMICError, k_no_device), X360_HK(WifiPrepareAuthenticationContext, k_no_device),
    X360_HK(WifiRecvEAPOLPacket, k_no_device), X360_HK(XUsbcamCreate, k_no_device),
    X360_HK(XUsbcamDestroy, k_no_device), X360_HK(XUsbcamGetConfig, k_no_device),
    X360_HK(XUsbcamGetState, k_no_device), X360_HK(XUsbcamGetView, k_no_device),
    X360_HK(XUsbcamReadFrame, k_no_device), X360_HK(XUsbcamReset, k_no_device),
    X360_HK(XUsbcamSetCaptureMode, k_no_device), X360_HK(XUsbcamSetConfig, k_no_device),
    X360_HK(XUsbcamSetView, k_no_device), X360_HK(XUsbcamSnapshot, k_no_device),
    /* k_not_connected */
    X360_HK(XInputdControl, k_not_connected), X360_HK(XInputdFFCancelIo, k_not_connected),
    X360_HK(XInputdFFDeviceControl, k_not_connected), X360_HK(XInputdFFEffectOperation, k_not_connected),
    X360_HK(XInputdFFGetDeviceInfo, k_not_connected), X360_HK(XInputdFFSetDeviceGain, k_not_connected),
    X360_HK(XInputdFFSetEffect, k_not_connected), X360_HK(XInputdFFSetRumble, k_not_connected),
    X360_HK(XInputdFFUpdateEffect, k_not_connected), X360_HK(XInputdGetCapabilities, k_not_connected),
    X360_HK(XInputdGetDevicePid, k_not_connected), X360_HK(XInputdGetDeviceStats, k_not_connected),
    X360_HK(XInputdGetFailedConnectionOrBind, k_not_connected),
    X360_HK(XInputdGetTextDeviceKeyLocks, k_not_connected), X360_HK(XInputdNotify, k_not_connected),
    X360_HK(XInputdPassThroughRFCommand, k_not_connected), X360_HK(XInputdPowerDownDevice, k_not_connected),
    X360_HK(XInputdRawState, k_not_connected), X360_HK(XInputdReadState, k_not_connected),
    X360_HK(XInputdReadTextKeystroke, k_not_connected), X360_HK(XInputdResetDevice, k_not_connected),
    X360_HK(XInputdSendStayAliveRequest, k_not_connected), X360_HK(XInputdSetMinMaxAuthDelay, k_not_connected),
    X360_HK(XInputdSetRFPowerMode, k_not_connected), X360_HK(XInputdSetRadioFrequency, k_not_connected),
    X360_HK(XInputdSetRingOfLight, k_not_connected), X360_HK(XInputdSetTextDeviceKeyLocks, k_not_connected),
    X360_HK(XInputdSetTextMessengerIndicator, k_not_connected), X360_HK(XInputdSetWifiChannel, k_not_connected),
    X360_HK(XInputdWriteState, k_not_connected), X360_HK(XVoicedActivate, k_not_connected),
    X360_HK(XVoicedClose, k_not_connected), X360_HK(XVoicedGetBatteryStatus, k_not_connected),
    X360_HK(XVoicedGetDirectionalData, k_not_connected), X360_HK(XVoicedSendVPort, k_not_connected),
    X360_HK(XVoicedSubmitPacket, k_not_connected),
    /* k_refused */
    X360_HK(AniBlockOnAnimation, k_refused), X360_HK(AniSetLogo, k_refused),
    X360_HK(AniStartBootAnimation, k_refused), X360_HK(AniTerminateAnimation, k_refused),
    X360_HK(DevAuthGetStatistics, k_refused), X360_HK(DmPrintData, k_refused), X360_HK(DrvBindToUser, k_refused),
    X360_HK(DrvDeviceConfigChange, k_refused), X360_HK(DrvGetContentStorageNotification, k_refused),
    X360_HK(DrvXenonButtonPressed, k_refused), X360_HK(DumpGetRawDumpInfo, k_refused),
    X360_HK(DumpRegisterDedicatedDataBlock, k_refused), X360_HK(DumpSetCollectionFacility, k_refused),
    X360_HK(DumpUpdateDumpSettings, k_refused), X360_HK(DumpWriteDump, k_refused), X360_HK(DumpXitThread, k_refused),
    X360_HK(EtxBufferRegister, k_refused), X360_HK(EtxBufferUnregister, k_refused),
    X360_HK(EtxConsumerDisableEventType, k_refused), X360_HK(EtxConsumerEnableEventType, k_refused),
    X360_HK(EtxConsumerFlushBuffers, k_refused), X360_HK(EtxConsumerProcessLogs, k_refused),
    X360_HK(EtxConsumerRegister, k_refused), X360_HK(EtxConsumerUnregister, k_refused),
    X360_HK(EtxProducerLog, k_refused), X360_HK(EtxProducerLogV, k_refused), X360_HK(EtxProducerLogXwpp, k_refused),
    X360_HK(EtxProducerLogXwppV, k_refused), X360_HK(EtxProducerRegister, k_refused),
    X360_HK(EtxProducerUnregister, k_refused), X360_HK(ExReadModifyWriteXConfigSettingUlong, k_refused),
    X360_HK(ExSetBetaFeaturesEnabled, k_refused), X360_HK(ExSetXConfigSetting, k_refused),
    X360_HK(HalCallBackgroundModeNotificationRoutines, k_refused), X360_HK(HalConfigureVeDevice, k_refused),
    X360_HK(HalFinalizePowerLossRecovery, k_refused), X360_HK(HalOpenCloseODDTray, k_refused),
    X360_HK(HalPowerDownToBackgroundMode, k_refused), X360_HK(HalSendSMCMessage, k_refused),
    X360_HK(HalSetPowerLossRecovery, k_refused), X360_HK(IoAcquireCancelSpinLock, k_refused),
    X360_HK(IoAcquireDeviceObjectLock, k_refused), X360_HK(IoCallDriver, k_refused), X360_HK(IoCreateFile, k_refused),
    X360_HK(IoDismountVolume, k_refused), X360_HK(IoDismountVolumeByFileHandle, k_refused),
    X360_HK(IoDismountVolumeByName, k_refused), X360_HK(IoFreeIrp, k_refused), X360_HK(IoInitializeIrp, k_refused),
    X360_HK(IoQueueThreadIrp, k_refused), X360_HK(IoReleaseCancelSpinLock, k_refused),
    X360_HK(IoReleaseDeviceObjectLock, k_refused), X360_HK(IoSetIoCompletion, k_refused),
    X360_HK(IoStartNextPacket, k_refused), X360_HK(IoStartNextPacketByKey, k_refused),
    X360_HK(IoStartPacket, k_refused), X360_HK(IoSynchronousDeviceIoControlRequest, k_refused),
    X360_HK(IoSynchronousFsdRequest, k_refused), X360_HK(IptvGetAesCtrTransform, k_refused),
    X360_HK(IptvGetSessionKeyHash, k_refused), X360_HK(IptvSetBoundaryKey, k_refused),
    X360_HK(IptvSetSessionKey, k_refused), X360_HK(IptvVerifyOmac1Signature, k_refused),
    X360_HK(KeBlowFuses, k_refused), X360_HK(KeDisablePFMInterrupt, k_refused),
    X360_HK(KeEnablePFMInterrupt, k_refused), X360_HK(KeEnablePPUPerformanceMonitor, k_refused),
    X360_HK(KeSetPMWRegister, k_refused), X360_HK(KeSetPRVRegister, k_refused), X360_HK(KeSetProfilerISR, k_refused),
    X360_HK(KeSetSocRegister, k_refused), X360_HK(KeSetSpecialPurposeRegister, k_refused),
    X360_HK(NtAllocateEncryptedMemory, k_refused), X360_HK(NtFreeEncryptedMemory, k_refused),
    X360_HK(VvcHandlerCancelTransfers, k_refused), X360_HK(VvcHandlerRetrieveVoiceExtension, k_refused),
    X360_HK(XeKeysAesCbc, k_refused), X360_HK(XeKeysAesCbcUsingKey, k_refused), X360_HK(XeKeysDes2Cbc, k_refused),
    X360_HK(XeKeysDes2CbcUsingKey, k_refused), X360_HK(XeKeysDesCbc, k_refused),
    X360_HK(XeKeysDesCbcUsingKey, k_refused), X360_HK(XeKeysDvdAuthAP25GetTableVersion, k_refused),
    X360_HK(XeKeysDvdAuthAP25InstallTable, k_refused), X360_HK(XeKeysDvdAuthExActivate, k_refused),
    X360_HK(XeKeysDvdAuthExInstall, k_refused), X360_HK(XeKeysDvdAuthExSave, k_refused),
    X360_HK(XeKeysExGetKey, k_refused), X360_HK(XeKeysExSaveKeyVault, k_refused), X360_HK(XeKeysExSetKey, k_refused),
    X360_HK(XeKeysExecute, k_refused), X360_HK(XeKeysFcrtLoad, k_refused), X360_HK(XeKeysFcrtSave, k_refused),
    X360_HK(XeKeysFcrtSet, k_refused), X360_HK(XeKeysGeneratePrivateKey, k_refused),
    X360_HK(XeKeysGenerateRandomKey, k_refused), X360_HK(XeKeysGetConsoleCertificate, k_refused),
    X360_HK(XeKeysGetConsoleID, k_refused), X360_HK(XeKeysGetDigest, k_refused),
    X360_HK(XeKeysGetFactoryChallenge, k_refused), X360_HK(XeKeysGetMediaID, k_refused),
    X360_HK(XeKeysGetTruncatedSecondaryConsoleId, k_refused), X360_HK(XeKeysGetVersions, k_refused),
    X360_HK(XeKeysHmacSha, k_refused), X360_HK(XeKeysHmacShaUsingKey, k_refused),
    X360_HK(XeKeysInitializeFuses, k_refused), X360_HK(XeKeysLoadKeyVault, k_refused),
    X360_HK(XeKeysLockSystemUpdate, k_refused), X360_HK(XeKeysObscureKey, k_refused),
    X360_HK(XeKeysRevokeSaveSettings, k_refused), X360_HK(XeKeysRevokeUpdateDynamic, k_refused),
    X360_HK(XeKeysSaveBootLoader, k_refused), X360_HK(XeKeysSaveBootLoaderEx, k_refused),
    X360_HK(XeKeysSaveKeyVault, k_refused), X360_HK(XeKeysSaveSystemUpdate, k_refused),
    X360_HK(XeKeysSecurityInitialize, k_refused), X360_HK(XeKeysSecurityLoadSettings, k_refused),
    X360_HK(XeKeysSecuritySaveSettings, k_refused), X360_HK(XeKeysSecuritySetActivated, k_refused),
    X360_HK(XeKeysSecuritySetDetected, k_refused), X360_HK(XeKeysSecuritySetStat, k_refused),
    X360_HK(XeKeysSetFactoryResponse, k_refused), X360_HK(XeKeysSetKey, k_refused),
    X360_HK(XeKeysSetProtectedFlag, k_refused), X360_HK(XeKeysSetRevocationList, k_refused),
    X360_HK(XexActivationGetNonce, k_refused), X360_HK(XexActivationSetLicense, k_refused),
    X360_HK(XexCommitCodeBuffer, k_refused), X360_HK(XexLoadExecutable, k_refused),
    X360_HK(XexLoadImageFromMemory, k_refused), X360_HK(XexLoadImageHeaders, k_refused),
    X360_HK(XexRegisterPatchDescriptor, k_refused), X360_HK(XexRegisterUsermodeModule, k_refused),
    X360_HK(XexReserveCodeBuffer, k_refused), X360_HK(XexSetLastKdcTime, k_refused),
    X360_HK(XexShimDisable, k_refused), X360_HK(XexShimEnable, k_refused), X360_HK(XexShimEntryDisable, k_refused),
    X360_HK(XexShimEntryEnable, k_refused), X360_HK(XexShimEntryRegister, k_refused), X360_HK(XexShimLock, k_refused),
    X360_HK(XexStartExecutable, k_refused), X360_HK(XexTitleHash, k_refused), X360_HK(XexTitleHashClose, k_refused),
    X360_HK(XexTitleHashContinue, k_refused), X360_HK(XexTitleHashOpen, k_refused),
    X360_HK(XexTransformImageKey, k_refused), X360_HK(XexUnloadImageAndExitThread, k_refused),
    X360_HK(XexUnloadTitleModules, k_refused), X360_HK(XexVerifyImageHeaders, k_refused),
    /* k_refused_zero */
    X360_HK(HidGetLastInputTime, k_refused_zero), X360_HK(NicGetLinkState, k_refused_zero),
    X360_HK(UsbdGetDeviceDescriptor, k_refused_zero), X360_HK(UsbdGetDeviceRootPortType, k_refused_zero),
    X360_HK(UsbdGetDeviceSpeed, k_refused_zero), X360_HK(UsbdGetDeviceTopology, k_refused_zero),
    X360_HK(UsbdGetEndpointDescriptor, k_refused_zero), X360_HK(UsbdGetNatalHardwareVersion, k_refused_zero),
    X360_HK(UsbdGetNatalHub, k_refused_zero), X360_HK(UsbdGetPortDeviceNode, k_refused_zero),
    X360_HK(UsbdGetRequiredDrivers, k_refused_zero), X360_HK(UsbdGetRootHubDeviceNode, k_refused_zero),
    X360_HK(UsbdIsDeviceAuthenticated, k_refused_zero), X360_HK(WifiCalculateRegulatoryDomain, k_refused_zero),
    X360_HK(WifiChannelToFrequency, k_refused_zero), X360_HK(WifiDeduceNetworkType, k_refused_zero),
    X360_HK(WifiGetAssociationIE, k_refused_zero), X360_HK(WifiSelectAdHocChannel, k_refused_zero),
    X360_HK(XInputdGetLastTextInputTime, k_refused_zero), X360_HK(XVoicedHeadsetPresent, k_refused_zero),
    X360_HK(XVoicedIsActiveProcess, k_refused_zero), X360_HK(DbgPrompt, k_refused_zero),
    X360_HK(IoAllocateIrp, k_refused_zero), X360_HK(IoBuildAsynchronousFsdRequest, k_refused_zero),
    X360_HK(IoBuildDeviceIoControlRequest, k_refused_zero), X360_HK(IoBuildSynchronousFsdRequest, k_refused_zero),
    X360_HK(KeGetImagePageTableEntry, k_refused_zero), X360_HK(KeGetPMWRegister, k_refused_zero),
    X360_HK(KeGetPRVRegister, k_refused_zero), X360_HK(KeGetSocRegister, k_refused_zero),
    X360_HK(KeGetSpecialPurposeRegister, k_refused_zero), X360_HK(KeGetVidInfo, k_refused_zero),
    X360_HK(MmCreateKernelStack, k_refused_zero), X360_HK(MmDbgReadCheck, k_refused_zero),
    X360_HK(MmDbgWriteCheck, k_refused_zero), X360_HK(MmDoubleMapMemory, k_refused_zero),
    X360_HK(MmLockAndMapSegmentArray, k_refused_zero), X360_HK(MmMapIoSpace, k_refused_zero),
    X360_HK(XeKeysConsolePrivateKeySign, k_refused_zero), X360_HK(XeKeysConsoleSignatureVerification, k_refused_zero),
    X360_HK(XeKeysGetConsoleType, k_refused_zero), X360_HK(XeKeysGetKeyProperties, k_refused_zero),
    X360_HK(XeKeysGetProtectedFlag, k_refused_zero), X360_HK(XeKeysGetStatus, k_refused_zero),
    X360_HK(XeKeysGetUpdateSequence, k_refused_zero), X360_HK(XeKeysObfuscate, k_refused_zero),
    X360_HK(XeKeysObfuscateEx, k_refused_zero), X360_HK(XeKeysQwNeRsaPrvCrypt, k_refused_zero),
    X360_HK(XeKeysRevokeIsDeviceRevoked, k_refused_zero), X360_HK(XeKeysSecurityGetActivated, k_refused_zero),
    X360_HK(XeKeysSecurityGetDetected, k_refused_zero), X360_HK(XeKeysUnObfuscate, k_refused_zero),
    X360_HK(XeKeysUnObfuscateEx, k_refused_zero), X360_HK(XeKeysVerifyPIRSSignature, k_refused_zero),
    X360_HK(XeKeysVerifyRSASignature, k_refused_zero), X360_HK(XexActivationVerifyOwnership, k_refused_zero),
};
const unsigned x360_tab_ob_n = sizeof x360_tab_ob / sizeof x360_tab_ob[0];
