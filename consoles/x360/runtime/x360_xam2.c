/* x360_xam2.c -- XAM beyond x360_xam.c and x360_more.c: content, users, notifications, UI, tasks (XBA-296). Names,
 * signatures and results follow Xenia's public xam_*.cc (BSD); no code of it is here.
 *
 * The console answers as x360_xam.c does: user 0 signed in locally and offline, English, United States, one hard
 * drive. A service a title cannot have here (Live, party, voice, Kinect, the network) answers its error.
 *
 * Everything a title can observe lives in guest memory, reached through the handle table (both are in a save state):
 * listeners, enumerators and one root block are tagged kernel objects found by walking x->handles.
 *
 * XAM_OVERLAPPED: a call completes at once, its event is signalled and it returns X_ERROR_IO_PENDING. A completion
 * routine runs as a user APC on the calling thread, at its next alertable wait: routine(result, length, overlapped).
 *
 * Content: content/<name> of the data directory is the package; content/.meta/<name>.dat keeps its XCONTENT_DATA,
 * content/.meta/<name>.png its thumbnail. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define A9(n) ((uint32_t)x360_stack_arg(x, c, (n)))
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))

#define X_ERROR_SUCCESS 0u
#define X_ERROR_FILE_NOT_FOUND 2u
#define X_ERROR_PATH_NOT_FOUND 3u
#define X_ERROR_ACCESS_DENIED 5u
#define X_ERROR_INVALID_HANDLE 6u
#define X_ERROR_NO_MORE_FILES 0x12u
#define X_ERROR_INVALID_PARAMETER 0x57u
#define X_ERROR_INSUFFICIENT_BUFFER 0x7Au
#define X_ERROR_ALREADY_EXISTS 0xB7u
#define X_ERROR_IO_INCOMPLETE 0x3E4u
#define X_ERROR_IO_PENDING 0x3E5u
#define X_ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define X_ERROR_NOT_FOUND 0x490u
#define X_ERROR_NOT_LOGGED_ON 0x524u
#define X_ERROR_NO_SUCH_USER 0x525u
#define X_ERROR_FUNCTION_FAILED 0x65Bu
#define X_E_FAIL 0x80004005u
#define X_E_INVALIDARG 0x80070057u
#define X_E_NOTFOUND 0x80070490u
#define X_E_NO_SUCH_USER 0x80070525u
#define X_E_DEVICE_NOT_CONNECTED 0x8007048Fu
#define X_E_INSUFFICIENT_BUFFER 0x8007007Au
#define X_STATUS_INVALID_PARAMETER_1 0xC00000EFu
#define WSAEINVAL 0x2726u
#define WSAENETDOWN 0x2742u

#define USER_XUID 0xB13EBABEBABEBABEull
#define CONTENT_DIR "\\Device\\Harddisk0\\Partition1\\content"
#define XCONTENT_SIZE 0x134u
#define XCONTENT_AGGREGATE_SIZE 0x148u
#define DEVICE_DATA_SIZE 0x50u
#define HDD_ID 1u
#define XN_SYS_UI 0x00000009u
#define XN_SYS_SIGNINCHANGED 0x0000000Au
#define XN_SYS_INPUTDEVICESCHANGED 0x00000012u
#define XN_SYS_INPUTDEVICECONFIGCHANGED 0x00000013u

/* ---- overlapped ---- */

/* the event signalled and IO_PENDING answered with an overlapped; sync_result without one */
static uint32_t finish_ex(x360_machine *x, x360_thread *t, uint32_t ov, uint32_t sync_result, uint32_t result,
                          uint32_t extended, uint32_t length)
{
    if (!ov)
        return sync_result;
    x360_w32(x, ov, result);
    x360_w32(x, ov + 4, length);
    x360_w32(x, ov + 8, t->handle);
    x360_w32(x, ov + 0x18, extended);
    uint32_t ev = x360_r32(x, ov + 0xC);
    uint32_t o = ev ? x360_handle_obj(x, ev) : 0;
    if (o)
        x360_w32(x, o + 4, 1);
    uint32_t routine = x360_r32(x, ov + 0x10);
    if (routine)
        x360_apc_queue_user(x, t->kthread, routine, result, length, ov);
    return X_ERROR_IO_PENDING;
}

/* the plain completion: the result is its own extended error, a failure's length is -1 */
static uint32_t finish(x360_machine *x, x360_thread *t, uint32_t ov, uint32_t result, uint32_t length)
{
    return finish_ex(x, t, ov, result, result, result, result ? 0xFFFFFFFFu : length);
}

static uint32_t hresult_of(uint32_t win32)
{
    return win32 ? 0x80070000u | (win32 & 0xFFFF) : 0;
}

static void x_XamGetOverlappedResult(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t ov = A(0);
    if (!ov) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    uint32_t result = x360_r32(x, ov);
    if (result == X_ERROR_IO_PENDING) { /* nothing here stays pending: the title's own structure, never started */
        RET(X_ERROR_IO_INCOMPLETE);
        return;
    }
    if (A(1))
        x360_w32(x, A(1), x360_r32(x, ov + 4));
    RET(result);
}

static void x_XGetOverlappedExtendedError(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t ov = A(0);
    if (!ov)
        RET(X_ERROR_INVALID_PARAMETER);
    else if (x360_r32(x, ov) == X_ERROR_IO_PENDING)
        RET(X_ERROR_IO_INCOMPLETE);
    else
        RET(x360_r32(x, ov + 0x18));
}

static void x_XMsgCompleteIORequest(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (overlapped, result, extended error, length) */
    uint32_t ov = A(0);
    if (ov) {
        x360_w32(x, ov, A(1));
        x360_w32(x, ov + 4, A(3));
        x360_w32(x, ov + 0x18, A(2));
        uint32_t ev = x360_r32(x, ov + 0xC), o = ev ? x360_handle_obj(x, ev) : 0;
        if (o)
            x360_w32(x, o + 4, 1);
    }
    RET(X_ERROR_SUCCESS);
}

/* ---- tagged kernel objects ---- */

#define TAG_ROOT 0x58414D52u
#define TAG_LISTENER 0x58414D4Cu
#define TAG_ENUM 0x58414D45u
#define OBJ_TAG 16u
#define OBJ_SELF 20u

static uint32_t tagged_new(x360_machine *x, uint32_t tag, uint32_t size, uint32_t *handle)
{
    uint32_t o = x360_kalloc(x, size), h = o ? x360_handle_new(x, o) : 0;
    if (!h)
        return 0;
    x360_w8(x, o, X_OBJ_EVENT_NOTIFICATION);
    x360_w32(x, o + OBJ_TAG, tag);
    x360_w32(x, o + OBJ_SELF, o);
    if (handle)
        *handle = h;
    return o;
}

static int tagged_is(x360_machine *x, uint32_t o, uint32_t tag)
{
    x360_heap *h = &x->heap_kernel;
    if (o < h->base || o - h->base >= h->size || !x360_heap_all_used(h, o, OBJ_SELF + 4))
        return 0;
    return x360_r32(x, o + OBJ_TAG) == tag && x360_r32(x, o + OBJ_SELF) == o;
}

static uint32_t tagged_of(x360_machine *x, uint32_t handle, uint32_t tag)
{
    uint32_t o = x360_handle_obj(x, handle);
    return o && tagged_is(x, o, tag) ? o : 0;
}

/* the root block: start-up notifications sent, the online schema, the settings the title wrote */
#define ROOT_STARTUP 24u
#define ROOT_SCHEMA 28u
#define ROOT_NSET 32u
#define ROOT_SET 40u
#define ROOT_MAXSET 48u
#define ROOT_SIZE (ROOT_SET + ROOT_MAXSET * 16u)

static uint32_t root_of(x360_machine *x)
{
    for (int k = 1; k < X360_MAX_HANDLES; k++)
        if (x->handles[k].used && tagged_is(x, x->handles[k].obj, TAG_ROOT))
            return x->handles[k].obj;
    return tagged_new(x, TAG_ROOT, ROOT_SIZE, NULL);
}

/* ---- notifications ---- */

#define LIS_MASK 24u
#define LIS_VERSION 32u
#define LIS_COUNT 36u
#define LIS_QUEUE 40u
#define LIS_MAX 64u
#define LIS_SIZE (LIS_QUEUE + LIS_MAX * 8u)

static void listener_post(x360_machine *x, uint32_t o, uint32_t id, uint32_t data)
{
    uint64_t mask = x360_r64(x, o + LIS_MASK);
    uint32_t n = x360_r32(x, o + LIS_COUNT);
    if (!(mask >> ((id >> 25) & 0x3F) & 1) || ((id >> 16) & 0x1FF) > x360_r32(x, o + LIS_VERSION) || n >= LIS_MAX)
        return;
    x360_w32(x, o + LIS_QUEUE + 8 * n, id);
    x360_w32(x, o + LIS_QUEUE + 8 * n + 4, data);
    x360_w32(x, o + LIS_COUNT, n + 1);
    x360_w32(x, o + 4, 1);
}

static void broadcast(x360_machine *x, uint32_t id, uint32_t data)
{
    for (int k = 1; k < X360_MAX_HANDLES; k++)
        if (x->handles[k].used && tagged_is(x, x->handles[k].obj, TAG_LISTENER))
            listener_post(x, x->handles[k].obj, id, data);
}

static uint32_t listener_create(x360_machine *x, uint64_t mask, uint32_t version)
{
    uint32_t h = 0, o = tagged_new(x, TAG_LISTENER, LIS_SIZE, &h);
    if (!o)
        return 0;
    x360_w64(x, o + LIS_MASK, mask);
    x360_w32(x, o + LIS_VERSION, version > 10 ? 10 : version);
    uint32_t root = root_of(x);
    if (root && (mask & 1) && !x360_r32(x, root + ROOT_STARTUP)) { /* what a title's first system listener is told */
        static const uint32_t burst[][2] = {{XN_SYS_UI, 1},
                                            {XN_SYS_UI, 0},
                                            {XN_SYS_SIGNINCHANGED, 1},
                                            {XN_SYS_SIGNINCHANGED, 1},
                                            {XN_SYS_INPUTDEVICESCHANGED, 0},
                                            {XN_SYS_INPUTDEVICESCHANGED, 0},
                                            {XN_SYS_INPUTDEVICECONFIGCHANGED, 0},
                                            {XN_SYS_INPUTDEVICECONFIGCHANGED, 0}};
        x360_w32(x, root + ROOT_STARTUP, 1);
        for (size_t k = 0; k < sizeof burst / sizeof burst[0]; k++)
            listener_post(x, o, burst[k][0], burst[k][1]);
    }
    return h;
}

void x360_x_XamNotifyCreateListener(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(listener_create(x, c->gpr[3], A(1)));
}

static void x_XamNotifyCreateListenerInternal(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (mask, is system, max version) */
    RET(listener_create(x, c->gpr[3], A(2)));
}

void x360_x_XNotifyGetNext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (listener, id to match or 0, id*, parameter*): 1 when one was taken */
    uint32_t match = A(1), idp = A(2), parp = A(3);
    if (idp)
        x360_w32(x, idp, 0);
    if (parp)
        x360_w32(x, parp, 0);
    uint32_t o = tagged_of(x, A(0), TAG_LISTENER);
    if (!o || !idp) {
        RET(0);
        return;
    }
    uint32_t n = x360_r32(x, o + LIS_COUNT), at = 0;
    while (match && at < n && x360_r32(x, o + LIS_QUEUE + 8 * at) != match)
        at++;
    if (at >= n) {
        RET(0);
        return;
    }
    x360_w32(x, idp, x360_r32(x, o + LIS_QUEUE + 8 * at));
    if (parp)
        x360_w32(x, parp, x360_r32(x, o + LIS_QUEUE + 8 * at + 4));
    for (uint32_t k = at + 1; k < n; k++)
        x360_w64(x, o + LIS_QUEUE + 8 * (k - 1), x360_r64(x, o + LIS_QUEUE + 8 * k));
    x360_w32(x, o + LIS_COUNT, n - 1);
    if (n == 1)
        x360_w32(x, o + 4, 0);
    RET(1);
}

static void x_XNotifyBroadcast(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    broadcast(x, A(0), A(1));
    RET(X_ERROR_SUCCESS);
}

void x360_x_XamShowSigninUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    broadcast(x, XN_SYS_SIGNINCHANGED, 1);
    broadcast(x, XN_SYS_UI, 0);
    RET(X_ERROR_SUCCESS);
}

/* ---- guest strings ---- */

static void read_a(x360_machine *x, uint32_t a, char *out, uint32_t n)
{
    uint32_t k = 0;
    for (; a && k + 1 < n; k++)
        if (!(out[k] = (char)x360_r8(x, a + k)))
            break;
    out[k] = 0;
}

static void read_w(x360_machine *x, uint32_t a, char *out, uint32_t n)
{
    uint32_t k = 0;
    for (; a && k + 1 < n; k++) {
        uint16_t ch = x360_r16(x, a + 2 * k);
        if (!ch)
            break;
        out[k] = ch < 128 ? (char)ch : '?';
    }
    out[k] = 0;
}

/* UTF-16 into cap characters, always terminated when cap > 0 */
static void put_w(x360_machine *x, uint32_t a, uint32_t cap, const char *s)
{
    uint32_t k = 0;
    if (!a || !cap)
        return;
    for (; s[k] && k + 1 < cap; k++)
        x360_w16(x, a + 2 * k, (uint8_t)s[k]);
    x360_w16(x, a + 2 * k, 0);
}

/* ---- the file layer, from inside an HLE call ---- */

/* a scratch page: the frame of the nested call, then the kernel's structures */
#define T_HANDLE 0x100u
#define T_IOSB 0x108u
#define T_ANSI 0x110u
#define T_ATTRS 0x118u
#define T_INFO 0x130u
#define T_PATH 0x200u
#define T_DIR 0x400u
#define T_META 0x800u
#define T_SIZE 0x1000u

static uint32_t nt(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, x360_hle_fn fn, int n,
                   const uint32_t *args)
{
    uint64_t save[9];
    save[0] = c->gpr[1];
    for (int k = 0; k < 8; k++)
        save[k + 1] = c->gpr[3 + k];
    c->gpr[1] = tmp;
    for (int k = 0; k < n && k < 8; k++)
        c->gpr[3 + k] = args[k];
    if (n > 8)
        x360_w64(x, tmp + 0x50, args[8]);
    fn(x, t, c);
    uint32_t r = (uint32_t)c->gpr[3];
    c->gpr[1] = save[0];
    for (int k = 0; k < 8; k++)
        c->gpr[3 + k] = save[k + 1];
    return r;
}

static void set_path(x360_machine *x, uint32_t tmp, const char *path)
{
    uint32_t n = (uint32_t)strlen(path);
    if (n > T_DIR - T_PATH - 1)
        n = T_DIR - T_PATH - 1;
    memcpy(x->base + tmp + T_PATH, path, n);
    x360_w8(x, tmp + T_PATH + n, 0);
    x360_w16(x, tmp + T_ANSI, n);
    x360_w16(x, tmp + T_ANSI + 2, n + 1);
    x360_w32(x, tmp + T_ANSI + 4, tmp + T_PATH);
    x360_w32(x, tmp + T_ATTRS, 0);
    x360_w32(x, tmp + T_ATTRS + 4, tmp + T_ANSI);
    x360_w32(x, tmp + T_ATTRS + 8, 0);
}

/* 0: nothing there, 1: a file, 2: a directory */
static int path_kind(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, const char *path)
{
    set_path(x, tmp, path);
    uint32_t args[2] = {tmp + T_ATTRS, tmp + T_INFO};
    if (nt(x, t, c, tmp, x360_k_NtQueryFullAttributesFile, 2, args))
        return 0;
    return x360_r32(x, tmp + T_INFO + 48) & 0x10 ? 2 : 1;
}

static uint32_t path_open(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, const char *path, uint32_t access,
                          uint32_t disposition, uint32_t options, uint32_t *handle)
{
    set_path(x, tmp, path);
    uint32_t args[9] = {tmp + T_HANDLE, access, tmp + T_ATTRS, tmp + T_IOSB, 0, 0, 0, disposition, options};
    uint32_t st = nt(x, t, c, tmp, x360_k_NtCreateFile, 9, args);
    if (!st)
        *handle = x360_r32(x, tmp + T_HANDLE);
    return st;
}

static void file_close(x360_machine *x, uint32_t handle)
{
    x360_file_close(x, handle);
    x360_handle_close(x, handle);
}

/* the file's size, or -1 */
static int64_t file_size(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, uint32_t handle)
{
    uint32_t args[5] = {handle, tmp + T_IOSB, tmp + T_INFO, 24, 5};
    if (nt(x, t, c, tmp, x360_k_NtQueryInformationFile, 5, args))
        return -1;
    return (int64_t)x360_r64(x, tmp + T_INFO + 8);
}

static uint32_t file_read(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, uint32_t handle, uint32_t buf,
                          uint32_t len)
{
    uint32_t args[8] = {handle, 0, 0, 0, tmp + T_IOSB, buf, len, 0};
    if (!len || nt(x, t, c, tmp, x360_k_NtReadFile, 8, args))
        return 0;
    return x360_r32(x, tmp + T_IOSB + 4);
}

/* write a whole file under content/.meta, the directory made when it is missing */
static uint32_t meta_write(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, const char *name, const char *ext,
                           uint32_t buf, uint32_t len)
{
    char path[128];
    uint32_t h;
    if (path_open(x, t, c, tmp, CONTENT_DIR "\\.meta", 0x80000000u, 3 /* open if */, 1 /* directory */, &h))
        return X_ERROR_FUNCTION_FAILED;
    file_close(x, h);
    snprintf(path, sizeof path, CONTENT_DIR "\\.meta\\%s.%s", name, ext);
    if (path_open(x, t, c, tmp, path, 0xC0000000u, 5 /* overwrite if */, 0x40, &h))
        return X_ERROR_FUNCTION_FAILED;
    uint32_t args[8] = {h, 0, 0, 0, tmp + T_IOSB, buf, len, 0};
    uint32_t st = len ? nt(x, t, c, tmp, x360_k_NtWriteFile, 8, args) : 0;
    file_close(x, h);
    return st ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS;
}

/* ---- content ---- */

/* the directory name of an XCONTENT_DATA, as XamContentCreateEx (x360_more.c) makes it */
static void content_name(x360_machine *x, uint32_t cd, char *name, uint32_t n)
{
    uint32_t o = 0;
    for (uint32_t k = 0; cd && k < 42 && o + 1 < n; k++) {
        char ch = (char)x360_r8(x, cd + 0x108 + k);
        if (!ch)
            break;
        name[o++] = isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.' ? ch : '_';
    }
    name[o] = 0;
}

static int content_exists(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, const char *name)
{
    char path[128];
    snprintf(path, sizeof path, CONTENT_DIR "\\%s", name);
    return name[0] && path_kind(x, t, c, tmp, path) == 2;
}

static int root_mounted(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t tmp, const char *root)
{
    char path[80];
    snprintf(path, sizeof path, "%s:\\", root);
    return root[0] && path_kind(x, t, c, tmp, path) == 2;
}

static uint32_t tmp_new(x360_machine *x)
{
    return x360_heap_alloc(x, &x->heap_v4k, T_SIZE, 0);
}

static void tmp_free(x360_machine *x, uint32_t tmp)
{
    x360_heap_free(x, &x->heap_v4k, tmp);
}

static uint32_t content_create(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t root_a, uint32_t cd, uint32_t flags,
                               uint32_t disp, uint32_t lic)
{
    char root[64], name[48];
    uint32_t mode = flags & 0xF, tmp = tmp_new(x);
    if (disp)
        x360_w32(x, disp, 0);
    if (!tmp)
        return X_ERROR_FUNCTION_FAILED;
    read_a(x, root_a, root, sizeof root);
    content_name(x, cd, name, sizeof name);
    uint32_t result = X_ERROR_SUCCESS;
    int exists = content_exists(x, t, c, tmp, name), created = 0;
    if (mode < 1 || mode > 5 || !cd)
        result = X_ERROR_INVALID_PARAMETER;
    else if (mode == 1 && exists)
        result = X_ERROR_ALREADY_EXISTS;
    else if ((mode == 3 || mode == 5) && !exists)
        result = X_ERROR_PATH_NOT_FOUND;
    else if (x360_set_content_root(x, root, name) < 0)
        result = X_ERROR_FUNCTION_FAILED;
    else {
        created = !exists || mode == 2 || mode == 5;
        if (exists && created && (x360_content_remove(x, name) || x360_set_content_root(x, root, name) < 0))
            result = X_ERROR_FUNCTION_FAILED; /* CREATE_ALWAYS / TRUNCATE_EXISTING start empty */
        if (!exists || created)
            meta_write(x, t, c, tmp, name, "dat", cd, XCONTENT_SIZE);
        if (disp)
            x360_w32(x, disp, created ? 1 : 2);
        if (lic)
            x360_w32(x, lic, 0xFFFFFFFFu);
    }
    tmp_free(x, tmp);
    return result;
}

static void x_XamContentCreate(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, root name, XCONTENT_DATA*, flags, disposition*, license mask*, overlapped) */
    uint32_t ov = A(6);
    RET(finish(x, t, ov, content_create(x, t, c, A(1), A(2), A(3), A(4), A(5)), 0));
}

static void x_XamContentCreateEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, root name, XCONTENT_DATA*, flags, disposition*, license mask*, cache size, content size, overlapped) */
    uint32_t ov = A9(8);
    RET(finish(x, t, ov, content_create(x, t, c, A(1), A(2), A(3), A(4), A(5)), 0));
}

static void x_XamContentCreateInternal(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (root name, XCONTENT_AGGREGATE_DATA*, flags, disposition*, license mask*, cache size, content size, overlapped) */
    uint32_t ov = A(7);
    RET(finish(x, t, ov, content_create(x, t, c, A(0), A(1), A(2), A(3), A(4)), 0));
}

static void x_XamContentFlush(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    char root[64];
    uint32_t tmp = tmp_new(x), ov = A(1), result = X_ERROR_FUNCTION_FAILED;
    read_a(x, A(0), root, sizeof root);
    if (tmp) {
        result = root_mounted(x, t, c, tmp, root) ? X_ERROR_SUCCESS : X_ERROR_INVALID_PARAMETER;
        tmp_free(x, tmp);
    }
    RET(finish(x, t, ov, result, 0));
}

static void x_XamContentClose(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    char root[64], link[72];
    uint32_t tmp = tmp_new(x), ov = A(1), result = X_ERROR_FUNCTION_FAILED;
    read_a(x, A(0), root, sizeof root);
    if (tmp) {
        result = X_ERROR_FILE_NOT_FOUND;
        if (root_mounted(x, t, c, tmp, root)) {
            snprintf(link, sizeof link, "%s:", root);
            set_path(x, tmp, link);
            uint32_t args[1] = {tmp + T_ANSI};
            nt(x, t, c, tmp, x360_k_ObDeleteSymbolicLink, 1, args);
            result = X_ERROR_SUCCESS;
        }
        tmp_free(x, tmp);
    }
    RET(finish(x, t, ov, result, 0));
}

static uint32_t content_delete(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t cd)
{
    char name[48];
    uint32_t tmp = tmp_new(x);
    if (!tmp)
        return X_ERROR_FUNCTION_FAILED;
    content_name(x, cd, name, sizeof name);
    int exists = content_exists(x, t, c, tmp, name);
    tmp_free(x, tmp);
    if (!exists)
        return X_ERROR_FILE_NOT_FOUND;
    return x360_content_remove(x, name) ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS;
}

static void x_XamContentDelete(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t ov = A(2);
    RET(finish(x, t, ov, content_delete(x, t, c, A(1)), 0));
}

static void x_XamContentDeleteInternal(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    uint32_t ov = A(1);
    RET(finish(x, t, ov, content_delete(x, t, c, A(0)), 0));
}

static void x_XamContentGetCreator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, XCONTENT_DATA*, is creator*, XUID*, overlapped): saved games are the signed-in user's */
    char name[48];
    uint32_t cd = A(1), isp = A(2), xp = A(3), ov = A(4), tmp = tmp_new(x), result = X_ERROR_FUNCTION_FAILED;
    if (tmp) {
        content_name(x, cd, name, sizeof name);
        result = content_exists(x, t, c, tmp, name) ? X_ERROR_SUCCESS : X_ERROR_PATH_NOT_FOUND;
        tmp_free(x, tmp);
    }
    if (result == X_ERROR_SUCCESS) {
        int mine = x360_r32(x, cd + 4) == 1;
        if (isp)
            x360_w32(x, isp, (uint32_t)mine);
        if (xp)
            x360_w64(x, xp, mine ? USER_XUID : 0);
    }
    RET(finish(x, t, ov, result, 0));
}

static void x_XamContentGetLicenseMask(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (mask*, overlapped): the title is owned, as XamContentCreateEx answers */
    if (!A(0)) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, A(0), 0xFFFFFFFFu);
    RET(finish(x, t, A(1), X_ERROR_SUCCESS, 0));
}

static void x_XamContentGetThumbnail(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, XCONTENT_DATA*, buffer, size*, overlapped): the size is always written */
    char name[48], path[128];
    uint32_t buf = A(2), sizep = A(3), ov = A(4), tmp, h, result = X_ERROR_FILE_NOT_FOUND;
    if (!sizep) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    if (!(tmp = tmp_new(x))) {
        RET(finish(x, t, ov, X_ERROR_FUNCTION_FAILED, 0));
        return;
    }
    content_name(x, A(1), name, sizeof name);
    snprintf(path, sizeof path, CONTENT_DIR "\\.meta\\%s.png", name);
    uint32_t cap = x360_r32(x, sizep), got = 0;
    if (name[0] && !path_open(x, t, c, tmp, path, 0x80000000u, 1, 0x40, &h)) {
        int64_t size = file_size(x, t, c, tmp, h);
        if (size >= 0) {
            x360_w32(x, sizep, (uint32_t)size);
            result = X_ERROR_SUCCESS;
            if (buf && cap >= (uint64_t)size)
                got = file_read(x, t, c, tmp, h, buf, (uint32_t)size);
            else if (buf)
                result = X_ERROR_INSUFFICIENT_BUFFER;
        }
        file_close(x, h);
    }
    tmp_free(x, tmp);
    RET(finish(x, t, ov, result, got));
}

static void x_XamContentSetThumbnail(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, XCONTENT_DATA*, buffer, size, overlapped) */
    char name[48];
    uint32_t buf = A(2), size = A(3), ov = A(4), tmp = tmp_new(x), result = X_ERROR_FUNCTION_FAILED;
    if (tmp) {
        content_name(x, A(1), name, sizeof name);
        result = !content_exists(x, t, c, tmp, name) ? X_ERROR_FILE_NOT_FOUND
                                                     : meta_write(x, t, c, tmp, name, "png", buf, size);
        tmp_free(x, tmp);
    }
    RET(finish(x, t, ov, result, 0));
}

static uint32_t content_resolve(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t cd, uint32_t out, uint32_t cap)
{
    char name[48], path[128];
    uint32_t tmp = tmp_new(x), result = X_ERROR_FUNCTION_FAILED;
    if (!tmp)
        return result;
    content_name(x, cd, name, sizeof name);
    snprintf(path, sizeof path, CONTENT_DIR "\\%s", name);
    if (!content_exists(x, t, c, tmp, name))
        result = X_ERROR_PATH_NOT_FOUND;
    else if (out && cap <= strlen(path))
        result = X_ERROR_INSUFFICIENT_BUFFER;
    else {
        if (out)
            memcpy(x->base + out, path, strlen(path) + 1);
        result = X_ERROR_SUCCESS;
    }
    tmp_free(x, tmp);
    return result;
}

static void x_XamContentResolve(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, XCONTENT_DATA*, path*, path size, create directory, root name, overlapped) */
    uint32_t ov = A(6);
    RET(finish(x, t, ov, content_resolve(x, t, c, A(1), A(2), A(3)), 0));
}

static void x_XamContentResolveInternal(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (XCONTENT_DATA*, path*, path size, create directory, root name, overlapped) */
    uint32_t ov = A(5);
    RET(finish(x, t, ov, content_resolve(x, t, c, A(0), A(1), A(2)), 0));
}

static void x_XamContentOpenFile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, root name, path, flags, disposition*, license mask*, overlapped): no package files here */
    RET(finish(x, t, A(6), X_ERROR_FILE_NOT_FOUND, 0));
}

/* ---- devices: the one hard drive ---- */

static void device_put(x360_machine *x, uint32_t p)
{
    memset(x->base + p, 0, DEVICE_DATA_SIZE);
    x360_w32(x, p, HDD_ID);
    x360_w32(x, p + 4, 1);
    x360_w64(x, p + 8, 20ull << 30);
    x360_w64(x, p + 16, 16ull << 30);
    put_w(x, p + 24, 28, "Hard Drive");
}

static void x_XamContentGetDeviceData(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0) != HDD_ID) {
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    device_put(x, A(1));
    RET(X_ERROR_SUCCESS);
}

static void x_XamContentGetDeviceState(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    if (A(0) == HDD_ID)
        RET(finish(x, t, A(1), X_ERROR_SUCCESS, 0));
    else
        RET(finish_ex(x, t, A(1), X_ERROR_DEVICE_NOT_CONNECTED, X_ERROR_FUNCTION_FAILED, X_ERROR_DEVICE_NOT_CONNECTED,
                      0));
}

static void x_XamContentGetDeviceName(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (device, name*, capacity in characters) */
    static const char name[] = "Hard Drive";
    if (A(0) != HDD_ID)
        RET(X_ERROR_DEVICE_NOT_CONNECTED);
    else if (A(2) < sizeof name)
        RET(X_ERROR_INSUFFICIENT_BUFFER);
    else {
        put_w(x, A(1), A(2), name);
        RET(X_ERROR_SUCCESS);
    }
}

/* ---- enumerators ---- */

enum { EN_EMPTY = 0, EN_CONTENT, EN_DEVICES };

#define EN_KIND 24u
#define EN_POS 28u
#define EN_ITEM 32u
#define EN_TYPE 36u
#define EN_SIZE 48u

static uint32_t enum_new(x360_machine *x, uint32_t kind, uint32_t item, uint32_t type, uint32_t handle_ptr)
{
    uint32_t h = 0, o = tagged_new(x, TAG_ENUM, EN_SIZE, &h);
    if (!o)
        return X_ERROR_FUNCTION_FAILED;
    x360_w32(x, o + EN_KIND, kind);
    x360_w32(x, o + EN_ITEM, item);
    x360_w32(x, o + EN_TYPE, type);
    x360_w32(x, handle_ptr, h);
    return X_ERROR_SUCCESS;
}

/* up to cap items of the content directory after the ones already handed out */
static uint32_t content_fill(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t e, uint32_t buf, uint32_t cap)
{
    uint32_t tmp = tmp_new(x), h, count = 0, seen = 0;
    uint32_t pos = x360_r32(x, e + EN_POS), item = x360_r32(x, e + EN_ITEM), want = x360_r32(x, e + EN_TYPE);
    if (!tmp)
        return 0;
    if (path_open(x, t, c, tmp, CONTENT_DIR, 0x80000000u, 1, 1, &h)) {
        tmp_free(x, tmp);
        return 0;
    }
    while (count < cap) {
        uint32_t args[9] = {h, 0, 0, 0, tmp + T_IOSB, tmp + T_DIR, T_META - T_DIR, 0, 0};
        if (nt(x, t, c, tmp, x360_k_NtQueryDirectoryFile, 9, args))
            break;
        uint32_t nl = x360_r32(x, tmp + T_DIR + 60);
        char name[48], path[128];
        if (!(x360_r32(x, tmp + T_DIR + 56) & 0x10) || nl >= sizeof name)
            continue;
        memcpy(name, x->base + tmp + T_DIR + 0x40, nl);
        name[nl] = 0;
        if (name[0] == '.')
            continue;
        uint32_t m, have = 0;
        snprintf(path, sizeof path, CONTENT_DIR "\\.meta\\%s.dat", name);
        if (!path_open(x, t, c, tmp, path, 0x80000000u, 1, 0x40, &m)) {
            have = file_read(x, t, c, tmp, m, tmp + T_META, XCONTENT_SIZE) == XCONTENT_SIZE;
            file_close(x, m);
        }
        uint32_t type = have ? x360_r32(x, tmp + T_META + 4) : 1; /* no record: made by XamContentCreateEx */
        if ((want && type != want) || seen++ < pos)
            continue;
        uint32_t p = buf + count * item;
        memset(x->base + p, 0, item);
        if (have)
            memcpy(x->base + p, x->base + tmp + T_META, XCONTENT_SIZE);
        else
            put_w(x, p + 8, 128, name);
        x360_w32(x, p, HDD_ID);
        x360_w32(x, p + 4, type);
        memset(x->base + p + 0x108, 0, 42);
        memcpy(x->base + p + 0x108, name, nl);
        if (item >= XCONTENT_AGGREGATE_SIZE)
            x360_w32(x, p + 0x140, x->title_id);
        count++;
    }
    file_close(x, h);
    tmp_free(x, tmp);
    return count;
}

static void x_XamEnumerate(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (handle, flags, buffer, size, items returned*, overlapped) */
    uint32_t e = tagged_of(x, A(0), TAG_ENUM), buf = A(2), size = A(3), outp = A(4), ov = A(5);
    if (!e) {
        RET(X_ERROR_INVALID_HANDLE);
        return;
    }
    if (!buf) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    uint32_t item = x360_r32(x, e + EN_ITEM), kind = x360_r32(x, e + EN_KIND), pos = x360_r32(x, e + EN_POS);
    uint32_t count = 0, result = X_ERROR_SUCCESS;
    if (outp)
        x360_w32(x, outp, 0);
    if (item && size < item && kind != EN_EMPTY)
        result = X_ERROR_INSUFFICIENT_BUFFER;
    else if (kind == EN_CONTENT)
        count = content_fill(x, t, c, e, buf, size / item);
    else if (kind == EN_DEVICES && pos == 0) {
        device_put(x, buf);
        count = 1;
    }
    if (result == X_ERROR_SUCCESS && !count)
        result = X_ERROR_NO_MORE_FILES;
    x360_w32(x, e + EN_POS, pos + count);
    if (outp)
        x360_w32(x, outp, count);
    RET(finish_ex(x, t, ov, result, result, hresult_of(result), count));
}

static void x_XamContentCreateEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (user, device, type, flags, items per call, buffer size*, handle*) */
    uint32_t user = A(0), dev = A(1), items = A(4) ? A(4) : 1, sizep = A(5), hp = A(6);
    if (user && user < 4) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    if ((dev && dev != HDD_ID) || !hp) {
        if (sizep)
            x360_w32(x, sizep, 0);
        RET(X_E_INVALIDARG);
        return;
    }
    if (sizep)
        x360_w32(x, sizep, XCONTENT_SIZE * items);
    RET(enum_new(x, EN_CONTENT, XCONTENT_SIZE, A(2), hp));
}

static void x_XamContentAggregateCreateEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (xuid, device, type, unknown, handle*) */
    if ((A(1) && A(1) != HDD_ID) || !A(4)) {
        RET(X_E_INVALIDARG);
        return;
    }
    RET(enum_new(x, EN_CONTENT, XCONTENT_AGGREGATE_SIZE, A(2), A(4)));
}

static void x_XamContentCreateDeviceEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (content type, flags, max items, buffer size*, handle*) */
    if (!A(4)) {
        RET(X_E_INVALIDARG);
        return;
    }
    if (A(3))
        x360_w32(x, A(3), DEVICE_DATA_SIZE * A(2));
    RET(enum_new(x, EN_DEVICES, DEVICE_DATA_SIZE, 0, A(4)));
}

static void x_XamCreateEnumeratorHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_INVALID_PARAMETER);
}

static void x_XamGetPrivateEnumStructureFromHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t e = tagged_of(x, A(0), TAG_ENUM);
    if (!e) {
        RET(X_STATUS_INVALID_HANDLE);
        return;
    }
    if (A(1))
        x360_w32(x, A(1), e);
    RET(X_STATUS_SUCCESS);
}

static void x_XamUserCreateAchievementEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (title, user, xuid, flags, offset, count, buffer size*, handle*): this profile has earned nothing */
    uint32_t count = A(5), sizep = A(6), hp = A(7), item = 36 + (A(3) & 7 ? 464 : 0);
    if (!count || !sizep || !hp || A(1) >= 4) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, sizep, item * count);
    RET(enum_new(x, EN_EMPTY, item, 0, hp));
}

static void x_XamUserCreateStatsEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (title, user, count, flags, size, stats*, buffer size*, handle*): the leaderboards are on Live */
    uint32_t sizep = A(6), hp = A(7);
    if (!A(2) || !sizep || !hp || A(1) >= 4) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, sizep, 0x18 * A(2));
    RET(enum_new(x, EN_EMPTY, 0x18, 0, hp));
}

static void x_XamUserCreateTitlesPlayedEnumerator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (title, user, xuid, first, count, buffer size*, handle*): no record of titles played */
    uint32_t sizep = A(5), hp = A(6);
    if (!A(4) || !sizep || !hp || A(1) >= 4) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    x360_w32(x, sizep, 0x128 * A(4));
    RET(enum_new(x, EN_EMPTY, 0x128, 0, hp));
}

/* ---- profile settings ---- */

#define SET_SIZE 40u
#define SET_FROM 0u
#define SET_USER 8u
#define SET_ID 16u
#define SET_TYPE 24u
#define SET_VALUE 32u
#define SET_PTR 36u
#define TYPE_WSTRING 4u
#define TYPE_BINARY 6u

static uint32_t setting_extra(uint32_t id)
{
    uint32_t type = id >> 28;
    return type == TYPE_WSTRING || type == TYPE_BINARY ? (id >> 16) & 0xFFF : 0;
}

static uint32_t setting_default(uint32_t id)
{
    static const uint32_t table[][2] = {{0x10040003u, 3},           {0x10040006u, 0xFA},
                                        {0x1004000Eu, 0x64},        {0x10040012u, 1},
                                        {0x1004001Du, 0xFFFF0000u}, {0x1004001Eu, 0xFF00FF00u},
                                        {0x10040022u, 1}};
    for (size_t k = 0; k < sizeof table / sizeof table[0]; k++)
        if (table[k][0] == id)
            return table[k][1];
    return 0;
}

/* the root's entry of a setting: {id, length, value (8 bytes: a scalar, or 0 and the data's address)} */
static uint32_t setting_find(x360_machine *x, uint32_t root, uint32_t id)
{
    uint32_t n = root ? x360_r32(x, root + ROOT_NSET) : 0;
    for (uint32_t k = 0; k < n; k++)
        if (x360_r32(x, root + ROOT_SET + 16 * k) == id)
            return root + ROOT_SET + 16 * k;
    return 0;
}

static uint32_t read_profile(x360_machine *x, x360_thread *t, xe_cpu *c, uint32_t buf, uint32_t ov)
{
    /* (title, user, xuid count, xuids, setting count, setting ids, buffer size*) */
    uint32_t user = A(1), xn = A(2), xuids = A(3), n = A(4), ids = A(5), sizep = A(6);
    if (!n || n > 32 || !ids || !sizep || (x360_r32(x, sizep) && !buf) || (xuids && !xn))
        return X_ERROR_INVALID_PARAMETER;
    if (!xuids && user)
        return finish(x, t, ov, X_ERROR_NO_SUCH_USER, 0);
    uint32_t rows = xuids ? xn : 1, extra = 0;
    for (uint32_t k = 0; k < n; k++)
        extra += setting_extra(x360_r32(x, ids + 4 * k));
    uint64_t need64 = 8 + (uint64_t)rows * (SET_SIZE * n + extra);
    if (need64 > 0x100000)
        return X_ERROR_INVALID_PARAMETER;
    uint32_t need = (uint32_t)need64;
    if (!buf || x360_r32(x, sizep) < need) {
        x360_w32(x, sizep, need);
        return X_ERROR_INSUFFICIENT_BUFFER; /* answered directly, with an overlapped or without */
    }
    memset(x->base + buf, 0, need);
    x360_w32(x, buf, rows * n);
    x360_w32(x, buf + 4, buf + 8);
    uint32_t root = root_of(x), s = buf + 8, data = buf + 8 + rows * n * SET_SIZE;
    for (uint32_t r = 0; r < rows; r++) {
        uint64_t who = xuids ? x360_r64(x, xuids + 8 * r) : 0;
        int mine = !xuids || who == USER_XUID;
        for (uint32_t k = 0; k < n; k++, s += SET_SIZE) {
            uint32_t id = x360_r32(x, ids + 4 * k), type = id >> 28, room = setting_extra(id);
            uint32_t e = mine ? setting_find(x, root, id) : 0;
            uint32_t from = (id & 0x3F00) == 0x3F00 ? 2 : 1;
            if (xuids)
                x360_w64(x, s + SET_USER, who);
            else
                x360_w32(x, s + SET_USER, user);
            x360_w32(x, s + SET_ID, id);
            x360_w8(x, s + SET_TYPE, type);
            if (type == TYPE_WSTRING || type == TYPE_BINARY) {
                uint32_t len = e ? x360_r32(x, e + 4) : 0;
                if (len > room)
                    len = room;
                if (len)
                    memcpy(x->base + data, x->base + x360_r32(x, e + 12), len);
                x360_w32(x, s + SET_VALUE, len);
                x360_w32(x, s + SET_PTR, data);
                x360_w32(x, s + SET_FROM, e ? from : 0);
                data += room;
            } else if (!mine) {
                x360_w32(x, s + SET_FROM, 0);
            } else {
                if (e)
                    x360_w64(x, s + SET_VALUE, x360_r64(x, e + 8));
                else
                    x360_w32(x, s + SET_VALUE, setting_default(id));
                x360_w32(x, s + SET_FROM, e ? from : 1);
            }
        }
    }
    return finish(x, t, ov, X_ERROR_SUCCESS, need);
}

static void x_XamUserReadProfileSettings(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (..., buffer size*, buffer, overlapped) */
    uint32_t buf = A(7), ov = A9(8);
    RET(read_profile(x, t, c, buf, ov));
}

static void x_XamUserReadProfileSettingsEx(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (..., buffer size*, unknown, buffer, overlapped) */
    uint32_t buf = A9(8), ov = A9(9);
    RET(read_profile(x, t, c, buf, ov));
}

static void x_XamUserWriteProfileSettings(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (title, user, setting count, X_USER_PROFILE_SETTING*, overlapped): kept for this run */
    uint32_t user = A(1), n = A(2), s = A(3), ov = A(4);
    if (!n || !s) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    if (user) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    uint32_t root = root_of(x), result = root ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
    for (uint32_t k = 0; root && k < n; k++, s += SET_SIZE) {
        uint32_t id = x360_r32(x, s + SET_ID), type = x360_r8(x, s + SET_TYPE), e = setting_find(x, root, id);
        if (type == 0 || type == 0xFF)
            continue;
        if (!e) {
            uint32_t count = x360_r32(x, root + ROOT_NSET);
            if (count >= ROOT_MAXSET) {
                result = X_ERROR_FUNCTION_FAILED;
                break;
            }
            e = root + ROOT_SET + 16 * count;
            x360_w32(x, e, id);
            x360_w32(x, root + ROOT_NSET, count + 1);
        }
        if (type == TYPE_WSTRING || type == TYPE_BINARY) {
            uint32_t room = setting_extra(id), len = x360_r32(x, s + SET_VALUE), src = x360_r32(x, s + SET_PTR);
            uint32_t data = x360_r32(x, e + 12);
            if (len > room)
                len = room;
            if (!data && room && !(data = x360_kalloc(x, room))) {
                result = X_ERROR_FUNCTION_FAILED;
                break;
            }
            if (src && len)
                memmove(x->base + data, x->base + src, len);
            else if (len)
                memset(x->base + data, 0, len);
            x360_w32(x, e + 4, len);
            x360_w32(x, e + 12, data);
        } else {
            x360_w64(x, e + 8, x360_r64(x, s + SET_VALUE));
        }
    }
    RET(finish(x, t, ov, result, 0));
}

/* ---- the user ---- */

static void x_ret0(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0);
}

static void x_function_failed(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_FUNCTION_FAILED);
}

static void x_XamUserGetIndexFromXUID(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (xuid, flags, index*) */
    if (!A(2)) {
        RET(X_E_INVALIDARG);
        return;
    }
    if (c->gpr[3] != USER_XUID) {
        RET(X_E_NO_SUCH_USER);
        return;
    }
    x360_w32(x, A(2), 0);
    RET(X_ERROR_SUCCESS);
}

static void x_XamUserGetDeviceContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(2))
        x360_w32(x, A(2), 0);
    RET(A(0) == 0 || (A(0) & 0xFF) == 0xFF ? X_ERROR_SUCCESS : X_E_DEVICE_NOT_CONNECTED);
}

static void x_XamUserContentRestrictionGetFlags(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0)) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    if (A(1))
        x360_w32(x, A(1), 0);
    RET(X_ERROR_SUCCESS);
}

static void x_XamUserContentRestrictionGetRating(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0)) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    if (A(2))
        x360_w32(x, A(2), 0x3F);
    if (A(3))
        x360_w32(x, A(3), 0);
    RET(X_ERROR_SUCCESS);
}

static void x_XamUserContentRestrictionCheckAccess(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, ?, content type, ?, ?, allowed*, overlapped) */
    if (A(5))
        x360_w32(x, A(5), 1);
    RET(finish(x, t, A(6), X_ERROR_SUCCESS, 0));
}

static void x_XamUserAreUsersFriends(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, xuids*, count, are friends*, overlapped): a local profile has no friends list */
    uint32_t user = A(0), out = A(3), ov = A(4);
    if (user >= 4) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    if (out)
        x360_w32(x, out, 0);
    uint32_t result = user ? X_ERROR_NO_SUCH_USER : X_ERROR_NOT_LOGGED_ON;
    RET(finish_ex(x, t, ov, result, X_ERROR_FUNCTION_FAILED, hresult_of(result), 0));
}

static void x_XamParseGamerTileKey(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (key*, title id*, big tile id*, small tile id*): this profile's key names no tile */
    if (!A(0)) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    for (int k = 1; k < 4; k++)
        if (A(k))
            x360_w32(x, A(k), 0);
    RET(X_ERROR_SUCCESS);
}

static void x_XamReadTileToTexture(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (type, title, tile id, user, buffer, stride, height, overlapped): no tile store */
    if (!c->gpr[5]) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    RET(finish(x, t, A(7), X_ERROR_FILE_NOT_FOUND, 0));
}

static void x_XamWriteGamerTile(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    RET(finish(x, t, A(5), X_ERROR_FUNCTION_FAILED, 0));
}

static void x_XamSessionCreateHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        x360_w32(x, A(0), 0);
    RET(X_ERROR_FUNCTION_FAILED); /* sessions are a Live service */
}

static void x_XamSessionRefObjByHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(1))
        x360_w32(x, A(1), 0);
    RET(X_ERROR_INVALID_HANDLE);
}

/* ---- UI, answered as headless ---- */

static void ui_shown(x360_machine *x)
{
    broadcast(x, XN_SYS_UI, 1);
    broadcast(x, XN_SYS_UI, 0);
}

static void x_ui_ok(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t user = A(0);
    if (user && user < 4) {
        RET(X_ERROR_NO_SUCH_USER);
        return;
    }
    ui_shown(x);
    RET(X_ERROR_SUCCESS);
}

static void x_XamShowKeyboardUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, flags, default text, title, description, buffer, length in characters, overlapped) */
    uint32_t def = A(2), buf = A(5), cap = A(6), ov = A(7), n = 0;
    if (!buf) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    ui_shown(x);
    memset(x->base + buf, 0, 2 * (size_t)cap);
    for (; def && n + 1 < cap; n++) {
        uint16_t ch = x360_r16(x, def + 2 * n);
        if (!ch)
            break;
        x360_w16(x, buf + 2 * n, ch);
    }
    RET(finish(x, t, ov, X_ERROR_SUCCESS, n));
}

static void x_XamShowMarketplaceDownloadItemsUI(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (user, type, offers*, offer count, hresult*, overlapped): dismissed, nothing was downloaded */
    uint32_t ov = A(5);
    if (A(0) >= 4 || !A(2) || A(3) > 6) {
        RET(X_ERROR_INVALID_PARAMETER);
        return;
    }
    ui_shown(x);
    if (A(4))
        x360_w32(x, A(4), X_E_FAIL);
    RET(finish(x, t, ov, X_ERROR_SUCCESS, 0));
}

/* ---- tasks ---- */

static void x_XamTaskSchedule(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (callback, message, optional arguments, handle*): a thread of its own */
    uint32_t stack = x->stack_size < 0x4000 ? 0x4000 : x->stack_size;
    x360_thread *n = x360_thread_create(x, stack, A(0), A(1), 0, 0);
    if (!n) {
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    if (A(3))
        x360_w32(x, A(3), n->handle);
    RET(X_STATUS_SUCCESS);
}

static void x_XamTaskCloseHandle(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x360_handle_close(x, A(0)) ? 0 : 1);
}

/* ---- information ---- */

static void x_XamGetOnlineSchema(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* {data*, size} before an empty schema of 44 bytes */
    uint32_t root = root_of(x), p = root ? x360_r32(x, root + ROOT_SCHEMA) : 0;
    if (root && !p && (p = x360_kalloc(x, 8 + 0x2C))) {
        x360_w32(x, p, p + 8);
        x360_w32(x, p + 4, 0x2C);
        x360_w32(x, root + ROOT_SCHEMA, p);
    }
    RET(p);
}

/* a FILETIME as a calendar date and time (the console's clock is UTC here) */
static void civil(uint64_t filetime, int *y, unsigned *m, unsigned *d, unsigned *hh, unsigned *mm)
{
    uint64_t secs = filetime / 10000000u;
    int64_t z = (int64_t)(secs / 86400) - 134774 + 719468; /* days since 0000-03-01 */
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + (int)era * 400 + (*m <= 2);
    *hh = (unsigned)(secs % 86400 / 3600);
    *mm = (unsigned)(secs % 3600 / 60);
}

static void x_XamFormatDateString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char s[32];
    int y;
    unsigned m, d, hh, mm;
    civil(c->gpr[4], &y, &m, &d, &hh, &mm);
    snprintf(s, sizeof s, "%02u/%02u/%04d", m, d, y);
    put_w(x, A(2), A(3), s);
    RET(0);
}

static void x_XamFormatTimeString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    char s[32];
    int y;
    unsigned m, d, hh, mm;
    civil(c->gpr[4], &y, &m, &d, &hh, &mm);
    snprintf(s, sizeof s, "%02u:%02u", hh, mm);
    put_w(x, A(2), A(3), s);
    RET(0);
}

static void locator(x360_machine *x, uint32_t module, const char *container, uint32_t res, uint32_t buf, uint32_t cap)
{
    char name[200], s[300];
    read_w(x, res, name, sizeof name);
    if (module)
        snprintf(s, sizeof s, "section://%X,%s#%s", module, container, name);
    else
        snprintf(s, sizeof s, "file://media:/%s.xzp#%s", container, name);
    put_w(x, buf, cap, s);
}

static void x_XamBuildResourceLocator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (module, container, resource, buffer, characters) */
    char container[64];
    read_w(x, A(1), container, sizeof container);
    locator(x, A(0), container, A(2), A(3), A(4));
    RET(0);
}

static void x_XamBuildGamercardResourceLocator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    locator(x, 0, "gamercrd", A(0), A(1), A(2));
    RET(0);
}

static void x_XamBuildSharedSystemResourceLocator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    locator(x, 0, "shrdres", A(0), A(1), A(2));
    RET(0);
}

static void x_XamBuildXamResourceLocator(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    locator(x, 0, "xam", A(0), A(1), A(2));
    RET(0);
}

static void x_XamQueryLiveHiveW(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_STATUS_INVALID_PARAMETER_1); /* the Live hive is a service */
}

static void x_XamLoaderGetDvdTrayState(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(2); /* closed, the title's disc inside */
}

static void x_XGetAudioFlags(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0x00010001u);
}

/* ---- locale: two-letter codes by id, "--" for an id without one ---- */

static const char LANGUAGES[] = "zzenjadefresitkozhpt--plrusvtrnbnlzh";
static const char LOCALES[] = "ZZAUATBEBRCACLCNCOCZDKFIFRDEGRHKHUINIEITJPKRMXNLNZNOPLPTSGSKZAESSECHTWGBUSRUZZTRARSAILAE";
static const char COUNTRIES[] = "ZZAEALAMARATAUAZBEBGBHBNBOBRBYBZCA--CHCLCNCOCRCZDEDKDODZECEEEGESFIFOFRGBGEGRGTHKHNHRHU"
                                "IDIEILINIQIRISITJMJOJPKEKGKRKWKZLBLILTLULVLYMAMCMKMNMOMVMXMYNINLNONZOMPAPEPHPKPLPRPTPY"
                                "QARORUSASESGSISK--SVSYTHTNTRTTTWUAUSUYUZVEVNYEZAZWZZ";
static const char ONLINE_LANGUAGES[] = "zzenjadefresitkozhptzhplrudafinbnlsvcselhuskidmsarbgethrheiskkltlvroslthtrukvips"
                                       "sqhybnbekmamfokaklswkylbmkmtmnneurrwwositk";
static const char ONLINE_COUNTRIES[] =
    "ZZAEALAMARATAUAZBEBGBHBNBOBRBYBZCA--CHCLCNCOCRCZDEDKDODZECEEEGESFIFOFRGBGEGRGTHKHNHRHUIDIEILINIQIRISITJMJOJPKEKG"
    "KRKWKZLBLILTLULVLYMAMCMKMNMOMVMXMYNINLNONZOMPAPEPHPKPLPRPTPYQARORUSASESGSISK--SVSYTHTNTRTTTWUAUSUYUZVEVNYEZAZW"
    "AF--ADAOAI--AGAWBSBDBBBJBMBTBABWBFBIKHCMCVKYCFTDCXCCKMCGCDCKCICYDJDM--GQERETFKFJGFPFGAGMGHGIGLGDGP--GGGNGWGYHTJE"
    "KILALSLRMGMWMLMTMHMQMRMUYTFMMDMEMSMZMMNANRNPANNCNENGNUNF--PWPSPGPNRERWWSSMSTSNRSSCSLSBSOLKSHKNLCPMVCSRSZTJTZTLTG"
    "TKTOTMTCTVUGVUVA--VGWFEHZMZZ";
static const uint8_t LOCALE_ONLINE_COUNTRY[44] = {2,   6,  5,  8,  13, 16, 19, 20, 21, 23, 25, 32, 34, 24, 37,
                                                  39,  42, 46, 44, 50, 53, 56, 71, 74, 76, 75, 82, 84, 91, 93,
                                                  109, 31, 90, 18, 101, 35, 103, 88, 236, 99, 4, 89, 45, 1};
#define LOCALE_US 36u
#define LANGUAGE_ENGLISH 1u

/* the code of an id in a table, NULL when there is none */
static const char *code_of(const char *table, uint32_t id, char out[3])
{
    if (id >= strlen(table) / 2 || table[2 * id] == '-')
        return NULL;
    out[0] = table[2 * id], out[1] = table[2 * id + 1], out[2] = 0;
    return out;
}

static uint32_t put_code(x360_machine *x, const char *s, uint32_t cap, uint32_t buf)
{
    if (cap >= 0x80000000u)
        return X_E_INVALIDARG;
    if (!s)
        return X_E_NOTFOUND;
    if (cap < strlen(s) + 1)
        return X_E_INSUFFICIENT_BUFFER;
    put_w(x, buf, cap, s);
    return 0;
}

#define CODE_CALL(fn, table)                                                                                           \
    static void fn(x360_machine *x, x360_thread *t, xe_cpu *c)                                                         \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        char s[3];                                                                                                     \
        RET(put_code(x, code_of(table, A(0), s), A(1), A(2)));                                                         \
    }
CODE_CALL(x_XamGetLanguageString, LANGUAGES)
CODE_CALL(x_XamGetLocaleString, LOCALES)
CODE_CALL(x_XamGetCountryString, COUNTRIES)
CODE_CALL(x_XamGetOnlineLanguageString, ONLINE_LANGUAGES)
CODE_CALL(x_XamGetOnlineCountryString, ONLINE_COUNTRIES)

static void pair_string(x360_machine *x, xe_cpu *c, const char *t1, const char *t2)
{
    /* (first id, second id, characters, buffer): "en-US" */
    char a[3], b[3], s[6];
    const char *pa = code_of(t1, A(0), a), *pb = code_of(t2, A(1), b);
    if (pa && pb)
        snprintf(s, sizeof s, "%s-%s", pa, pb);
    RET(put_code(x, pa && pb ? s : NULL, A(2), A(3)));
}

static void x_XamGetLanguageLocaleString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    pair_string(x, c, LANGUAGES, LOCALES);
}

static void x_XamGetOnlineLanguageAndCountryString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    pair_string(x, c, ONLINE_LANGUAGES, ONLINE_COUNTRIES);
}

static void x_XamGetLocale(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(LOCALE_US);
}

static void x_XamGetLanguage(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(LANGUAGE_ENGLISH);
}

static void x_XamGetOnlineCountryFromLocale(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(A(0) < sizeof LOCALE_ONLINE_COUNTRY ? LOCALE_ONLINE_COUNTRY[A(0)] : 0);
}

static void x_XamGetLocaleFromOnlineCountry(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    uint32_t r = 0;
    for (uint32_t k = 1; k < sizeof LOCALE_ONLINE_COUNTRY && !r; k++)
        if (LOCALE_ONLINE_COUNTRY[k] == A(0))
            r = k;
    RET(r);
}

static void x_XamGetCountryFromOnlineCountry(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(A(0) <= 110 ? A(0) : 0); /* the first 111 ids of the two lists are the same countries */
}

static void x_XamGetLanguageFromOnlineLanguage(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    char a[3], b[3];
    uint32_t r = 0;
    if (code_of(ONLINE_LANGUAGES, A(0), a))
        for (uint32_t k = 1; k < strlen(LANGUAGES) / 2 && !r; k++)
            if (code_of(LANGUAGES, k, b) && !strcmp(a, b))
                r = k;
    RET(r);
}

/* ---- services this console does not have ---- */

static void x_XamAvatarInitialize(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0xFFFFFFFFu);
}

static void x_XamPartyGetUserList(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0x807D0003u); /* not in a party */
}

static void x_XamVoiceCreate(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(2))
        x360_w32(x, A(2), 0);
    RET(X_ERROR_ACCESS_DENIED);
}

static void x_XamNuiGetDeviceStatus(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    if (A(0))
        memset(x->base + A(0), 0, 24); /* status 0: no Kinect */
    RET(0);
}

static void x_XMsgSystemProcessCall(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(X_ERROR_NOT_FOUND); /* no system application answers */
}

/* ---- the network: there is none ---- */

static void x_net_error(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0xFFFFFFFFu); /* SOCKET_ERROR */
}

static void x_one(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(1);
}

static void x_WSAGetLastError(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(WSAENETDOWN);
}

static void x_XNetGetOpt(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(WSAEINVAL);
}

static void x_XNetRandom(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    /* (caller, buffer, length) */
    uint32_t args[2] = {A(1), A(2)};
    nt(x, t, c, (uint32_t)c->gpr[1], x360_k_XeCryptRandom, 2, args);
    RET(0);
}

static void x_WSACreateEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    uint32_t o = x360_kalloc(x, 16);
    if (o)
        x360_w8(x, o, X_OBJ_EVENT_NOTIFICATION);
    RET(o ? x360_handle_new(x, o) : 0);
}

static void x_WSACloseEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    RET(x360_handle_close(x, A(0)) ? 0 : 1);
}

static void wsa_event_set(x360_machine *x, xe_cpu *c, uint32_t value)
{
    uint32_t o = x360_handle_obj(x, A(0));
    if (o)
        x360_w32(x, o + 4, value);
    RET(o ? 1 : 0);
}

static void x_WSASetEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    wsa_event_set(x, c, 1);
}

static void x_WSAResetEvent(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    wsa_event_set(x, c, 0);
}

static void x_XNetInAddrToString(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (caller, address, string*, size) */
    char s[16];
    uint32_t a = A(1), n = A(3);
    snprintf(s, sizeof s, "%u.%u.%u.%u", a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
    if (A(2) && n) {
        uint32_t len = (uint32_t)strlen(s) < n - 1 ? (uint32_t)strlen(s) : n - 1;
        memcpy(x->base + A(2), s, len);
        x360_w8(x, A(2) + len, 0);
    }
    RET(0);
}

static void x_XNetLookup(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    /* (caller, host or flags, event, result*) */
    if (A(3))
        x360_w32(x, A(3), 0);
    RET(WSAENETDOWN);
}

static void x_XNetRelease(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(A(1) ? 0 : X_STATUS_INVALID_PARAMETER);
}

/* ---- the table ---- */

const x360_hle_row x360_tab_xam2[] = {
    /* overlapped */
    X360_HX(XamGetOverlappedResult, x_XamGetOverlappedResult),
    X360_HX(XGetOverlappedResult, x_XamGetOverlappedResult),
    X360_HX(XGetOverlappedExtendedError, x_XGetOverlappedExtendedError),
    X360_HX(XMsgCompleteIORequest, x_XMsgCompleteIORequest),
    X360_HX(XMsgCancelIORequest, x_ret0),
    X360_HX(XMsgSystemProcessCall, x_XMsgSystemProcessCall),
    /* notifications */
    X360_HX(XamNotifyCreateListenerInternal, x_XamNotifyCreateListenerInternal),
    X360_HX(XNotifyBroadcast, x_XNotifyBroadcast),
    X360_HX(XNotifyDelayUI, x_ret0),
    X360_HX(XNotifyPositionUI, x_ret0),
    X360_HX(XNotifyQueueUI, x_ret0),
    /* content */
    X360_HX(XamContentCreate, x_XamContentCreate), X360_HX(XamContentCreateEx, x_XamContentCreateEx),
    X360_HX(XamContentCreateInternal, x_XamContentCreateInternal),
    X360_HX(XamContentFlush, x_XamContentFlush),
    X360_HX(XamContentClose, x_XamContentClose),
    X360_HX(XamContentDelete, x_XamContentDelete),
    X360_HX(XamContentDeleteInternal, x_XamContentDeleteInternal),
    X360_HX(XamContentGetCreator, x_XamContentGetCreator),
    X360_HX(XamContentGetLicenseMask, x_XamContentGetLicenseMask),
    X360_HX(XamContentGetThumbnail, x_XamContentGetThumbnail),
    X360_HX(XamContentSetThumbnail, x_XamContentSetThumbnail),
    X360_HX(XamContentResolve, x_XamContentResolve),
    X360_HX(XamContentResolveInternal, x_XamContentResolveInternal),
    X360_HX(XamContentOpenFile, x_XamContentOpenFile),
    X360_HX(XamContentGetDeviceData, x_XamContentGetDeviceData),
    X360_HX(XamContentGetDeviceState, x_XamContentGetDeviceState),
    X360_HX(XamContentGetDeviceName, x_XamContentGetDeviceName),
    /* enumerators */
    X360_HX(XamEnumerate, x_XamEnumerate),
    X360_HX(XamContentCreateEnumerator, x_XamContentCreateEnumerator),
    X360_HX(XamContentAggregateCreateEnumerator, x_XamContentAggregateCreateEnumerator),
    X360_HX(XamContentCreateDeviceEnumerator, x_XamContentCreateDeviceEnumerator),
    X360_HX(XamCreateEnumeratorHandle, x_XamCreateEnumeratorHandle),
    X360_HX(XamGetPrivateEnumStructureFromHandle, x_XamGetPrivateEnumStructureFromHandle),
    X360_HX(XamUserCreateAchievementEnumerator, x_XamUserCreateAchievementEnumerator),
    X360_HX(XamUserCreateStatsEnumerator, x_XamUserCreateStatsEnumerator),
    X360_HX(XamUserCreateTitlesPlayedEnumerator, x_XamUserCreateTitlesPlayedEnumerator),
    /* the user */
    X360_HX(XamUserReadProfileSettings, x_XamUserReadProfileSettings),
    X360_HX(XamUserReadProfileSettingsEx, x_XamUserReadProfileSettingsEx),
    X360_HX(XamUserWriteProfileSettings, x_XamUserWriteProfileSettings),
    X360_HX(XamUserGetMembershipTier, x_ret0),
    X360_HX(XamUserGetMembershipTierFromXUID, x_ret0),
    X360_HX(XamUserGetIndexFromXUID, x_XamUserGetIndexFromXUID),
    X360_HX(XamUserGetDeviceContext, x_XamUserGetDeviceContext),
    X360_HX(XamUserIsGuest, x_ret0),
    X360_HX(XamUserIsParentalControlled, x_ret0),
    X360_HX(XamUserGetUserFlags, x_ret0),
    X360_HX(XamUserGetCachedUserFlags, x_ret0),
    X360_HX(XamUserGetUserFlagsFromXUID, x_ret0),
    X360_HX(XamUserContentRestrictionGetFlags, x_XamUserContentRestrictionGetFlags),
    X360_HX(XamUserContentRestrictionGetRating, x_XamUserContentRestrictionGetRating),
    X360_HX(XamUserContentRestrictionCheckAccess, x_XamUserContentRestrictionCheckAccess),
    X360_HX(XamUserAreUsersFriends, x_XamUserAreUsersFriends),
    X360_HX(XamParseGamerTileKey, x_XamParseGamerTileKey),
    X360_HX(XamReadTileToTexture, x_XamReadTileToTexture),
    X360_HX(XamWriteGamerTile, x_XamWriteGamerTile),
    X360_HX(XamSessionCreateHandle, x_XamSessionCreateHandle),
    X360_HX(XamSessionRefObjByHandle, x_XamSessionRefObjByHandle),
    /* UI */
    X360_HX(XamIsUIActive, x_ret0),
    X360_HX(XamShowKeyboardUI, x_XamShowKeyboardUI),
    X360_HX(XamShowGamerCardUI, x_ui_ok),
    X360_HX(XamShowGamerCardUIForXUID, x_ui_ok),
    X360_HX(XamShowAchievementsUI, x_ui_ok),
    X360_HX(XamShowFriendsUI, x_ui_ok),
    X360_HX(XamShowMarketplaceUI, x_ui_ok),
    X360_HX(XamShowMarketplaceUIEx, x_ui_ok),
    X360_HX(XamShowMarketplaceDownloadItemsUI, x_XamShowMarketplaceDownloadItemsUI),
    X360_HX(XamShowPartyUI, x_function_failed),
    X360_HX(XamShowCommunitySessionsUI, x_function_failed),
    X360_HX(XamShowNuiTroubleshooterUI, x_ret0),
    /* tasks */
    X360_HX(XamTaskSchedule, x_XamTaskSchedule),
    X360_HX(XamTaskCloseHandle, x_XamTaskCloseHandle),
    X360_HX(XamTaskShouldExit, x_ret0),
    /* information */
    X360_HX(XamGetOnlineSchema, x_XamGetOnlineSchema),
    X360_HX(XamFormatDateString, x_XamFormatDateString),
    X360_HX(XamFormatTimeString, x_XamFormatTimeString),
    X360_HX(XamBuildResourceLocator, x_XamBuildResourceLocator),
    X360_HX(XamBuildGamercardResourceLocator, x_XamBuildGamercardResourceLocator),
    X360_HX(XamBuildSharedSystemResourceLocator, x_XamBuildSharedSystemResourceLocator),
    X360_HX(XamBuildLegacySystemResourceLocator, x_XamBuildSharedSystemResourceLocator),
    X360_HX(XamBuildXamResourceLocator, x_XamBuildXamResourceLocator),
    X360_HX(XamQueryLiveHiveW, x_XamQueryLiveHiveW),
    X360_HX(XCustomRegisterDynamicActions, x_ret0),
    X360_HX(XamLoaderGetDvdTrayState, x_XamLoaderGetDvdTrayState),
    X360_HX(XGetAudioFlags, x_XGetAudioFlags),
    X360_HX(XGetVideoCapabilities, x_ret0),
    /* locale */
    X360_HX(XamGetLocale, x_XamGetLocale),
    X360_HX(XamGetLocaleEx, x_XamGetLocale),
    X360_HX(XamGetLanguage, x_XamGetLanguage),
    X360_HX(XamGetLanguageString, x_XamGetLanguageString),
    X360_HX(XamGetLocaleString, x_XamGetLocaleString),
    X360_HX(XamGetCountryString, x_XamGetCountryString),
    X360_HX(XamGetOnlineLanguageString, x_XamGetOnlineLanguageString),
    X360_HX(XamGetOnlineCountryString, x_XamGetOnlineCountryString),
    X360_HX(XamGetLanguageLocaleString, x_XamGetLanguageLocaleString),
    X360_HX(XamGetOnlineLanguageAndCountryString, x_XamGetOnlineLanguageAndCountryString),
    X360_HX(XamGetOnlineCountryFromLocale, x_XamGetOnlineCountryFromLocale),
    X360_HX(XamGetLocaleFromOnlineCountry, x_XamGetLocaleFromOnlineCountry),
    X360_HX(XamGetCountryFromOnlineCountry, x_XamGetCountryFromOnlineCountry),
    X360_HX(XamGetLanguageFromOnlineLanguage, x_XamGetLanguageFromOnlineLanguage),
    /* avatar, party, voice, Kinect */
    X360_HX(XamAvatarInitialize, x_XamAvatarInitialize),
    X360_HX(XamAvatarShutdown, x_ret0),
    X360_HX(XamPartyGetUserList, x_XamPartyGetUserList),
    X360_HX(XamPartySendGameInvites, x_function_failed),
    X360_HX(XamPartySetCustomData, x_function_failed),
    X360_HX(XamPartyGetBandwidth, x_function_failed),
    X360_HX(XamVoiceIsActiveProcess, x_ret0),
    X360_HX(XamVoiceCreate, x_XamVoiceCreate),
    X360_HX(XamVoiceClose, x_ret0),
    X360_HX(XamVoiceHeadsetPresent, x_ret0),
    X360_HX(XamNuiGetDeviceStatus, x_XamNuiGetDeviceStatus),
    /* the network */
    X360_HX(NetDll_XNetCleanup, x_ret0),
    X360_HX(NetDll_XNetGetOpt, x_XNetGetOpt),
    X360_HX(NetDll_XNetRandom, x_XNetRandom),
    X360_HX(NetDll_WSAGetLastError, x_WSAGetLastError),
    X360_HX(NetDll_WSASetLastError, x_ret0),
    X360_HX(NetDll_WSARecvFrom, x_net_error),
    X360_HX(NetDll_WSASendTo, x_net_error),
    X360_HX(NetDll_WSAWaitForMultipleEvents, x_net_error),
    X360_HX(NetDll_WSACreateEvent, x_WSACreateEvent),
    X360_HX(NetDll_WSACloseEvent, x_WSACloseEvent),
    X360_HX(NetDll_WSASetEvent, x_WSASetEvent),
    X360_HX(NetDll_WSAResetEvent, x_WSAResetEvent),
    X360_HX(NetDll_XNetGetDebugXnAddr, x_one),
    X360_HX(NetDll_XNetXnAddrToMachineId, x_one),
    X360_HX(NetDll_XNetInAddrToString, x_XNetInAddrToString),
    X360_HX(NetDll_XNetXnAddrToInAddr, x_one),
    X360_HX(NetDll_XNetInAddrToXnAddr, x_one),
    X360_HX(NetDll_XNetSetSystemLinkPort, x_one),
    X360_HX(NetDll_XNetGetEthernetLinkStatus, x_ret0),
    X360_HX(NetDll_XNetDnsLookup, x_XNetLookup),
    X360_HX(NetDll_XNetDnsRelease, x_XNetRelease),
    X360_HX(NetDll_XNetQosServiceLookup, x_XNetLookup),
    X360_HX(NetDll_XNetQosRelease, x_XNetRelease),
    X360_HX(NetDll_XNetQosListen, x_function_failed),
    X360_HX(NetDll_shutdown, x_net_error),
    X360_HX(NetDll_ioctlsocket, x_net_error),
    X360_HX(NetDll_recvfrom, x_net_error),
    X360_HX(NetDll_sendto, x_net_error),
};
const unsigned x360_tab_xam2_n = sizeof x360_tab_xam2 / sizeof x360_tab_xam2[0];
