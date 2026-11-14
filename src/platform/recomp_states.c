/* recomp_states.c -- see recomp_states.h (XBA-182). */
#define _POSIX_C_SOURCE 200809L
#include "recomp_states.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SLOT_MAGIC "XRSLOT\0\0"
#define HEADER_BYTES 96u
#define BLOB_MAX (64u << 20)

const char *recomp_states_error(int code)
{
    switch (code) {
    case RECOMP_STATE_OK: return "ok";
    case RECOMP_STATE_ENOGAME: return "no console game";
    case RECOMP_STATE_EEMPTY: return "empty slot";
    case RECOMP_STATE_EGAME: return "another game's state";
    case RECOMP_STATE_EVERSION: return "another format version";
    case RECOMP_STATE_EIO: return "file error";
    case RECOMP_STATE_ESLOT: return "bad slot";
    case RECOMP_STATE_ECORRUPT: return "not a state file";
    default: return "?";
    }
}

/* ---- Files ---------------------------------------------------------------------------------------------------- */

int recomp_states_slot_path(const char *dir, int slot, char *out, size_t out_n)
{
    if (!dir || !*dir || !out || slot < RECOMP_STATE_AUTO || slot > RECOMP_STATE_SLOTS)
        return -1;
    int n = slot == RECOMP_STATE_AUTO ? snprintf(out, out_n, "%s/auto.st", dir)
                                      : snprintf(out, out_n, "%s/state%d.st", dir, slot);
    return n < 0 || (size_t)n >= out_n ? -1 : 0;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Header (little-endian): magic[8], version u32, header bytes u32, console[8], key[48], saved_ms i64, frames u64,
 * thumb w u16, thumb h u16, blob bytes u32 = 96 bytes; then the thumbnail (w * h u16), then the blob. */
int recomp_states_write(const char *path, const recomp_state_meta *meta, const uint16_t *thumb, const uint8_t *blob,
                        size_t blob_n)
{
    if (!path || !meta || !blob || !blob_n || blob_n > BLOB_MAX)
        return RECOMP_STATE_EIO;
    int tw = thumb ? meta->thumb_w : 0, th = thumb ? meta->thumb_h : 0;
    if (tw < 0 || th < 0 || tw > RECOMP_STATE_THUMB_W || th > RECOMP_STATE_THUMB_H)
        tw = th = 0;
    uint8_t hdr[HEADER_BYTES];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, SLOT_MAGIC, 8);
    put32(hdr + 8, RECOMP_STATE_SLOT_VERSION);
    put32(hdr + 12, HEADER_BYTES);
    memcpy(hdr + 16, meta->console, strnlen(meta->console, 7));
    memcpy(hdr + 24, meta->key, strnlen(meta->key, 47));
    put64(hdr + 72, (uint64_t)(meta->saved_ms ? meta->saved_ms : now_ms()));
    put64(hdr + 80, meta->frames);
    put16(hdr + 88, (uint32_t)tw);
    put16(hdr + 90, (uint32_t)th);
    put32(hdr + 92, (uint32_t)blob_n);

    char dir[1024], tmp[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return RECOMP_STATE_EIO;
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int i = 0; ok && i < tw * th; i++) {
        uint8_t px[2];
        put16(px, thumb[i]);
        ok = fwrite(px, 1, 2, f) == 2;
    }
    ok = ok && fwrite(blob, 1, blob_n, f) == blob_n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return RECOMP_STATE_EIO;
    }
    return RECOMP_STATE_OK;
}

int recomp_states_read(const char *path, recomp_state_meta *meta, uint16_t *thumb, uint8_t **blob, size_t *blob_n)
{
    if (blob)
        *blob = NULL;
    if (blob_n)
        *blob_n = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return errno == ENOENT || !path ? RECOMP_STATE_EEMPTY : RECOMP_STATE_EIO;
    uint8_t hdr[HEADER_BYTES];
    int rc = RECOMP_STATE_ECORRUPT;
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, SLOT_MAGIC, 8) != 0)
        goto out;
    if (get32(hdr + 8) != RECOMP_STATE_SLOT_VERSION || get32(hdr + 12) != HEADER_BYTES) {
        rc = RECOMP_STATE_EVERSION;
        goto out;
    }
    recomp_state_meta m;
    memset(&m, 0, sizeof m);
    memcpy(m.console, hdr + 16, 7);
    memcpy(m.key, hdr + 24, 47);
    m.saved_ms = (int64_t)get64(hdr + 72);
    m.frames = get64(hdr + 80);
    m.thumb_w = (int)get16(hdr + 88);
    m.thumb_h = (int)get16(hdr + 90);
    uint32_t n = get32(hdr + 92);
    if (m.thumb_w > RECOMP_STATE_THUMB_W || m.thumb_h > RECOMP_STATE_THUMB_H || !n || n > BLOB_MAX)
        goto out;
    for (int i = 0; i < m.thumb_w * m.thumb_h; i++) {
        uint8_t px[2];
        if (fread(px, 1, 2, f) != 2)
            goto out;
        if (thumb)
            thumb[i] = (uint16_t)get16(px);
    }
    if (blob) {
        uint8_t *b = malloc(n);
        if (!b) {
            rc = RECOMP_STATE_EIO;
            goto out;
        }
        if (fread(b, 1, n, f) != n) {
            free(b);
            goto out;
        }
        *blob = b;
        if (blob_n)
            *blob_n = n;
    }
    if (meta)
        *meta = m;
    rc = RECOMP_STATE_OK;
out:
    fclose(f);
    return rc;
}

void recomp_states_thumb(const uint8_t *rgba, int w, int h, int stride, int aspect_w, int aspect_h, uint16_t *out,
                         int *out_w, int *out_h)
{
    *out_w = *out_h = 0;
    if (!rgba || w <= 0 || h <= 0 || !out)
        return;
    if (aspect_w <= 0 || aspect_h <= 0) {
        aspect_w = w;
        aspect_h = h;
    }
    /* The largest box of the display aspect inside THUMB_W x THUMB_H. */
    int tw = RECOMP_STATE_THUMB_W, th = (int)((int64_t)tw * aspect_h / aspect_w);
    if (th > RECOMP_STATE_THUMB_H) {
        th = RECOMP_STATE_THUMB_H;
        tw = (int)((int64_t)th * aspect_w / aspect_h);
    }
    if (tw < 1 || th < 1)
        return;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((int64_t)y * h / th), y1 = (int)((int64_t)(y + 1) * h / th);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((int64_t)x * w / tw), x1 = (int)((int64_t)(x + 1) * w / tw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, c = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const uint8_t *p = rgba + (size_t)yy * (size_t)stride + (size_t)xx * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    c++;
                }
            if (c) {
                r /= c;
                g /= c;
                b /= c;
            }
            out[y * tw + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
    *out_w = tw;
    *out_h = th;
}

/* ---- The attached game ---------------------------------------------------------------------------------------- */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static recomp_state_ops s_ops;
static char s_console[8];
static char s_dir[1024];
static int s_attached;
static int s_quick;   /* the quick slot, 0: not chosen yet this run */

void recomp_states_attach(const recomp_state_ops *ops, const char *dir)
{
    pthread_mutex_lock(&s_mu);
    if (ops) {
        s_ops = *ops;
        snprintf(s_console, sizeof s_console, "%s", ops->console ? ops->console : "");
        s_ops.console = s_console;
        snprintf(s_dir, sizeof s_dir, "%s", dir ? dir : "");
        s_attached = 1;
    } else {
        s_attached = 0;
        s_dir[0] = 0;
    }
    s_quick = 0;
    pthread_mutex_unlock(&s_mu);
}

void recomp_states_detach(void)
{
    recomp_states_attach(NULL, NULL);
}

int recomp_states_available(void)
{
    pthread_mutex_lock(&s_mu);
    int on = s_attached && s_dir[0] && s_ops.save && s_ops.load;
    pthread_mutex_unlock(&s_mu);
    return on;
}

const char *recomp_states_console(void)
{
    static char copy[8];
    pthread_mutex_lock(&s_mu);
    snprintf(copy, sizeof copy, "%s", s_attached ? s_console : "");
    pthread_mutex_unlock(&s_mu);
    return copy;
}

/* The registry's lock is held through the callback, so a detach waits for a save / load in progress; the runtime
 * detaches before it takes its own frame lock to end. */
static int run_op(int slot, int load)
{
    if (slot < RECOMP_STATE_AUTO || slot > RECOMP_STATE_SLOTS)
        return RECOMP_STATE_ESLOT;
    pthread_mutex_lock(&s_mu);
    int rc = RECOMP_STATE_ENOGAME;
    if (s_attached && s_dir[0] && s_ops.save && s_ops.load)
        rc = load ? s_ops.load(slot, s_ops.user) : s_ops.save(slot, s_ops.user);
    if (rc == RECOMP_STATE_OK && slot != RECOMP_STATE_AUTO)
        s_quick = slot;
    pthread_mutex_unlock(&s_mu);
    return rc;
}

int recomp_states_save(int slot) { return run_op(slot, 0); }
int recomp_states_load(int slot) { return run_op(slot, 1); }

int recomp_states_info(int slot, recomp_state_meta *meta, uint16_t *thumb)
{
    char path[1100];
    pthread_mutex_lock(&s_mu);
    int ok = s_attached && recomp_states_slot_path(s_dir, slot, path, sizeof path) == 0;
    pthread_mutex_unlock(&s_mu);
    if (!ok)
        return slot < RECOMP_STATE_AUTO || slot > RECOMP_STATE_SLOTS ? RECOMP_STATE_ESLOT : RECOMP_STATE_ENOGAME;
    return recomp_states_read(path, meta, thumb, NULL, NULL);
}

/* ---- Quick save / load, resume, events ------------------------------------------------------------------------ */

static recomp_state_event s_event;   /* under s_ev_mu */
static pthread_mutex_t s_ev_mu = PTHREAD_MUTEX_INITIALIZER;

static void note(int kind, int slot, int rc)
{
    pthread_mutex_lock(&s_ev_mu);
    s_event.seq++;
    s_event.kind = kind;
    s_event.slot = slot;
    s_event.rc = rc;
    pthread_mutex_unlock(&s_ev_mu);
}

void recomp_states_last_event(recomp_state_event *ev)
{
    if (!ev)
        return;
    pthread_mutex_lock(&s_ev_mu);
    *ev = s_event;
    pthread_mutex_unlock(&s_ev_mu);
}

int recomp_states_quick_slot(void)
{
    pthread_mutex_lock(&s_mu);
    int q = s_quick;
    char dir[sizeof s_dir];
    snprintf(dir, sizeof dir, "%s", s_attached ? s_dir : "");
    pthread_mutex_unlock(&s_mu);
    if (q)
        return q;
    /* Not chosen yet this run: the most recently saved numbered slot, else slot 1. */
    int best = 1;
    int64_t best_ms = 0;
    for (int slot = 1; dir[0] && slot <= RECOMP_STATE_SLOTS; slot++) {
        char path[1100];
        recomp_state_meta m;
        if (recomp_states_slot_path(dir, slot, path, sizeof path) == 0 &&
            recomp_states_read(path, &m, NULL, NULL, NULL) == RECOMP_STATE_OK && m.saved_ms > best_ms) {
            best_ms = m.saved_ms;
            best = slot;
        }
    }
    return best;
}

int recomp_states_quick_save(void)
{
    int slot = recomp_states_quick_slot();
    int rc = recomp_states_save(slot);
    note(RECOMP_STATE_EV_SAVED, slot, rc);
    return rc;
}

int recomp_states_quick_load(void)
{
    int slot = recomp_states_quick_slot();
    int rc = recomp_states_load(slot);
    note(RECOMP_STATE_EV_LOADED, slot, rc);
    return rc;
}

int recomp_states_chord(int back, int lshoulder, int rshoulder, int *held, int *swallow)
{
    int l = back && lshoulder, r = back && rshoulder, action = RECOMP_CHORD_NONE;
    if (r && !(*held & 2))
        action = RECOMP_CHORD_SAVE;
    else if (l && !(*held & 1))
        action = RECOMP_CHORD_LOAD;
    *held = (l ? 1 : 0) | (r ? 2 : 0);
    if (swallow)
        *swallow = l || r;
    return action;
}

int recomp_states_chord_run(int action)
{
    if (action == RECOMP_CHORD_SAVE)
        return recomp_states_quick_save();
    if (action == RECOMP_CHORD_LOAD)
        return recomp_states_quick_load();
    return RECOMP_STATE_OK;
}

int recomp_states_resume_enabled(void)
{
    const char *v = getenv("RECOMP_STATE_RESUME");
    return v && (!strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "yes"));
}

int recomp_states_resume(void)
{
    int rc = recomp_states_load(RECOMP_STATE_AUTO);
    if (rc != RECOMP_STATE_EEMPTY)
        note(RECOMP_STATE_EV_RESUMED, RECOMP_STATE_AUTO, rc);
    return rc;
}

/* ---- Fast-forward --------------------------------------------------------------------------------------------- */

static atomic_int s_ff_toggle, s_ff_hold, s_ff_speed = -1;

void recomp_ff_set(int on) { atomic_store(&s_ff_toggle, on != 0); }
int recomp_ff_toggled(void) { return atomic_load(&s_ff_toggle); }
void recomp_ff_hold(int held) { atomic_store(&s_ff_hold, held != 0); }
int recomp_ff_active(void) { return atomic_load(&s_ff_toggle) || atomic_load(&s_ff_hold); }

int recomp_ff_speed(void)
{
    int s = atomic_load(&s_ff_speed);
    if (s >= 0)
        return s;
    const char *v = getenv("RECOMP_FF_SPEED");
    s = 4;
    if (v && *v) {
        if (!strcmp(v, "max") || !strcmp(v, "0") || !strcmp(v, "unlimited")) {
            s = 0;
        } else {
            int n = atoi(v[0] == 'x' || v[0] == 'X' ? v + 1 : v);
            if (n >= 2 && n <= 16)
                s = n;
        }
    }
    atomic_store(&s_ff_speed, s);
    return s;
}

void recomp_ff_reset(void)
{
    atomic_store(&s_ff_speed, -1);
    atomic_store(&s_ff_toggle, 0);
    atomic_store(&s_ff_hold, 0);
}

size_t recomp_ff_audio(int16_t *stereo, size_t n, int factor, recomp_ff_carry *carry)
{
    if (factor <= 1 || !stereo || !carry)
        return n;
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        carry->sum[0] += stereo[2 * i];
        carry->sum[1] += stereo[2 * i + 1];
        if (++carry->n == factor) {
            stereo[2 * out] = (int16_t)(carry->sum[0] / factor);
            stereo[2 * out + 1] = (int16_t)(carry->sum[1] / factor);
            out++;
            carry->sum[0] = carry->sum[1] = 0;
            carry->n = 0;
        }
    }
    return out;
}
