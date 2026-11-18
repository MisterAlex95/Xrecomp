/* x360_xam.c -- a first slice of xam.xex (XBA-270): what a title's start-up asks the system before its first
 * frame. Signatures and return values follow Xenia's xam_info.cc / xam_user.cc / xam_input.cc / xam_notify.cc (BSD).
 *
 * The machine looks like a console with user 0 signed in locally ("Player", offline), no controller connected
 * (pads come with the host layer: XBA-229 step 4), English, all regions, HDMI, no launch data, no notifications. */
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define X_ERROR_SUCCESS 0u
#define X_ERROR_INVALID_PARAMETER 0x57u
#define X_ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define X_ERROR_NOT_FOUND 0x490u
#define X_ERROR_NO_SUCH_USER 0x525u
#define X_E_INVALIDARG 0x80070057u
#define X_E_NO_SUCH_USER 0x80070525u
#define USER_XUID 0xB13EBABEBABEBABEull

void x360_x_XamGetSystemVersion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

void x360_x_XGetAVPack(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(6); /* HDMI */
}

void x360_x_XGetGameRegion(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0xFFFF);
}

void x360_x_XGetLanguage(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1); /* English */
}

void x360_x_XamGetCurrentTitleId(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x->title_id);
}

void x360_x_XamGetExecutionId(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (!A(0)) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, A(0), x->execution_info);
    RET(x->execution_info ? X_ERROR_SUCCESS : X_ERROR_NOT_FOUND);
}

void x360_x_XamLoaderGetLaunchDataSize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (!A(0)) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, A(0), 0);
    RET(X_ERROR_NOT_FOUND);
}

void x360_x_XamLoaderGetLaunchData(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_NOT_FOUND);
}

void x360_x_XamLoaderSetLaunchData(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamLoaderLaunchTitle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char name[128] = "";
    for (uint32_t k = 0; A(0) && k < sizeof name - 1; k++)
        if (!(name[k] = (char)x360_r8(x, A(0) + k)))
            break;
    x360_logf(x, "x360: XamLoaderLaunchTitle(\"%s\"): the title ends here\n", name);
    x->status = X360_EXITED;
    x->exit_code = 0;
}

void x360_x_XamAlloc(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t a = x360_heap_alloc(x, &x->heap_v4k, A(1), 0);
    if (A(2))
        x360_w32(x, A(2), a);
    RET(a ? X_ERROR_SUCCESS : 0x8007000Eu /* E_OUTOFMEMORY */);
}

void x360_x_XamFree(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_heap_free(x, &x->heap_v4k, A(0));
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamUserGetSigninState(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(A(0) == 0 ? 1 : 0); /* signed in locally */
}

void x360_x_XamUserGetXUID(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0) >= 4 || !A(2)) {
        RET(X_E_INVALIDARG);
        return;
    }
    if (A(0)) {
        RET(X_E_NO_SUCH_USER);
        return;
    }
    x360_w64(x, A(2), USER_XUID);
    RET(0);
}

static void put_name(x360_machine *x, uint32_t buf, uint32_t len)
{
    static const char name[] = "Player";
    for (uint32_t k = 0; k < len; k++) {
        x360_w8(x, buf + k, k < sizeof name ? (uint8_t)name[k] : 0);
        if (k >= sizeof name)
            break;
    }
}

void x360_x_XamUserGetName(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0) >= 4) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    if (A(0)) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    if (A(1) && A(2))
        put_name(x, A(1), A(2));
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamUserGetSigninInfo(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* X_USER_SIGNIN_INFO: xuid 8, unknown 4, signin state 4, unknown 8, name 16 */
    uint32_t info = A(2);
    if (A(0) >= 4 || !info) {
        RET(X_E_INVALIDARG);
        return;
    }
    memset(x->base + info, 0, 40);
    if (A(0)) {
        RET(X_E_NO_SUCH_USER);
        return;
    }
    x360_w64(x, info, USER_XUID);
    x360_w32(x, info + 12, 1);
    put_name(x, info + 24, 16);
    RET(0);
}

void x360_x_XamUserCheckPrivilege(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(2))
        x360_w32(x, A(2), 1);
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamUserIsOnlineEnabled(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

void x360_x_XamEnableInactivityProcessing(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamNotifyCreateListener(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* a notification object that is never signalled: no system notifications yet */
    uint32_t o = x360_kalloc(x, 16);
    x360_w8(x, o, X_OBJ_EVENT_NOTIFICATION);
    RET(x360_handle_new(x, o));
}

void x360_x_XNotifyGetNext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0); /* nothing pending */
}

void x360_x_XamShowSigninUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamFeatureEnabled(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

void x360_x_XamResetInactivity(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}
