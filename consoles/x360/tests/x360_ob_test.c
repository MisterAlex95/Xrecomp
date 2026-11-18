/* x360_ob_test -- objects, symbolic link objects, deleting files, and the honest refusals (XBA-296). */
#include <sys/stat.h>
#include <unistd.h>

#include "x360_hle_util.h"

/* ANSI_STRING over a guest buffer, OBJECT_ATTRIBUTES over a path */
static uint32_t ansi_of(const char *s)
{
    uint32_t a = galloc(8);
    x360_w16(X, a, (uint32_t)strlen(s));
    x360_w16(X, a + 2, (uint32_t)strlen(s) + 1);
    x360_w32(X, a + 4, gstr(s));
    return a;
}

static uint32_t attrs(const char *path)
{
    uint32_t oa = galloc(12);
    x360_w32(X, oa + 4, ansi_of(path));
    return oa;
}

static void test_objects(void)
{
    uint32_t out = galloc(8), hp = galloc(8);
    CHECK(K(X360_xboxkrnl_ObCreateObject, 4, 0, 0, 0x40, out) == 0 && x360_r32(X, out) != 0, "ObCreateObject");
    uint32_t o = x360_r32(X, out);
    CHECK(K(X360_xboxkrnl_ObInsertObject, 4, o, 0, 0, hp) == 0 && x360_handle_obj(X, x360_r32(X, hp)) == o,
          "ObInsertObject gives a handle on the object");
    CHECK(K(X360_xboxkrnl_ObOpenObjectByPointer, 3, o, 0, hp) == 0 && x360_handle_obj(X, x360_r32(X, hp)) == o,
          "ObOpenObjectByPointer");
    CHECK(K(X360_xboxkrnl_ObOpenObjectByPointer, 3, 0, 0, hp) == X_STATUS_INVALID_PARAMETER, "no object");
    CHECK(K(X360_xboxkrnl_ObOpenObjectByName, 4, attrs("\\Nothing"), 0, 0, hp) == 0xC0000034u, "no such name");
    CHECK(K(X360_xboxkrnl_ObLookupAnyThreadByThreadId, 2, x360_cur(X)->id, out) == 0 &&
              x360_r32(X, out) == x360_cur(X)->kthread, "the thread by id");
    CHECK(K(X360_xboxkrnl_ObLookupAnyThreadByThreadId, 2, 0x7777, out) == X_STATUS_INVALID_PARAMETER, "no such thread");
    CHECK(K(X360_xboxkrnl_ObGetWaitableObject, 1, o) == o, "the waitable object");
}

static void test_links(void)
{
    uint32_t hp = galloc(8), len = galloc(4), buf = galloc(64), out = galloc(8);
    x360_w16(X, out + 2, 64);
    x360_w32(X, out + 4, buf);
    CHECK(K(X360_xboxkrnl_NtCreateSymbolicLinkObject, 4, hp, 0, attrs("\\??\\saves:"), ansi_of("\\Device\\Harddisk0")) == 0,
          "NtCreateSymbolicLinkObject");
    uint32_t h = x360_r32(X, hp);
    CHECK(K(X360_xboxkrnl_NtQuerySymbolicLinkObject, 3, h, out, len) == 0 && x360_r32(X, len) == 17 &&
              x360_r16(X, out) == 17 && !memcmp(X->base + buf, "\\Device\\Harddisk0", 17), "the target comes back");
    CHECK(K(X360_xboxkrnl_NtOpenSymbolicLinkObject, 2, hp, attrs("SAVES:")) == 0, "opened by name, in any case");
    CHECK(K(X360_xboxkrnl_NtOpenSymbolicLinkObject, 2, hp, attrs("none:")) == 0xC0000034u, "no such link");
    uint32_t o = x360_handle_obj(X, x360_r32(X, hp));
    memset(X->base + buf, 0, 64);
    CHECK(K(X360_xboxkrnl_ObTranslateSymbolicLink, 2, o, out) == 0 && !memcmp(X->base + buf, "\\Device\\Harddisk0", 17),
          "ObTranslateSymbolicLink");
    x360_w16(X, out + 2, 4);
    CHECK(K(X360_xboxkrnl_NtQuerySymbolicLinkObject, 3, h, out, len) == 0xC0000023u && x360_r32(X, len) == 17,
          "too small a buffer: the length needed");
    CHECK(K(X360_xboxkrnl_NtQuerySymbolicLinkObject, 3, 0xF8FFFFFCu, out, len) == X_STATUS_INVALID_HANDLE, "bad handle");
}

static void test_delete(const char *dir)
{
    char path[256];
    snprintf(path, sizeof path, "%s/victim.txt", dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "the file to delete");
    if (f)
        fclose(f);
    x360_set_data_dir(X, dir);
    CHECK(K(X360_xboxkrnl_NtDeleteFile, 1, attrs("cache:\\victim.txt")) == 0, "deleted");
    CHECK(access(path, F_OK) != 0, "it is gone");
    CHECK(K(X360_xboxkrnl_NtDeleteFile, 1, attrs("cache:\\victim.txt")) == 0xC0000034u, "already gone");
    CHECK(K(X360_xboxkrnl_NtDeleteFile, 1, attrs("game:\\default.xex")) == 0xC0000022u, "the disc is read-only");
}

static void test_refusals(void)
{
    CHECK(K(X360_xboxkrnl_UsbdOpenEndpoint, 0) == 0xC000009Du, "USB: no device");
    CHECK(K(X360_xboxkrnl_UsbdIsDeviceAuthenticated, 0) == 0, "a BOOL refuses with FALSE, not a status that reads TRUE");
    CHECK(K(X360_xboxkrnl_MmMapIoSpace, 0) == 0, "a pointer refuses with NULL");
    CHECK(K(X360_xboxkrnl_NicAttach, 0) == 0xC000009Du, "no network adapter");
    CHECK(K(X360_xboxkrnl_WifiBeginAuthentication, 0) == 0xC000009Du, "no Wi-Fi");
    CHECK(K(X360_xboxkrnl_UsbdGetNatalHub, 0) == 0, "no Kinect hub");
    CHECK(K(X360_xboxkrnl_XInputdReadState, 0) == 0x48Fu, "the controller driver: not connected");
    CHECK(K(X360_xboxkrnl_IptvSetSessionKey, 0) == 0xC00000BBu, "no IPTV");
    uint32_t buf = galloc(32);
    memset(X->base + buf, 0x5A, 32);
    CHECK(K(X360_xboxkrnl_XeKeysGetConsoleID, 2, buf, buf) == 0xC00000BBu, "no console id");
    CHECK(X->base[buf] == 0x5A && X->base[buf + 31] == 0x5A, "and nothing written");
    CHECK(K(X360_xboxkrnl_XeKeysQwNeRsaPrvCrypt, 3, buf, buf, buf) == 0, "no private key");
    CHECK(K(X360_xboxkrnl_NtDeviceIoControlFile, 0) == 0xC0000010u, "no device controls");
    CHECK(K(X360_xboxkrnl_NtSetSystemTime, 0) == 0xC0000061u, "the clock is not the title's");
    CHECK(K(X360_xboxkrnl_HalRegisterPowerDownNotification, 0) == 0, "a registration is accepted");

    /* a refusal is not silent: counted once, and strict imports stop on it */
    x360_stats st;
    x360_get_stats(X, &st);
    uint32_t before = st.unimplemented;
    K(X360_xboxkrnl_NicDetach, 0);
    K(X360_xboxkrnl_NicDetach, 0);
    x360_get_stats(X, &st);
    CHECK(st.unimplemented == before + 1, "counted once: %u after %u", st.unimplemented, before);
    X->strict_imports = 1;
    K(X360_xboxkrnl_NicShutdown, 0);
    CHECK(X->status == X360_ERROR && strstr(X->error, "NicShutdown"), "strict imports stop on a refusal: %s", X->error);
    X->status = X360_RUNNING;
    X->strict_imports = 0;
}

int main(void)
{
    if (hle_util_start())
        return 1;
    char dir[] = "/tmp/x360_ob_XXXXXX";
    test_objects();
    test_links();
    if (mkdtemp(dir)) {
        test_delete(dir);
        rmdir(dir);
    } else {
        CHECK(0, "mkdtemp");
    }
    test_refusals();
    return hle_util_end("x360_ob");
}
