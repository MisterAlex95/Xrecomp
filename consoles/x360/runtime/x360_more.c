/* x360_more.c -- more of the kernel and XAM surface a real title imports (XBA-270): the gaps a shipped title's import
 * list showed (names only were compared, from a public recompilation project; nothing of it is here). Signatures,
 * structure layouts and results follow Xenia's kernel (BSD): strings, I/O manager stubs, memory statistics, the
 * display information, the audio render driver (its client callback runs every 256-sample frame of virtual time on a
 * guest context, the frames it submits are dropped: no audio output yet), XMA contexts, content (save data:
 * a directory per content name under the data directory, mounted on the title's root name), enumerators, message
 * boxes and the device selector (answered at once, as headless), profile settings (defaults), the network (no
 * network: sockets fail), XMsg (no system applications). XeKeysGetKey answers zeros: no console key is here. */
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define A9(n) ((uint32_t)x360_stack_arg(x, c, (n)))
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define X_ERROR_SUCCESS 0u
#define X_ERROR_FILE_NOT_FOUND 2u
#define X_ERROR_NO_MORE_FILES 0x12u
#define X_ERROR_INVALID_PARAMETER 0x57u
#define X_ERROR_INSUFFICIENT_BUFFER 0x7Au
#define X_ERROR_ALREADY_EXISTS 0xB7u
#define X_ERROR_IO_PENDING 0x3E5u
#define X_ERROR_NOT_FOUND 0x490u
#define X_ERROR_FUNCTION_FAILED 0x65Bu

/* complete a XAM_OVERLAPPED {result, length, context, event, routine, routine context, extended error} at once */
static uint32_t finish(x360_machine *x, uint32_t ov, uint32_t result, uint32_t length)
{
    if (!ov)
        return result;
    x360_w32(x, ov, result);
    x360_w32(x, ov + 4, length);
    x360_w32(x, ov + 0x18, result);
    uint32_t ev = x360_r32(x, ov + 0xC);
    uint32_t o = ev ? x360_handle_obj(x, ev) : 0;
    if (o)
        x360_w32(x, o + 4, 1);
    return X_ERROR_IO_PENDING;
}

/* ---- strings ---- */

static void k_RtlCompareStringN(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (string 1, length 1, string 2, length 2, case insensitive) */
    uint32_t a = A(0), na = A(1), b = A(2), nb = A(3), ci = A(4) & 0xFF;
    uint32_t n = na < nb ? na : nb;
    for (uint32_t k = 0; k < n; k++) {
        int ca = x360_r8(x, a + k), cb = x360_r8(x, b + k);
        if (ci)
            ca = toupper(ca), cb = toupper(cb);
        if (ca != cb) {
            RET((uint32_t)(ca - cb));
            return;
        }
    }
    RET((uint32_t)((int)na - (int)nb));
}

static void k_RtlFreeAnsiString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t s = A(0), buf = s ? x360_r32(x, s + 4) : 0;
    if (buf)
        x360_heap_free(x, &x->heap_v4k, buf);
    if (s) {
        x360_w32(x, s, 0);
        x360_w32(x, s + 4, 0);
    }
    RET(0);
}

static void k_RtlMultiByteToUnicodeN(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (dst, dst bytes, written bytes*, src, src bytes) */
    uint32_t dst = A(0), cap = A(1) / 2, outp = A(2), src = A(3), n = A(4);
    uint32_t k = 0;
    for (; k < n && k < cap; k++)
        x360_w16(x, dst + 2 * k, x360_r8(x, src + k));
    if (outp)
        x360_w32(x, outp, 2 * k);
    RET(0);
}

static void k_RtlUnicodeToMultiByteN(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (dst, dst bytes, written bytes*, src, src bytes) */
    uint32_t dst = A(0), cap = A(1), outp = A(2), src = A(3), n = A(4) / 2;
    uint32_t k = 0;
    for (; k < n && k < cap; k++) {
        uint16_t ch = x360_r16(x, src + 2 * k);
        x360_w8(x, dst + k, ch < 256 ? ch : '?');
    }
    if (outp)
        x360_w32(x, outp, k);
    RET(0);
}

static void k_RtlUnicodeStringToAnsiString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (ANSI_STRING* dst, UNICODE_STRING* src, allocate) */
    uint32_t dst = A(0), src = A(1), alloc = A(2) & 0xFF;
    uint32_t len = x360_r16(x, src) / 2, sbuf = x360_r32(x, src + 4);
    uint32_t buf = alloc ? x360_heap_alloc(x, &x->heap_v4k, len + 1, 0) : x360_r32(x, dst + 4);
    uint32_t cap = alloc ? len + 1 : x360_r16(x, dst + 2);
    if (!buf || cap <= len) {
        RET(0x80000005u); /* STATUS_BUFFER_OVERFLOW */
        return;
    }
    for (uint32_t k = 0; k < len; k++) {
        uint16_t ch = x360_r16(x, sbuf + 2 * k);
        x360_w8(x, buf + k, ch < 256 ? ch : '?');
    }
    x360_w8(x, buf + len, 0);
    x360_w16(x, dst, len);
    x360_w16(x, dst + 2, cap);
    x360_w32(x, dst + 4, buf);
    RET(0);
}

/* ---- I/O manager, objects, misc (accepted) ---- */

static void k_ok(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

static void k_one(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1);
}

static void k_IoCreateDevice(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (driver object, extension size, name, type, characteristics, device object*): a zeroed device object */
    uint32_t dev = x360_kalloc(x, 0x40 + A(1));
    if (A(5))
        x360_w32(x, A(5), dev);
    RET(0);
}

static void k_MmQueryStatistics(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* X_MM_QUERY_STATISTICS_RESULT (104 bytes): size, total pages, kernel pages, title section (8 words + ...),
     * system section, highest page; the figures Xenia gives */
    uint32_t p = A(0);
    if (!p) {
        RET(X_STATUS_INVALID_PARAMETER);
        return;
    }
    if (x360_r32(x, p) != 104) {
        RET(0xC0000023u); /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    memset(x->base + p, 0, 104);
    uint32_t used = 0;
    for (uint32_t k = 0; k < x->heap_phys.pages; k++)
        used += x->heap_phys.used[k];
    x360_w32(x, p, 104);
    x360_w32(x, p + 4, 0x20000);
    x360_w32(x, p + 8, 0x300);
    static const uint32_t title[11] = {0, 0x2FFF0000u, 0x00160000u, 0x1000, 0x10, 0x100, 0x100, 0x100, 0x100,
                                       0x100, 0x100};
    for (int k = 0; k < 11; k++)
        x360_w32(x, p + 12 + 4 * (uint32_t)k, title[k]);
    x360_w32(x, p + 12, 0x20000 - used);
    x360_w32(x, p + 100, 0x1FFFF);
    RET(0);
}

static void k_NtReadFileScatter(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (handle, event, apc, apc context, iosb, segment array (4 KB pages), length, offset*): read into a bounce
     * buffer through NtReadFile, then spread over the pages */
    uint32_t segs = A(5), len = A(6);
    uint32_t tmp = x360_heap_alloc(x, &x->heap_v4k, len ? len : 1, 0);
    c->gpr[8] = tmp;
    x360_thunk th = {0, X360_xboxkrnl_NtReadFile, X360_LIB_XBOXKRNL};
    (void)t;
    x360_hle_call(x, &th);
    uint32_t st = (uint32_t)c->gpr[3], got = A(4) ? x360_r32(x, A(4) + 4) : len;
    for (uint32_t o = 0; o < got; o += 0x1000) {
        uint32_t page = x360_r32(x, segs + 8 * (o / 0x1000)); /* FILE_SEGMENT_ELEMENT: 64-bit each */
        if (!page)
            page = x360_r32(x, segs + 8 * (o / 0x1000) + 4);
        uint32_t n = got - o < 0x1000 ? got - o : 0x1000;
        memmove(x->base + page, x->base + tmp + o, n);
    }
    x360_heap_free(x, &x->heap_v4k, tmp);
    RET(st);
}

static void k_RtlCaptureContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* the CONTEXT x360_seh.c hands to handlers and RtlVirtualUnwind reads: as the caller will be when this returns */
    uint32_t p = A(0);
    if (p) {
        x360_seh_ctx k;
        memset(&k, 0, sizeof k);
        memcpy(k.gpr, c->gpr, sizeof k.gpr);
        memcpy(k.fpr, c->fpr, sizeof k.fpr);
        k.lr = c->lr, k.ctr = c->ctr, k.pc = c->pc, k.cr = c->cr;
        k.xer = c->xer_so << 31 | c->xer_ov << 30 | c->xer_ca << 29;
        memset(x->base + p, 0, 0x230);
        x360_seh_write_context(x, p, &k);
    }
    RET(0);
}

static void k_C_specific_handler(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1); /* ExceptionContinueSearch: structured exceptions are not dispatched in this runtime */
}

static void k_VdGetCurrentDisplayInformation(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t p = A(0);
    float hz = 60.0f;
    uint32_t bits;
    memcpy(&bits, &hz, 4);
    memset(x->base + p, 0, 0x58);
    x360_w16(x, p, 1280);
    x360_w16(x, p + 2, 720);
    x360_w16(x, p + 0x48, 1280);
    x360_w16(x, p + 0x4A, 720);
    x360_w32(x, p + 0x4C, bits);
    x360_w16(x, p + 0x56, 1280);
    RET(0);
}

static void k_VdInitializeScalerCommandBuffer(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (source xy, source wh, output xy, output wh, front buffer wh, vertical filter type, its parameters*, horizontal
     * filter type, its parameters*, unknown*, destination*, destination size in words): the title copies the words this
     * reports into its command buffer. No scaler here: the destination gets type 2 PM4 packets (NOPs), as in Xenia. */
    uint32_t dest = A9(10), count = A9(11);
    for (uint32_t k = 0; dest && k < count; k++)
        x360_w32(x, dest + 4 * k, 0x80000000u);
    RET(dest ? count : 0);
}

static void k_XeCryptBnQwBeSigVerify(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1); /* signatures accepted (no console public key here) */
}

static void k_XeKeysGetKey(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (key number, buffer, size*): zeros; no console key is in this repository */
    uint32_t sz = A(2) ? x360_r32(x, A(2)) : 0;
    if (A(1) && sz <= 0x1000)
        memset(x->base + A(1), 0, sz);
    RET(0);
}

static void k_XeCryptRotSumSha(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (input 1, size, input 2, size, digest, digest size): a checksum titles use on save data; SHA-1 of the two
     * inputs stands in for it (an approximation: a title only compares it with what it computed itself) */
    uint32_t digest = A(4), size = A(5);
    c->gpr[9] = digest, c->gpr[10] = size, c->gpr[7] = 0, c->gpr[8] = 0;
    x360_thunk th = {0, X360_xboxkrnl_XeCryptSha, X360_LIB_XBOXKRNL};
    (void)t;
    x360_hle_call(x, &th);
}

/* ---- audio: the render driver ---- */

#define AUDIO_FRAME_100NS 53333u /* 256 samples at 48 kHz */

void x360_audio_poll(x360_machine *x)
{
    uint64_t now = x360_now(x);
    for (int k = 0; k < 8; k++) {
        x360_audio_client *a = &x->audio[k];
        if (!a->callback || now < a->next)
            continue;
        a->next = now + AUDIO_FRAME_100NS - (now - a->next) % AUDIO_FRAME_100NS;
        if (k == 0 || !x->audio[0].callback)
            x360_audio_tick(x); /* a new frame: the last one's mix is complete */
        if (x360_callback_run(x, &a->thread, a->callback, a->arg, 0) == 0)
            a->frames_requested++;
    }
}

uint64_t x360_audio_next_event(x360_machine *x)
{
    uint64_t d = 0;
    for (int k = 0; k < 8; k++)
        if (x->audio[k].callback && (!d || x->audio[k].next < d))
            d = x->audio[k].next;
    return d;
}

static void k_XAudioRegisterRenderDriverClient(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (callback*: {routine, argument}, driver*): the driver handle is 0x41550000 | index (Xenia) */
    uint32_t cb = A(0);
    for (int k = 0; k < 8; k++)
        if (!x->audio[k].callback) {
            x->audio[k].callback = x360_r32(x, cb);
            x->audio[k].arg = x360_r32(x, cb + 4);
            x->audio[k].next = x360_now(x) + AUDIO_FRAME_100NS;
            x->audio[k].thread = -1;
            if (A(1))
                x360_w32(x, A(1), 0x41550000u | (uint32_t)k);
            RET(0);
            return;
        }
    RET(X_ERROR_FUNCTION_FAILED);
}

static void k_XAudioUnregisterRenderDriverClient(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t k = A(0) & 0xFFFF;
    if (k < 8)
        x->audio[k].callback = 0;
    RET(0);
}

/* --audio FILE.wav: the frames clients submit, as the render driver would play them (Xenia's audio system: 256
 * samples of 6 channels, channel after channel, big-endian floats, 48 kHz), written interleaved to a 32-bit float
 * WAV whose sizes x360_audio_close fills in. Several clients' frames are written one after the other (no mixing). */
static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint8_t h[44] = "RIFF....WAVEfmt ";
    uint32_t v[9] = {36 + data_bytes, 16, 3 | 6u << 16 /* IEEE float, 6 channels */, 48000, 48000 * 24, 24 | 32u << 16,
                     0x61746164u /* "data" */, data_bytes, 0};
    for (int i = 0; i < 4; i++)
        h[4 + i] = (uint8_t)(v[0] >> (8 * i));
    for (int k = 1; k < 8; k++)
        for (int i = 0; i < 4; i++)
            h[12 + 4 * k + i] = (uint8_t)(v[k] >> (8 * i));
    memcpy(h + 36, "data", 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

void x360_set_audio_out(x360_machine *x, FILE *f)
{
    x->audio_out = f;
    x->audio_bytes = 0;
    if (f)
        wav_header(f, 0);
}

void x360_audio_close(x360_machine *x)
{
    if (x->audio_out) {
        wav_header(x->audio_out, x->audio_bytes);
        fflush(x->audio_out);
        x->audio_out = NULL;
    }
}

static void k_XAudioSubmitRenderDriverFrame(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t k = A(0) & 0xFFFF;
    if (k < 8)
        x->audio[k].frames_submitted++;
    if (A(1)) { /* to the mix the host plays (x360_audio_pull) */
        float planar[256 * 6];
        for (int i = 0; i < 256 * 6; i++) {
            uint32_t bits = x360_r32(x, A(1) + 4 * (uint32_t)i);
            memcpy(&planar[i], &bits, 4);
        }
        x360_audio_mix_frame(x, (int)k, planar);
    }
    if (x->audio_out && A(1)) {
        float out[256 * 6];
        for (int s = 0; s < 256; s++)
            for (int ch = 0; ch < 6; ch++) {
                uint32_t bits = x360_r32(x, A(1) + 4 * (uint32_t)(ch * 256 + s));
                memcpy(&out[s * 6 + ch], &bits, 4);
            }
        fwrite(out, 4, 256 * 6, x->audio_out);
        x->audio_bytes += 4 * 256 * 6;
    }
    RET(0);
}

static void k_XAudioGetVoiceCategoryVolume(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    float one = 1.0f;
    uint32_t bits;
    memcpy(&bits, &one, 4);
    if (A(1))
        x360_w32(x, A(1), bits);
    RET(0);
}

static void k_XAudioGetVoiceCategoryVolumeChangeMask(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(1))
        x360_w32(x, A(1), 0);
    RET(0);
}


/* ---- XAM: video, content, enumerators, UI, profile, network, messages ---- */

static void x_XGetVideoMode(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    x360_thunk th = {0, X360_xboxkrnl_VdQueryVideoMode, X360_LIB_XBOXKRNL};
    (void)t;
    x360_hle_call(x, &th);
    (void)c;
}

static void x_XamShowDeviceSelectorUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (user, content type, flags, bytes requested (64-bit), device id*, overlapped) */
    if (A(4))
        x360_w32(x, A(4), 1);
    RET(finish(x, A(5), X_ERROR_SUCCESS, 0));
}

static void x_XamShowMessageBoxUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (user, title, text, buttons, button texts, active button, flags, result*, overlapped): the first button,
     * at once (headless) */
    char text[160] = "";
    for (uint32_t k = 0; A(2) && k < sizeof text - 1; k++) {
        uint16_t ch = x360_r16(x, A(2) + 2 * k);
        if (!ch)
            break;
        text[k] = ch < 128 ? (char)ch : '?';
    }
    x360_logf(x, "x360: message box: \"%s\" (answered with its first button)\n", text);
    if (A(7))
        x360_w32(x, A(7), 0);
    RET(finish(x, A9(8), X_ERROR_SUCCESS, 0));
}

static void x_XamShowDirtyDiscErrorUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t, (void)c;
    x360_fail(x, "the title reported a disc read error (XamShowDirtyDiscErrorUI)");
}

static void x_net_fail(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0xFFFFFFFFu); /* INVALID_SOCKET / SOCKET_ERROR: no network */
}

static void x_net_ok(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

static void x_NetDll_WSAStartup(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (caller, version, WSADATA*): version 2.2, no description */
    if (A(2)) {
        memset(x->base + A(2), 0, 0x190);
        x360_w16(x, A(2), A(1) & 0xFFFF);
        x360_w16(x, A(2) + 2, 0x0202);
    }
    RET(0);
}

static void x_NetDll_XNetGetTitleXnAddr(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (caller, XNADDR*): loopback, as Xenia; XNET_GET_XNADDR_STATIC | ETHERNET */
    if (A(1)) {
        memset(x->base + A(1), 0, 0x24);
        x360_w32(x, A(1), 0x7F000001u);
    }
    RET(0x4 | 0x2);
}

static void x_NetDll_inet_addr(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char s[32] = "";
    for (int k = 0; A(1) && k < 31; k++)
        if (!(s[k] = (char)x360_r8(x, A(1) + (uint32_t)k)))
            break;
    unsigned a, b, cc, d;
    RET(sscanf(s, "%u.%u.%u.%u", &a, &b, &cc, &d) == 4 && a < 256 && b < 256 && cc < 256 && d < 256
            ? a << 24 | b << 16 | cc << 8 | d
            : 0xFFFFFFFFu);
}

static void x_XMsg_not_found(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_NOT_FOUND); /* no system application answers */
}

static void x_XMsgStartIORequest(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(finish(x, A(2), X_ERROR_NOT_FOUND, 0));
}

#define KM(name, fn) {X360_LIB_XBOXKRNL, X360_xboxkrnl_##name, fn}
#define XM2(name, fn) {X360_LIB_XAM, X360_xam_##name, fn}

static const x360_hle_row MORE[] = {
    KM(RtlCompareStringN, k_RtlCompareStringN),
    KM(RtlFreeAnsiString, k_RtlFreeAnsiString), KM(RtlMultiByteToUnicodeN, k_RtlMultiByteToUnicodeN),
    KM(RtlUnicodeToMultiByteN, k_RtlUnicodeToMultiByteN),
    KM(RtlUnicodeStringToAnsiString, k_RtlUnicodeStringToAnsiString),
    KM(ExRegisterTitleTerminateNotification, k_ok), KM(FscSetCacheElementCount, k_ok),
    KM(IoCheckShareAccess, k_ok), KM(IoSetShareAccess, k_ok), KM(IoRemoveShareAccess, k_ok),
    KM(IoCompleteRequest, k_ok), KM(IoCreateDevice, k_IoCreateDevice), KM(IoDeleteDevice, k_ok),
    KM(IoInvalidDeviceRequest, k_ok), KM(KeEnableFpuExceptions, k_ok), KM(KiApcNormalRoutineNop, k_ok),
    KM(MmQueryStatistics, k_MmQueryStatistics), KM(NtFlushBuffersFile, k_ok),
    KM(NtReadFileScatter, k_NtReadFileScatter), KM(ObIsTitleObject, k_one), KM(ObReferenceObject, k_ok),
    KM(RtlCaptureContext, k_RtlCaptureContext), KM(StfsControlDevice, k_ok), KM(StfsCreateDevice, k_ok),
    KM(VdGetCurrentDisplayInformation, k_VdGetCurrentDisplayInformation),
    KM(VdInitializeScalerCommandBuffer, k_VdInitializeScalerCommandBuffer),
    KM(VdSetSystemCommandBufferGpuIdentifierAddress, k_ok),
    KM(XAudioRegisterRenderDriverClient, k_XAudioRegisterRenderDriverClient),
    KM(XAudioUnregisterRenderDriverClient, k_XAudioUnregisterRenderDriverClient),
    KM(XAudioSubmitRenderDriverFrame, k_XAudioSubmitRenderDriverFrame),
    KM(XAudioGetVoiceCategoryVolume, k_XAudioGetVoiceCategoryVolume),
    KM(XAudioGetVoiceCategoryVolumeChangeMask, k_XAudioGetVoiceCategoryVolumeChangeMask),
    KM(XeCryptBnQwBeSigVerify, k_XeCryptBnQwBeSigVerify),
    KM(XeCryptRotSumSha, k_XeCryptRotSumSha), KM(XeKeysGetKey, k_XeKeysGetKey),
    KM(__C_specific_handler, k_C_specific_handler),
    XM2(XGetVideoMode, x_XGetVideoMode),
    XM2(XamShowDeviceSelectorUI, x_XamShowDeviceSelectorUI), XM2(XamShowMessageBoxUI, x_XamShowMessageBoxUI),
    XM2(XamShowMessageBoxUIEx, x_XamShowMessageBoxUI), XM2(XamShowDirtyDiscErrorUI, x_XamShowDirtyDiscErrorUI),
    XM2(NetDll_WSAStartup, x_NetDll_WSAStartup), XM2(NetDll_WSACleanup, x_net_ok), XM2(NetDll_XNetStartup, x_net_ok),
    XM2(NetDll_XNetGetTitleXnAddr, x_NetDll_XNetGetTitleXnAddr), XM2(NetDll___WSAFDIsSet, x_net_ok),
    XM2(NetDll_inet_addr, x_NetDll_inet_addr), XM2(NetDll_socket, x_net_fail), XM2(NetDll_accept, x_net_fail),
    XM2(NetDll_bind, x_net_fail), XM2(NetDll_closesocket, x_net_ok), XM2(NetDll_connect, x_net_fail),
    XM2(NetDll_listen, x_net_fail), XM2(NetDll_recv, x_net_fail), XM2(NetDll_select, x_net_fail),
    XM2(NetDll_send, x_net_fail), XM2(NetDll_setsockopt, x_net_fail),
    XM2(XMsgInProcessCall, x_XMsg_not_found), XM2(XMsgStartIORequest, x_XMsgStartIORequest),
    XM2(XMsgStartIORequestEx, x_XMsgStartIORequest),
};

void x360_more_register(x360_machine *x)
{
    x360_hle_add(x, MORE, sizeof MORE / sizeof MORE[0]);
}
