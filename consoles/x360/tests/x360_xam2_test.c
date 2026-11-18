/* x360_xam2_test -- the xam2 HLE group called directly (XBA-296). */
#include <stdlib.h>
#include <unistd.h>

#include "x360_hle_util.h"

#define IO_PENDING 0x3E5u
#define NO_MORE_FILES 0x12u
#define INSUFFICIENT 0x7Au
#define FUNCTION_FAILED 0x65Bu
#define NOT_LOGGED_ON 0x524u
#define XUID 0xB13EBABEBABEBABEull

static uint32_t event_new(void)
{
    uint32_t p = galloc(4);
    K(X360_xboxkrnl_NtCreateEvent, 4, p, 0, 0, 0);
    return x360_r32(X, p);
}

static int event_state(uint32_t h) { return (int)x360_r32(X, x360_handle_obj(X, h) + 4); }

/* an overlapped with its own event */
static uint32_t ov_new(uint32_t *event)
{
    uint32_t ov = galloc(0x1C);
    *event = event_new();
    x360_w32(X, ov + 0xC, *event);
    return ov;
}

static uint32_t content_data(const char *name, uint32_t type)
{
    uint32_t cd = galloc(0x148);
    x360_w32(X, cd, 1);
    x360_w32(X, cd + 4, type);
    x360_w16(X, cd + 8, 'S');
    memcpy(X->base + cd + 0x108, name, strlen(name) + 1);
    return cd;
}

static int wstr_is(uint32_t a, const char *s)
{
    uint32_t k = 0;
    for (; s[k]; k++)
        if (x360_r16(X, a + 2 * k) != (uint8_t)s[k])
            return 0;
    return x360_r16(X, a + 2 * k) == 0;
}

static void test_overlapped(void)
{
    uint32_t ev, ov = ov_new(&ev), len = galloc(4);
    CHECK(XAM(X360_xam_XamContentGetDeviceState, 2, 1, ov) == IO_PENDING, "pending through an overlapped");
    CHECK(x360_r32(X, ov) == 0 && event_state(ev) == 1, "result written, event signalled");
    CHECK(XAM(X360_xam_XamGetOverlappedResult, 3, ov, len, 0) == 0, "overlapped result");
    CHECK(XAM(X360_xam_XGetOverlappedResult, 3, ov, len, 1) == 0, "XGetOverlappedResult");

    uint32_t ev2, ov2 = ov_new(&ev2);
    CHECK(XAM(X360_xam_XamContentGetDeviceState, 2, 9, ov2) == IO_PENDING && x360_r32(X, ov2) == FUNCTION_FAILED &&
              x360_r32(X, ov2 + 0x18) == 0x48F && event_state(ev2) == 1, "an error completes the overlapped too");
    CHECK(XAM(X360_xam_XGetOverlappedExtendedError, 1, ov2) == 0x48F, "extended error");
    CHECK(XAM(X360_xam_XamGetOverlappedResult, 3, ov2, len, 0) == FUNCTION_FAILED, "the failed result");
    uint32_t ev4, ov4 = ov_new(&ev4);
    CHECK(XAM(X360_xam_XamContentOpenFile, 7, 0, 0, 0, 0, 0, 0, ov4) == IO_PENDING && x360_r32(X, ov4) == 2 &&
              x360_r32(X, ov4 + 4) == 0xFFFFFFFFu && x360_r32(X, ov4 + 0x18) == 2, "a plain failure: length -1");
    CHECK(XAM(X360_xam_XamContentGetDeviceState, 2, 9, 0) == 0x48F, "no overlapped: the result itself");

    uint32_t ev3, ov3 = ov_new(&ev3);
    x360_w32(X, ov3, IO_PENDING);
    CHECK(XAM(X360_xam_XamGetOverlappedResult, 3, ov3, len, 0) == 0x3E4, "still pending: incomplete");
    CHECK(XAM(X360_xam_XGetOverlappedExtendedError, 1, ov3) == 0x3E4, "still pending: no extended error");
    CHECK(XAM(X360_xam_XMsgCompleteIORequest, 4, ov3, 5, 6, 7) == 0 && x360_r32(X, ov3) == 5 &&
              x360_r32(X, ov3 + 4) == 7 && x360_r32(X, ov3 + 0x18) == 6 && event_state(ev3) == 1,
          "XMsgCompleteIORequest");
}

static void test_notifications(void)
{
    uint32_t idp = galloc(4), parp = galloc(4);
    uint32_t h = XAM(X360_xam_XamNotifyCreateListener, 2, 1, 10);
    CHECK(h != 0, "listener");
    /* the first system listener is told the start-up state, in order */
    static const uint32_t burst[][2] = {{9, 1}, {9, 0}, {10, 1}, {10, 1}, {0x12, 0}, {0x12, 0}, {0x13, 0}, {0x13, 0}};
    CHECK(event_state(h) == 1, "signalled while something is queued");
    for (int k = 0; k < 8; k++) {
        CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0, idp, parp) == 1, "start-up notification %d", k);
        CHECK(x360_r32(X, idp) == burst[k][0] && x360_r32(X, parp) == burst[k][1], "start-up %d: %X %u", k,
              x360_r32(X, idp), x360_r32(X, parp));
    }
    CHECK(event_state(h) == 0, "reset once empty");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0, idp, parp) == 0 && x360_r32(X, idp) == 0, "nothing left");

    /* a second listener gets no burst; broadcasts reach both, first in first out */
    uint32_t h2 = XAM(X360_xam_XamNotifyCreateListener, 2, 1, 10);
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h2, 0, idp, parp) == 0, "the burst is sent once");
    XAM(X360_xam_XNotifyBroadcast, 2, 0x20, 1);
    XAM(X360_xam_XNotifyBroadcast, 2, 0x21, 2);
    XAM(X360_xam_XNotifyBroadcast, 2, 0x22, 3);
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0x21, idp, parp) == 1 && x360_r32(X, parp) == 2, "taken by id");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0x77, idp, parp) == 0, "an id not queued");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0, idp, parp) == 1 && x360_r32(X, idp) == 0x20, "order kept (1)");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h, 0, idp, parp) == 1 && x360_r32(X, idp) == 0x22, "order kept (2)");
    for (uint32_t id = 0x20; id <= 0x22; id++)
        CHECK(XAM(X360_xam_XNotifyGetNext, 4, h2, 0, idp, parp) == 1 && x360_r32(X, idp) == id, "second listener");

    /* the mask (bits 25 up of the id) and the version (bits 16 up) filter */
    uint32_t h3 = XAM(X360_xam_XamNotifyCreateListener, 2, 2, 1);
    XAM(X360_xam_XNotifyBroadcast, 2, 0x30, 0);          /* area 0: not in the mask */
    XAM(X360_xam_XNotifyBroadcast, 2, 0x02050031u, 0);   /* area 1, version 5: too new */
    XAM(X360_xam_XNotifyBroadcast, 2, 0x02010032u, 7);   /* area 1, version 1 */
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h3, 0, idp, parp) == 1 && x360_r32(X, idp) == 0x02010032u, "filtered");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h3, 0, idp, parp) == 0, "only that one");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, 0xF8000FFCu, 0, idp, parp) == 0, "not a listener");

    /* a UI shown and gone is two notifications */
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h2, 0, idp, parp) == 1 && x360_r32(X, idp) == 0x30, "area 0 reached it");
    XAM(X360_xam_XamShowFriendsUI, 1, 0);
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h2, 0, idp, parp) == 1 && x360_r32(X, idp) == 9 && x360_r32(X, parp) == 1,
          "UI on");
    CHECK(XAM(X360_xam_XNotifyGetNext, 4, h2, 0, idp, parp) == 1 && x360_r32(X, idp) == 9 && x360_r32(X, parp) == 0,
          "UI off");
    CHECK(XAM(X360_xam_XNotifyDelayUI, 1, 100) == 0, "delay");
}

static void test_ui(void)
{
    uint32_t def = gwstr("Hello"), buf = galloc(64), ev, ov = ov_new(&ev);
    CHECK(XAM(X360_xam_XamIsUIActive, 0) == 0, "no UI stays active");
    CHECK(XAM(X360_xam_XamShowKeyboardUI, 8, 0, 0, def, 0, 0, buf, 32, ov) == IO_PENDING, "keyboard: pending");
    CHECK(wstr_is(buf, "Hello"), "keyboard: the default text");
    CHECK(x360_r32(X, ov) == 0 && x360_r32(X, ov + 4) == 5 && event_state(ev) == 1, "keyboard: completed");
    CHECK(XAM(X360_xam_XamShowKeyboardUI, 8, 0, 0, def, 0, 0, buf, 3, 0) == 0 && wstr_is(buf, "He"), "truncated");
    CHECK(XAM(X360_xam_XamShowKeyboardUI, 8, 0, 0, 0, 0, 0, buf, 32, 0) == 0 && wstr_is(buf, ""), "no default text");
    CHECK(XAM(X360_xam_XamShowKeyboardUI, 8, 0, 0, def, 0, 0, 0, 32, 0) == 0x57, "no buffer");
    CHECK(XAM(X360_xam_XamShowGamerCardUI, 1, 0) == 0 && XAM(X360_xam_XamShowAchievementsUI, 2, 0, 0) == 0, "local UI");
    CHECK(XAM(X360_xam_XamShowGamerCardUI, 1, 2) == 0x525, "a user who is not there");
    CHECK(XAM(X360_xam_XamShowPartyUI, 1, 0) == FUNCTION_FAILED, "party UI");
    CHECK(XAM(X360_xam_XamShowCommunitySessionsUI, 2, 0, 0) == FUNCTION_FAILED, "community sessions UI");
    uint32_t offers = galloc(8), hr = galloc(4), ev2, ov2 = ov_new(&ev2);
    CHECK(XAM(X360_xam_XamShowMarketplaceDownloadItemsUI, 6, 0, 0, offers, 1, hr, ov2) == IO_PENDING &&
              event_state(ev2) == 1 && x360_r32(X, hr) == 0x80004005u, "nothing downloaded");
}

static void test_tasks(void)
{
    uint32_t hp = galloc(4);
    CHECK(XAM(X360_xam_XamTaskSchedule, 4, 0x82000000u, 0x1234, 0, hp) == 0 && x360_r32(X, hp) != 0, "task scheduled");
    x360_thread *n = x360_thread_by_kthread(X, x360_handle_obj(X, x360_r32(X, hp)));
    CHECK(n && n->cpu.pc == 0x82000000u && n->cpu.gpr[3] == 0x1234, "the task runs its callback with the message");
    CHECK(XAM(X360_xam_XamTaskShouldExit, 0) == 0, "should not exit");
    CHECK(XAM(X360_xam_XamTaskCloseHandle, 1, x360_r32(X, hp)) == 1, "closed");
    CHECK(XAM(X360_xam_XamTaskCloseHandle, 1, x360_r32(X, hp)) == 0, "closed twice");
}

static void test_profile(void)
{
    uint32_t ids = galloc(12), sizep = galloc(4), buf = galloc(0x1000);
    x360_w32(X, ids, 0x10040003u);     /* an integer with a default */
    x360_w32(X, ids + 4, 0x63E83FFFu); /* title-specific binary, 1000 bytes */
    x360_w32(X, sizep, 0);
    uint32_t need = 8 + 2 * 40 + 1000;
    CHECK(XAM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 0, 0, 2, ids, sizep, 0, 0) == INSUFFICIENT &&
              x360_r32(X, sizep) == need, "size asked: %u", x360_r32(X, sizep));
    x360_w32(X, sizep, need - 1);
    CHECK(XAM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 0, 0, 2, ids, sizep, buf, 0) == INSUFFICIENT &&
              x360_r32(X, sizep) == need, "a buffer one byte short");
    uint32_t ev, ov = ov_new(&ev);
    CHECK(XAM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 0, 0, 2, ids, sizep, buf, ov) == IO_PENDING &&
              x360_r32(X, ov) == 0 && event_state(ev) == 1, "the right size");
    CHECK(x360_r32(X, buf) == 2 && x360_r32(X, buf + 4) == buf + 8, "header");
    CHECK(x360_r32(X, buf + 8 + 16) == 0x10040003u && x360_r32(X, buf + 8 + 32) == 3 && x360_r32(X, buf + 8) == 1,
          "the default value");
    CHECK(x360_r32(X, buf + 48) == 0 && x360_r32(X, buf + 48 + 32) == 0, "nothing stored yet");

    /* written, then read back through the Ex call (buffer and overlapped on the stack) */
    uint32_t s = galloc(80), blob = galloc(8);
    memcpy(X->base + blob, "SAVE", 4);
    x360_w32(X, s + 16, 0x10040003u);
    x360_w8(X, s + 24, 1);
    x360_w32(X, s + 32, 77);
    x360_w32(X, s + 40 + 16, 0x63E83FFFu);
    x360_w8(X, s + 40 + 24, 6);
    x360_w32(X, s + 40 + 32, 4);
    x360_w32(X, s + 40 + 36, blob);
    CHECK(XAM(X360_xam_XamUserWriteProfileSettings, 5, 0, 0, 2, s, 0) == 0, "written");
    CHECK(XAM(X360_xam_XamUserReadProfileSettingsEx, 10, 0, 0, 0, 0, 2, ids, sizep, 0, buf, 0) == 0, "read back");
    CHECK(x360_r32(X, buf + 8 + 32) == 77, "the integer");
    uint32_t b = buf + 48;
    CHECK(x360_r32(X, b) == 2 && x360_r32(X, b + 32) == 4 && !memcmp(X->base + x360_r32(X, b + 36), "SAVE", 4),
          "the title's own data");
    CHECK(XAM(X360_xam_XamUserWriteProfileSettings, 5, 0, 1, 1, s, 0) == 0x525, "write: no such user");
    CHECK(XAM(X360_xam_XamUserWriteProfileSettings, 5, 0, 0, 0, s, 0) == 0x57, "write: nothing");
    CHECK(XAM(X360_xam_XamUserReadProfileSettings, 9, 0, 1, 0, 0, 2, ids, sizep, buf, 0) == 0x525, "read: no such user");

    /* by XUID: this profile's and a stranger's */
    uint32_t xs = galloc(16);
    x360_w64(X, xs, XUID);
    x360_w64(X, xs + 8, 0x1234);
    x360_w32(X, sizep, 0x1000);
    CHECK(XAM(X360_xam_XamUserReadProfileSettings, 9, 0, 0, 2, xs, 1, ids, sizep, buf, 0) == 0 &&
              x360_r32(X, buf) == 2, "two users");
    CHECK(x360_r64(X, buf + 8 + 8) == XUID && x360_r32(X, buf + 8 + 32) == 77, "this profile");
    CHECK(x360_r64(X, buf + 48 + 8) == 0x1234 && x360_r32(X, buf + 48) == 0 && x360_r32(X, buf + 48 + 32) == 0,
          "a stranger's setting is not set");
}

static void test_user(void)
{
    uint32_t out = galloc(16), xs = galloc(8), ev, ov = ov_new(&ev);
    CHECK(XAM(X360_xam_XamUserGetMembershipTier, 1, 0) == 0, "a local profile has no membership");
    x360_w32(X, out, 9);
    CHECK(XAM(X360_xam_XamUserAreUsersFriends, 5, 0, xs, 1, out, ov) == IO_PENDING && x360_r32(X, ov) == FUNCTION_FAILED &&
              x360_r32(X, ov + 0x18) == (0x80070000u | NOT_LOGGED_ON) && x360_r32(X, out) == 0 && event_state(ev) == 1,
          "friends need Live");
    CHECK(XAM(X360_xam_XamUserAreUsersFriends, 5, 0, xs, 1, out, 0) == NOT_LOGGED_ON, "friends: no overlapped");
    CHECK(XAM(X360_xam_XamUserAreUsersFriends, 5, 4, xs, 1, out, 0) == 0x57, "friends: a bad index");
    CHECK(XAM(X360_xam_XamUserContentRestrictionGetFlags, 2, 0, out) == 0 && x360_r32(X, out) == 0, "no restrictions");
    CHECK(XAM(X360_xam_XamUserContentRestrictionGetRating, 4, 0, 0, out, out + 4) == 0 && x360_r32(X, out) == 0x3F,
          "rating");
    uint32_t ev2, ov2 = ov_new(&ev2);
    CHECK(XAM(X360_xam_XamUserContentRestrictionCheckAccess, 7, 0, 0, 0, 0, 0, out, ov2) == IO_PENDING &&
              x360_r32(X, out) == 1 && event_state(ev2) == 1, "access allowed");
    CHECK(XAM(X360_xam_XamUserGetDeviceContext, 3, 0, 0, out) == 0, "device context");
    CHECK(XAM(X360_xam_XamUserGetDeviceContext, 3, 1, 0, out) == 0x8007048Fu, "no device for another user");
    /* tiles and sessions: none here */
    uint32_t ev3, ov3 = ov_new(&ev3);
    CHECK(XAM(X360_xam_XamReadTileToTexture, 8, 0, 0, 5, 0, out, 4, 4, ov3) == IO_PENDING && x360_r32(X, ov3) == 2 &&
              event_state(ev3) == 1, "no tile");
    CHECK(XAM(X360_xam_XamReadTileToTexture, 8, 0, 0, 0, 0, out, 4, 4, 0) == 0x57, "tile 0");
    CHECK(XAM(X360_xam_XamWriteGamerTile, 6, 0, 0, 0, 0, 0, 0) == FUNCTION_FAILED, "tiles cannot be written");
    CHECK(XAM(X360_xam_XamSessionCreateHandle, 1, out) == FUNCTION_FAILED && x360_r32(X, out) == 0, "no session");
    CHECK(XAM(X360_xam_XamSessionRefObjByHandle, 2, 0x1234, out) == 6, "no session object");
    CHECK(XAM(X360_xam_XamParseGamerTileKey, 4, 0, out, out, out) == 0x57, "no tile key");
}

static void test_enumerators(void)
{
    uint32_t sizep = galloc(4), hp = galloc(4), countp = galloc(4), buf = galloc(0x1000);

    /* one device, then the end */
    CHECK(XAM(X360_xam_XamContentCreateDeviceEnumerator, 5, 0, 0, 2, sizep, hp) == 0 && x360_r32(X, sizep) == 0xA0,
          "device enumerator");
    uint32_t h = x360_r32(X, hp), e = galloc(4);
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0x10, countp, 0) == INSUFFICIENT, "smaller than an item");
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0xA0, countp, 0) == 0 && x360_r32(X, countp) == 1, "one device");
    CHECK(x360_r32(X, buf) == 1 && x360_r32(X, buf + 4) == 1 && wstr_is(buf + 24, "Hard Drive"), "the hard drive");
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0xA0, countp, 0) == NO_MORE_FILES && x360_r32(X, countp) == 0,
          "the end");
    CHECK(XAM(X360_xam_XamGetPrivateEnumStructureFromHandle, 2, h, e) == 0 && x360_r32(X, e) != 0, "its object");
    CHECK(XAM(X360_xam_XamGetPrivateEnumStructureFromHandle, 2, 0xF8000FFCu, e) == 0xC0000008u, "not an enumerator");
    CHECK(XAM(X360_xam_XamEnumerate, 6, 0xF8000FFCu, 0, buf, 0x100, countp, 0) == 6, "not a handle");
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, 0, 0x100, countp, 0) == 0x57, "no buffer");
    CHECK(XAM(X360_xam_XamCreateEnumeratorHandle, 0) == 0x57, "XamCreateEnumeratorHandle");

    /* empty ones end at once, through an overlapped too */
    CHECK(XAM(X360_xam_XamUserCreateAchievementEnumerator, 8, 0, 0, 0, 0, 0, 4, sizep, hp) == 0 &&
              x360_r32(X, sizep) == 36 * 4, "achievements");
    uint32_t ev, ov = ov_new(&ev);
    CHECK(XAM(X360_xam_XamEnumerate, 6, x360_r32(X, hp), 0, buf, 0x100, 0, ov) == IO_PENDING &&
              x360_r32(X, ov) == NO_MORE_FILES && x360_r32(X, ov + 0x18) == 0x80070012u && x360_r32(X, ov + 4) == 0 &&
              event_state(ev) == 1, "no achievement");
    CHECK(XAM(X360_xam_XamUserCreateAchievementEnumerator, 8, 0, 0, 0, 1, 0, 2, sizep, hp) == 0 &&
              x360_r32(X, sizep) == 500 * 2, "achievements with their strings");
    CHECK(XAM(X360_xam_XamUserCreateAchievementEnumerator, 8, 0, 0, 0, 0, 0, 0, sizep, hp) == 0x57, "no count");
    CHECK(XAM(X360_xam_XamUserCreateStatsEnumerator, 8, 0, 0, 2, 0, 0, 0, sizep, hp) == 0 &&
              XAM(X360_xam_XamEnumerate, 6, x360_r32(X, hp), 0, buf, 0x100, countp, 0) == NO_MORE_FILES, "stats");
    CHECK(XAM(X360_xam_XamUserCreateTitlesPlayedEnumerator, 7, 0, 0, 0, 0, 2, sizep, hp) == 0 &&
              XAM(X360_xam_XamEnumerate, 6, x360_r32(X, hp), 0, buf, 0x100, countp, 0) == NO_MORE_FILES, "titles");
}

static void test_content(void)
{
    char dir[] = "/tmp/x360_xam2_XXXXXX", cmd[64];
    CHECK(mkdtemp(dir) != NULL, "temporary directory");
    x360_set_data_dir(X, dir);

    uint32_t root = gstr("save"), a = content_data("slot_a", 1), b = content_data("slot_b", 2);
    uint32_t disp = galloc(4), lic = galloc(4), ev, ov = ov_new(&ev);

    /* what the flags ask of content that does not exist */
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 3, disp, lic, 0) == 3 && x360_r32(X, disp) == 0, "open: missing");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 5, disp, lic, 0) == 3, "truncate: missing");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 9, disp, lic, 0) == 0x57, "unknown flags");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 1, disp, lic, ov) == IO_PENDING && x360_r32(X, ov) == 0 &&
              event_state(ev) == 1 && x360_r32(X, disp) == 1 && x360_r32(X, lic) == 0xFFFFFFFFu, "created");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 1, disp, lic, 0) == 0xB7, "create new: it exists");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 3, disp, lic, 0) == 0 && x360_r32(X, disp) == 2, "opened");
    CHECK(XAM(X360_xam_XamContentCreate, 7, 0, root, a, 4, disp, lic, 0) == 0 && x360_r32(X, disp) == 2, "open always");
    CHECK(XAM(X360_xam_XamContentCreateInternal, 8, gstr("dlc"), b, 4, disp, lic, 0, 0, 0) == 0 &&
              x360_r32(X, disp) == 1, "created (internal)");

    /* mounted: flush answers for a root, close unmounts it */
    CHECK(XAM(X360_xam_XamContentFlush, 2, root, 0) == 0, "flush");
    CHECK(XAM(X360_xam_XamContentFlush, 2, gstr("nope"), 0) == 0x57, "flush: not mounted");

    /* the creator of a saved game */
    uint32_t is = galloc(4), who = galloc(8), missing = content_data("slot_z", 1);
    CHECK(XAM(X360_xam_XamContentGetCreator, 5, 0, a, is, who, 0) == 0 && x360_r32(X, is) == 1 &&
              x360_r64(X, who) == XUID, "creator");
    CHECK(XAM(X360_xam_XamContentGetCreator, 5, 0, b, is, who, 0) == 0 && x360_r32(X, is) == 0, "not a saved game");
    CHECK(XAM(X360_xam_XamContentGetCreator, 5, 0, missing, is, who, 0) == 3, "creator: missing");

    /* the thumbnail: a size query, too small, then the bytes */
    uint32_t png = gstr("PNGDATA"), sizep = galloc(4), out = galloc(16);
    x360_w32(X, sizep, 0);
    CHECK(XAM(X360_xam_XamContentGetThumbnail, 5, 0, a, 0, sizep, 0) == 2, "no thumbnail yet");
    CHECK(XAM(X360_xam_XamContentSetThumbnail, 5, 0, a, png, 7, 0) == 0, "thumbnail set");
    CHECK(XAM(X360_xam_XamContentSetThumbnail, 5, 0, missing, png, 7, 0) == 2, "set: missing content");
    CHECK(XAM(X360_xam_XamContentGetThumbnail, 5, 0, a, 0, sizep, 0) == 0 && x360_r32(X, sizep) == 7, "size");
    x360_w32(X, sizep, 3);
    CHECK(XAM(X360_xam_XamContentGetThumbnail, 5, 0, a, out, sizep, 0) == INSUFFICIENT && x360_r32(X, sizep) == 7,
          "too small");
    uint32_t ev2, ov2 = ov_new(&ev2);
    CHECK(XAM(X360_xam_XamContentGetThumbnail, 5, 0, a, out, sizep, ov2) == IO_PENDING && x360_r32(X, ov2 + 4) == 7 &&
              !memcmp(X->base + out, "PNGDATA", 7), "the bytes");

    /* resolve */
    uint32_t path = galloc(128);
    CHECK(XAM(X360_xam_XamContentResolve, 7, 0, a, path, 128, 0, 0, 0) == 0 &&
              !strcmp((char *)X->base + path, "\\Device\\Harddisk0\\Partition1\\content\\slot_a"), "resolved");
    CHECK(XAM(X360_xam_XamContentResolve, 7, 0, a, path, 4, 0, 0, 0) == INSUFFICIENT, "resolve: too small");
    CHECK(XAM(X360_xam_XamContentResolve, 7, 0, missing, path, 128, 0, 0, 0) == 3, "resolve: missing");
    CHECK(XAM(X360_xam_XamContentOpenFile, 7, 0, root, path, 3, 0, 0, 0) == 2, "no package file");

    /* the enumerator: by type, one item per call, then the end */
    uint32_t hp = galloc(4), countp = galloc(4), buf = galloc(0x400);
    CHECK(XAM(X360_xam_XamContentCreateEnumerator, 7, 0, 1, 0, 0, 1, sizep, hp) == 0 && x360_r32(X, sizep) == 0x134,
          "every type");
    uint32_t h = x360_r32(X, hp);
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0x134, countp, 0) == 0 && x360_r32(X, countp) == 1 &&
              !strcmp((char *)X->base + buf + 0x108, "slot_a") && x360_r16(X, buf + 8) == 'S', "first");
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0x134, countp, 0) == 0 &&
              !strcmp((char *)X->base + buf + 0x108, "slot_b") && x360_r32(X, buf + 4) == 2, "second");
    CHECK(XAM(X360_xam_XamEnumerate, 6, h, 0, buf, 0x134, countp, 0) == NO_MORE_FILES && x360_r32(X, countp) == 0,
          "the end");
    CHECK(XAM(X360_xam_XamContentCreateEnumerator, 7, 0, 1, 2, 0, 4, sizep, hp) == 0, "one type");
    CHECK(XAM(X360_xam_XamEnumerate, 6, x360_r32(X, hp), 0, buf, 0x400, countp, 0) == 0 && x360_r32(X, countp) == 1 &&
              !strcmp((char *)X->base + buf + 0x108, "slot_b"), "only that type");
    CHECK(XAM(X360_xam_XamContentCreateEnumerator, 7, 0, 7, 0, 0, 1, sizep, hp) == 0x80070057u && x360_r32(X, sizep) == 0,
          "an unknown device");
    CHECK(XAM(X360_xam_XamContentCreateEnumerator, 7, 2, 1, 0, 0, 1, sizep, hp) == 0x525, "another user");
    CHECK(XAM(X360_xam_XamContentAggregateCreateEnumerator, 5, 0, 0, 1, 0, hp) == 0, "aggregate");
    X->title_id = 0x41560817;
    CHECK(XAM(X360_xam_XamEnumerate, 6, x360_r32(X, hp), 0, buf, 0x148, countp, 0) == 0 &&
              x360_r32(X, buf + 0x140) == 0x41560817, "aggregate: the title");

    CHECK(XAM(X360_xam_XamContentClose, 2, root, 0) == 0, "closed");
    CHECK(XAM(X360_xam_XamContentClose, 2, root, 0) == 2, "closed twice");
    CHECK(XAM(X360_xam_XamContentFlush, 2, root, 0) == 0x57, "flush after close");

    /* deleted for real; a completion routine is queued to the caller as a user APC: routine(result, length, overlapped) */
    uint32_t ev3, ov3 = ov_new(&ev3);
    x360_w32(X, ov3 + 0x10, 0x82000100u);
    CHECK(XAM(X360_xam_XamContentDelete, 3, 0, a, ov3) == IO_PENDING && x360_r32(X, ov3) == 0 && event_state(ev3) == 1,
          "deleted");
    uint32_t apc = x360_cur(X)->apc_head;
    CHECK(apc && x360_r32(X, apc + 0x18) == 0x82000100u && x360_r32(X, apc + 0x1C) == 0 &&
              x360_r32(X, apc + 0x24) == ov3, "the completion routine is queued with its arguments");
    CHECK(K(X360_xboxkrnl_KeRemoveQueueApc, 1, apc) == 1, "not delivered in this test");
    CHECK(XAM(X360_xam_XamContentDelete, 3, 0, a, 0) == 2, "deleted: it is gone");
    CHECK(XAM(X360_xam_XamContentDeleteInternal, 2, missing, 0) == 2, "delete: missing");

    uint32_t mask = galloc(4);
    CHECK(XAM(X360_xam_XamContentGetLicenseMask, 2, mask, 0) == 0 && x360_r32(X, mask) == 0xFFFFFFFFu, "license mask");
    CHECK(XAM(X360_xam_XamContentGetLicenseMask, 2, 0, 0) == 0x57, "license mask: no pointer");

    /* the device */
    uint32_t name = galloc(64);
    CHECK(XAM(X360_xam_XamContentGetDeviceName, 3, 1, name, 32) == 0 && wstr_is(name, "Hard Drive"), "device name");
    CHECK(XAM(X360_xam_XamContentGetDeviceName, 3, 1, name, 4) == INSUFFICIENT, "device name: too small");
    CHECK(XAM(X360_xam_XamContentGetDeviceName, 3, 2, name, 32) == 0x48F, "no such device");
    uint32_t data = galloc(0x50);
    CHECK(XAM(X360_xam_XamContentGetDeviceData, 2, 1, data) == 0 && x360_r64(X, data + 8) == 20ull << 30, "device data");
    CHECK(XAM(X360_xam_XamContentGetDeviceData, 2, 2, data) == 0x48F, "device data: no such device");

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0, "clean up");
}

static void test_info(void)
{
    uint32_t buf = galloc(256);
    /* 2021-03-04 05:06 UTC */
    uint64_t ft = (uint64_t)((int64_t)(18690 + 134774) * 86400 + 5 * 3600 + 6 * 60) * 10000000u;
    xe_cpu *c = cpu();
    c->gpr[1] = stack_top;
    c->gpr[3] = 0, c->gpr[4] = ft, c->gpr[5] = buf, c->gpr[6] = 32;
    x360_thunk date = {0, X360_xam_XamFormatDateString, X360_LIB_XAM}, tim = {0, X360_xam_XamFormatTimeString, X360_LIB_XAM};
    x360_hle_call(X, &date);
    CHECK(wstr_is(buf, "03/04/2021"), "date string");
    c->gpr[3] = 0, c->gpr[4] = ft, c->gpr[5] = buf, c->gpr[6] = 32;
    x360_hle_call(X, &tim);
    CHECK(wstr_is(buf, "05:06"), "time string");

    uint32_t res = gwstr("pic.png");
    XAM(X360_xam_XamBuildResourceLocator, 5, 0, gwstr("media"), res, buf, 64);
    CHECK(wstr_is(buf, "file://media:/media.xzp#pic.png"), "a local locator");
    XAM(X360_xam_XamBuildResourceLocator, 5, 0x82000000u, gwstr("skin"), res, buf, 64);
    CHECK(wstr_is(buf, "section://82000000,skin#pic.png"), "a section locator");
    XAM(X360_xam_XamBuildGamercardResourceLocator, 3, res, buf, 64);
    CHECK(wstr_is(buf, "file://media:/gamercrd.xzp#pic.png"), "gamercard");
    XAM(X360_xam_XamBuildLegacySystemResourceLocator, 3, res, buf, 64);
    CHECK(wstr_is(buf, "file://media:/shrdres.xzp#pic.png"), "legacy system");
    XAM(X360_xam_XamBuildXamResourceLocator, 3, res, buf, 8);
    CHECK(wstr_is(buf, "file://"), "truncated locator");

    uint32_t s = XAM(X360_xam_XamGetOnlineSchema, 0);
    CHECK(s && x360_r32(X, s) == s + 8 && x360_r32(X, s + 4) == 0x2C, "online schema");
    CHECK(XAM(X360_xam_XamGetOnlineSchema, 0) == s, "one schema");
    CHECK(XAM(X360_xam_XamQueryLiveHiveW, 4, 0, 0, 0, 0) == 0xC00000EFu, "no Live hive");
    CHECK(XAM(X360_xam_XamLoaderGetDvdTrayState, 0) == 2, "the tray is closed");

    /* locale */
    CHECK(XAM(X360_xam_XamGetLocale, 0) == 36 && XAM(X360_xam_XamGetLocaleEx, 2, 111, 43) == 36, "United States");
    CHECK(XAM(X360_xam_XamGetLanguage, 0) == 1, "English");
    CHECK(XAM(X360_xam_XamGetLanguageString, 3, 1, 8, buf) == 0 && wstr_is(buf, "en"), "language code");
    CHECK(XAM(X360_xam_XamGetLocaleString, 3, 36, 8, buf) == 0 && wstr_is(buf, "US"), "locale code");
    CHECK(XAM(X360_xam_XamGetCountryString, 3, 103, 8, buf) == 0 && wstr_is(buf, "US"), "country code");
    CHECK(XAM(X360_xam_XamGetOnlineLanguageString, 3, 3, 8, buf) == 0 && wstr_is(buf, "de"), "online language");
    CHECK(XAM(X360_xam_XamGetOnlineCountryString, 3, 111, 8, buf) == 0 && wstr_is(buf, "AF"), "online country");
    CHECK(XAM(X360_xam_XamGetLanguageLocaleString, 4, 1, 36, 8, buf) == 0 && wstr_is(buf, "en-US"), "language-locale");
    CHECK(XAM(X360_xam_XamGetOnlineLanguageAndCountryString, 4, 2, 53, 8, buf) == 0 && wstr_is(buf, "ja-JP"), "online");
    CHECK(XAM(X360_xam_XamGetLanguageString, 3, 10, 8, buf) == 0x80070490u, "a language without a code");
    CHECK(XAM(X360_xam_XamGetLanguageString, 3, 200, 8, buf) == 0x80070490u, "an unknown language");
    CHECK(XAM(X360_xam_XamGetLanguageString, 3, 1, 2, buf) == 0x8007007Au, "too small");
    CHECK(XAM(X360_xam_XamGetLanguageString, 3, 1, 0x80000000u, buf) == 0x80070057u, "a negative length");
    CHECK(XAM(X360_xam_XamGetOnlineCountryFromLocale, 1, 36) == 103, "locale to online country");
    CHECK(XAM(X360_xam_XamGetLocaleFromOnlineCountry, 1, 103) == 36, "online country to locale");
    CHECK(XAM(X360_xam_XamGetCountryFromOnlineCountry, 1, 103) == 103, "online country to country");
    CHECK(XAM(X360_xam_XamGetLanguageFromOnlineLanguage, 1, 4) == 4, "online language to language");
    CHECK(XAM(X360_xam_XamGetLanguageFromOnlineLanguage, 1, 60) == 0, "a language the console does not have");
}

static void test_services(void)
{
    uint32_t out = galloc(32);
    CHECK(XAM(X360_xam_XamAvatarInitialize, 6, 0, 0, 0, 0, 0, 0) == 0xFFFFFFFFu, "no avatars");
    CHECK(XAM(X360_xam_XamPartyGetUserList, 2, 4, out) == 0x807D0003u, "no party");
    CHECK(XAM(X360_xam_XamPartySendGameInvites, 3, 0, 0, 0) == FUNCTION_FAILED, "no party invites");
    CHECK(XAM(X360_xam_XamPartySetCustomData, 3, 0, 0, 0) == FUNCTION_FAILED, "no party data");
    CHECK(XAM(X360_xam_XamPartyGetBandwidth, 2, 0, 0) == FUNCTION_FAILED, "no party bandwidth");
    x360_w32(X, out, 5);
    CHECK(XAM(X360_xam_XamVoiceCreate, 3, 0, 0, out) == 5 && x360_r32(X, out) == 0, "no voice");
    CHECK(XAM(X360_xam_XamVoiceHeadsetPresent, 1, 0) == 0 && XAM(X360_xam_XamVoiceIsActiveProcess, 0) == 0, "no headset");
    memset(X->base + out, 0xFF, 24);
    XAM(X360_xam_XamNuiGetDeviceStatus, 1, out);
    CHECK(x360_r32(X, out + 12) == 0, "no Kinect");
    CHECK(XAM(X360_xam_XMsgSystemProcessCall, 4, 0, 0, 0, 0) == 0x490, "no system application");

    /* the network */
    CHECK(XAM(X360_xam_NetDll_recvfrom, 7, 0, 1, out, 4, 0, 0, 0) == 0xFFFFFFFFu, "recvfrom");
    CHECK(XAM(X360_xam_NetDll_sendto, 7, 0, 1, out, 4, 0, 0, 0) == 0xFFFFFFFFu, "sendto");
    CHECK(XAM(X360_xam_NetDll_WSAGetLastError, 0) == 0x2742, "the network is down");
    CHECK(XAM(X360_xam_NetDll_XNetGetOpt, 4, 1, 1, out, out) == 0x2726, "no option");
    CHECK(XAM(X360_xam_NetDll_XNetGetEthernetLinkStatus, 1, 0) == 0, "no link");
    CHECK(XAM(X360_xam_NetDll_XNetGetDebugXnAddr, 2, 0, out) == 1, "no address");
    x360_w32(X, out, 7);
    CHECK(XAM(X360_xam_NetDll_XNetDnsLookup, 4, 0, 0, 0, out) == 0x2742 && x360_r32(X, out) == 0, "no DNS");
    CHECK(XAM(X360_xam_NetDll_XNetQosServiceLookup, 4, 0, 0, 0, out) == 0x2742, "no QoS");
    CHECK(XAM(X360_xam_NetDll_XNetDnsRelease, 2, 0, 0) == 0xC000000Du, "DNS release: nothing");
    CHECK(XAM(X360_xam_NetDll_XNetQosListen, 6, 0, 0, 0, 0, 0, 0) == FUNCTION_FAILED, "no QoS listen");
    XAM(X360_xam_NetDll_XNetInAddrToString, 4, 0, 0x0A010203u, out, 16);
    CHECK(!strcmp((char *)X->base + out, "10.1.2.3"), "address string");
    memset(X->base + out, 0, 16);
    CHECK(XAM(X360_xam_NetDll_XNetRandom, 3, 0, out, 16) == 0, "random");
    int any = 0;
    for (int k = 0; k < 16; k++)
        any |= x360_r8(X, out + (uint32_t)k);
    CHECK(any, "random bytes");
    uint32_t ev = XAM(X360_xam_NetDll_WSACreateEvent, 0);
    CHECK(ev != 0 && event_state(ev) == 0, "event");
    CHECK(XAM(X360_xam_NetDll_WSASetEvent, 1, ev) == 1 && event_state(ev) == 1, "set");
    CHECK(XAM(X360_xam_NetDll_WSAResetEvent, 1, ev) == 1 && event_state(ev) == 0, "reset");
    CHECK(XAM(X360_xam_NetDll_WSACloseEvent, 1, ev) == 1 && XAM(X360_xam_NetDll_WSACloseEvent, 1, ev) == 0, "closed");
}

int main(void)
{
    if (hle_util_start())
        return 1;
    test_overlapped();
    test_notifications();
    test_ui();
    test_tasks();
    test_profile();
    test_user();
    test_enumerators();
    test_content();
    test_info();
    test_services();
    return hle_util_end("x360_xam2");
}
