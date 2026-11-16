/* ctr_services.c -- the system services of the 3DS spike (XBA-228), high-level: what a libctru program's start-up
 * and its console need, nothing more. Commands arrive through svcSendSyncRequest with the command buffer at the
 * thread's TLS + 0x80 (header = id << 16 | normal words << 6 | translate words; the reply's word 1 is the result).
 * Command ids and reply layouts: 3dbrew (« Services », « srv: », « APT:U », « HID Services », « GSP Services »,
 * « Filesystem services ») and libctru 2.7's use of them.
 *
 * - srv: RegisterClient, EnableNotification, GetServiceHandle (sessions to the services below; any other name is
 *   « not found », so optional modules such as ptm:sysm turn themselves off).
 * - APT: the lock, Initialize (notification and resume events), Enable (which queues the WAKEUP parameter and signals
 *   the resume event, as the home menu does for a starting application), Glance / ReceiveParameter, the commands the
 *   start-up sends after that answered with success.
 * - HID: GetIPCHandles (a shared memory block that stays zero: no key, no touch, no circle pad; five events).
 * - GSP: rights, the interrupt relay queue (VBlank0 / VBlank1 pushed into the shared memory and its event signaled
 *   every frame, ctr_services_vblank), cache maintenance, SetBufferSwap and the framebuffer info libctru writes into
 *   the shared memory: what the core shows (ctr_core.c).
 * - FS: Initialize; no archive opens (the SD card is absent).
 * Unknown commands of a known service are answered with success and zeroed outputs, and counted (ctr_run --stats,
 * -v names them): the list of what a commercial title needs is in the epic (XBA-222). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_internal.h"

static const struct {
    const char *name;
    int service;
} names[] = {
    {"APT:U", CTR_SVC_APT}, {"APT:A", CTR_SVC_APT}, {"APT:S", CTR_SVC_APT},
    {"hid:USER", CTR_SVC_HID}, {"hid:SPVR", CTR_SVC_HID},
    {"gsp::Gpu", CTR_SVC_GSP},
    {"fs:USER", CTR_SVC_FS},
    {"cfg:u", CTR_SVC_CFG}, {"cfg:s", CTR_SVC_CFG}, {"cfg:i", CTR_SVC_CFG},
    {"ndm:u", CTR_SVC_NDM},
};

static const char *service_name[CTR_SVC_COUNT] = {"srv", "APT", "HID", "GSP", "FS", "CFG", "NDM"};

int ctr_service_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(names[i].name, name))
            return names[i].service;
    return -1;
}

/* ---- helpers ---- */

typedef struct ipc {
    ctr_machine *m;
    uint32_t buf;  /* guest address of the command buffer */
    uint32_t cmd;  /* command id */
} ipc;

static uint32_t arg(const ipc *c, int i)
{
    return ctr_r32(c->m, c->buf + 4u * (uint32_t)i);
}

static void put(const ipc *c, int i, uint32_t v)
{
    ctr_w32(c->m, c->buf + 4u * (uint32_t)i, v);
}

/* The reply header and result; normal / translate: word counts after the result. */
static void reply(const ipc *c, uint32_t result, unsigned normal, unsigned translate)
{
    put(c, 0, c->cmd << 16 | (normal + 1) << 6 | translate);
    put(c, 1, result);
}

static void unknown(const ipc *c, int service, uint32_t header)
{
    c->m->ipc_unknown++;
    if (c->m->verbose)
        fprintf(stderr, "ctr: %s command 0x%08X not implemented (answered with success)\n", service_name[service],
                header);
    (void)header;
    reply(c, CTR_RES_OK, 0, 0);
    for (int i = 2; i < 10; i++) /* outputs a caller may read: zero */
        put(c, i, 0);
}

static ctr_obj *new_event(ctr_machine *m, const char *name, int reset)
{
    ctr_obj *o = ctr_obj_new(m, CTR_OBJ_EVENT, name);
    if (o)
        o->u.ev.reset = reset;
    return o;
}

static ctr_obj *new_shm(ctr_machine *m, const char *name, uint32_t size)
{
    ctr_obj *o = ctr_obj_new(m, CTR_OBJ_SHMEM, name);
    if (!o)
        return NULL;
    o->u.shm.size = size;
    o->u.shm.buf = calloc(1, size); /* the backing store while unmapped (freed by ctr_destroy) */
    if (!o->u.shm.buf) {
        ctr_stop(m, "out of memory");
        o->type = CTR_OBJ_FREE;
        return NULL;
    }
    return o;
}

/* ---- srv: ---- */

static void srv_call(ipc *c, uint32_t header)
{
    ctr_machine *m = c->m;
    switch (c->cmd) {
    case 0x0001: /* RegisterClient */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0002: /* EnableNotification: a semaphore */
        if (!m->srv_notify) {
            m->srv_notify = ctr_obj_new(m, CTR_OBJ_SEMAPHORE, "srv notify");
            if (m->srv_notify)
                m->srv_notify->u.sem.max = 64;
        }
        reply(c, CTR_RES_OK, 0, 2);
        put(c, 2, 0);
        put(c, 3, ctr_handle_new(m, m->srv_notify));
        return;
    case 0x0005: { /* GetServiceHandle(name[8], length, flags) */
        char name[9] = {0};
        uint32_t len = arg(c, 3);
        for (uint32_t i = 0; i < 8 && i < len; i++)
            name[i] = (char)m->h.mem[c->buf + 4 + i];
        int s = ctr_service_lookup(name);
        if (m->verbose)
            fprintf(stderr, "ctr: srv:GetServiceHandle(\"%s\")%s\n", name, s < 0 ? ": not found" : "");
        if (s < 0) {
            reply(c, CTR_RES_NOT_FOUND, 0, 0);
            return;
        }
        ctr_obj *o = ctr_obj_new(m, CTR_OBJ_SESSION, name);
        if (!o)
            return;
        o->u.sess.service = s;
        reply(c, CTR_RES_OK, 0, 2);
        put(c, 2, 0x04000000u); /* move handle */
        put(c, 3, ctr_handle_new(m, o));
        return;
    }
    case 0x0009: /* Subscribe */
    case 0x000A: /* Unsubscribe */
    case 0x000C: /* PublishToSubscriber */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x000B: /* ReceiveNotification: none */
        reply(c, CTR_RES_OK, 1, 0);
        put(c, 2, 0);
        return;
    default:
        unknown(c, CTR_SVC_SRV, header);
        return;
    }
}

/* ---- APT ---- */

static void apt_call(ipc *c, uint32_t header)
{
    ctr_machine *m = c->m;
    switch (c->cmd) {
    case 0x0001: /* GetLockHandle: attributes, state, the lock (a mutex) */
        if (!m->apt_lock) {
            m->apt_lock = ctr_obj_new(m, CTR_OBJ_MUTEX, "apt lock");
            if (m->apt_lock)
                m->apt_lock->u.mtx.owner = -1;
        }
        reply(c, CTR_RES_OK, 2, 2);
        put(c, 2, 0);
        put(c, 3, 0);
        put(c, 4, 0x04000000u);
        put(c, 5, ctr_handle_new(m, m->apt_lock));
        return;
    case 0x0002: /* Initialize(appid, attributes): the notification and resume events */
        if (!m->apt_notify)
            m->apt_notify = new_event(m, "apt notify", CTR_RESET_ONESHOT);
        if (!m->apt_resume)
            m->apt_resume = new_event(m, "apt resume", CTR_RESET_ONESHOT);
        reply(c, CTR_RES_OK, 0, 3);
        put(c, 2, 0x04000000u | 1u << 26);
        put(c, 3, ctr_handle_new(m, m->apt_notify));
        put(c, 4, ctr_handle_new(m, m->apt_resume));
        return;
    case 0x0003: /* Enable: the home menu then wakes the application up */
        reply(c, CTR_RES_OK, 0, 0);
        m->apt_param_cmd = 1; /* APTCMD_WAKEUP */
        m->apt_param_sender = 0x101; /* APPID_HOMEMENU */
        if (m->apt_resume)
            ctr_event_signal(m, m->apt_resume);
        return;
    case 0x000B: /* InquireNotification: none */
        reply(c, CTR_RES_OK, 1, 0);
        put(c, 2, 0);
        return;
    case 0x000D: /* ReceiveParameter */
    case 0x000E: /* GlanceParameter */
        if (!m->apt_param_cmd) {
            reply(c, 0xC8A0CFEFu, 0, 0); /* no parameter */
            return;
        }
        reply(c, CTR_RES_OK, 3, 2);
        put(c, 2, m->apt_param_sender);
        put(c, 3, m->apt_param_cmd);
        put(c, 4, 0); /* parameter size */
        put(c, 5, 0); /* handle descriptor */
        put(c, 6, 0); /* no handle */
        if (c->cmd == 0x000D)
            m->apt_param_cmd = 0;
        return;
    case 0x004B: /* AppletUtility */
        reply(c, CTR_RES_OK, 1, 2);
        put(c, 2, 0);
        put(c, 3, 0);
        put(c, 4, 0);
        return;
    case 0x0043: /* NotifyToWait */
    case 0x004F: /* SetApplicationCpuTimeLimit */
    case 0x0044: /* SetScreenCapPostPermission */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0102: /* CheckNew3DS */
        reply(c, CTR_RES_OK, 1, 0);
        put(c, 2, 0);
        return;
    default:
        unknown(c, CTR_SVC_APT, header);
        return;
    }
}

/* ---- HID ---- */

static void hid_call(ipc *c, uint32_t header)
{
    ctr_machine *m = c->m;
    switch (c->cmd) {
    case 0x000A: /* GetIPCHandles: shared memory, pad 0 / pad 1 / accelerometer / gyroscope / debug pad events */
        if (!m->hid_shm) {
            m->hid_shm = new_shm(m, "hid", 0x2B0);
            for (int i = 0; i < 5; i++)
                m->hid_events[i] = new_event(m, "hid", CTR_RESET_ONESHOT);
        }
        reply(c, CTR_RES_OK, 0, 7);
        put(c, 2, 0x14000000u); /* copy 6 handles */
        put(c, 3, ctr_handle_new(m, m->hid_shm));
        for (int i = 0; i < 5; i++)
            put(c, 4 + i, ctr_handle_new(m, m->hid_events[i]));
        return;
    case 0x0011: /* EnableAccelerometer */
    case 0x0012: /* DisableAccelerometer */
    case 0x0013: /* EnableGyroscopeLow */
    case 0x0014: /* DisableGyroscopeLow */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    default:
        unknown(c, CTR_SVC_HID, header);
        return;
    }
}

/* ---- GSP ---- */

static void gsp_set_fb(ctr_machine *m, unsigned screen, uint32_t addr, uint32_t stride, uint32_t format)
{
    if (screen > 1)
        return;
    m->fb[screen].addr = addr;
    m->fb[screen].stride = stride;
    m->fb[screen].format = format;
    m->fb[screen].valid = true;
}

static void gsp_call(ipc *c, uint32_t header)
{
    ctr_machine *m = c->m;
    switch (c->cmd) {
    case 0x0016: /* AcquireRight */
        m->gsp_right = true;
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0017: /* ReleaseRight */
        m->gsp_right = false;
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0013: { /* RegisterInterruptRelayQueue(flags, event): thread id 0, the shared memory */
        m->gsp_event = ctr_handle_get(m, arg(c, 3));
        if (!m->gsp_shm)
            m->gsp_shm = new_shm(m, "gsp", 0x1000);
        reply(c, CTR_RES_OK, 1, 2);
        put(c, 2, 0);
        put(c, 3, 0x04000000u);
        put(c, 4, ctr_handle_new(m, m->gsp_shm));
        return;
    }
    case 0x0014: /* UnregisterInterruptRelayQueue */
        m->gsp_event = NULL;
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0005: /* SetBufferSwap(screen, FramebufferInfo) */
        gsp_set_fb(m, arg(c, 1), arg(c, 3), arg(c, 5), arg(c, 6));
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0004: { /* ReadHWRegs(reg, size): zeros into the static buffer */
        uint32_t size = arg(c, 2), out = ctr_r32(m, m->h.cpu.tls + 0x184);
        if (ctr_mem_mapped(m, out, size))
            memset(m->h.mem + out, 0, size);
        reply(c, CTR_RES_OK, 0, 0);
        return;
    }
    case 0x0001: /* WriteHWRegs */
    case 0x0002: /* WriteHWRegsWithMask */
    case 0x0008: /* FlushDataCache */
    case 0x0009: /* InvalidateDataCache */
    case 0x000B: /* SetLcdForceBlack */
    case 0x000C: /* TriggerCmdReqQueue: the GX commands are not run (no GPU) */
    case 0x0019: /* SaveVramSysArea */
    case 0x001A: /* RestoreVramSysArea */
    case 0x001F: /* SetInternalPriorities */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x0018: /* ImportDisplayCaptureInfo */
        reply(c, CTR_RES_OK, 8, 0);
        for (int i = 0; i < 8; i++)
            put(c, 2 + i, 0);
        return;
    default:
        unknown(c, CTR_SVC_GSP, header);
        return;
    }
}

/* ---- FS ---- */

static void fs_call(ipc *c, uint32_t header)
{
    switch (c->cmd) {
    case 0x0801: /* Initialize */
    case 0x0861: /* InitializeWithSdkVersion */
    case 0x0862: /* SetPriority */
        reply(c, CTR_RES_OK, 0, 0);
        return;
    case 0x080C: /* OpenArchive: none (no SD card, no save data) */
    case 0x0802: /* OpenFile */
    case 0x0803: /* OpenFileDirectly */
        reply(c, CTR_RES_FS_NOT_FOUND, 0, 0);
        return;
    case 0x0817: /* IsSdmcDetected */
    case 0x0818: /* IsSdmcWritable */
        reply(c, CTR_RES_OK, 1, 0);
        put(c, 2, 0);
        return;
    default:
        unknown(c, CTR_SVC_FS, header);
        return;
    }
}

void ctr_service_call(ctr_machine *m, ctr_obj *session, uint32_t cmdbuf)
{
    ipc c = {m, cmdbuf, 0};
    uint32_t header = ctr_r32(m, cmdbuf);
    c.cmd = header >> 16;
    int s = session->u.sess.service;
    if (m->verbose >= 2)
        fprintf(stderr, "ctr: ipc %s 0x%08X\n", service_name[s], header);
    switch (s) {
    case CTR_SVC_SRV: srv_call(&c, header); return;
    case CTR_SVC_APT: apt_call(&c, header); return;
    case CTR_SVC_HID: hid_call(&c, header); return;
    case CTR_SVC_GSP: gsp_call(&c, header); return;
    case CTR_SVC_FS: fs_call(&c, header); return;
    default: unknown(&c, s, header); return;
    }
}

/* ---- the VBlank ---- */

/* Pushes interrupt id into the relay queue of GSP thread 0 (shared memory + 0: u8 first, u8 count, u8 error, u8
 * unused; ids from + 0xC, 0x34 slots). */
static void gsp_push(ctr_machine *m, uint8_t id)
{
    uint8_t *q = ctr_shm_ptr(m, m->gsp_shm);
    unsigned first = q[0], count = q[1];
    if (count >= 0x34) {
        q[2] = 1; /* overflow */
        return;
    }
    q[0xC + (first + count) % 0x34] = id;
    q[1] = (uint8_t)(count + 1);
}

void ctr_services_vblank(ctr_machine *m)
{
    if (!m->gsp_shm)
        return;
    /* What the program asked GSP to show: per screen at 0x200 + screen * 0x40 (thread 0), u8 index, u8 « updated »,
     * then two GSPGPU_FramebufferInfo (active, left address, right address, stride, format, select, unk). */
    uint8_t *s = ctr_shm_ptr(m, m->gsp_shm);
    for (unsigned screen = 0; screen < 2; screen++) {
        uint8_t *hdr = s + 0x200 + screen * 0x40;
        if (!hdr[1])
            continue;
        unsigned idx = hdr[0] & 1;
        uint32_t w[7];
        memcpy(w, hdr + 4 + idx * 0x1C, sizeof w);
        gsp_set_fb(m, screen, w[1], w[3], w[4]);
        hdr[1] = 0;
    }
    if (m->gsp_event) {
        gsp_push(m, 2); /* VBlank0 (top) */
        gsp_push(m, 3); /* VBlank1 (bottom) */
        ctr_event_signal(m, m->gsp_event);
    }
}
