/* ctr_core.c -- the 3DS spike's machine (XBA-228): creation (address space, configuration memory, the 3DSX, the main
 * thread), the frame loop (threads, timeouts, idle skips, the VBlank) and the screens GSP shows. */
#include <stdlib.h>
#include <string.h>

#include "ctr_internal.h"

static void config_memory(ctr_machine *m)
{
    uint8_t *c = m->h.mem + CTR_CONFIG_MEM;
    /* Kernel and FIRM version 2.55.0 (system 11.17), APPMEMTYPE 0 (64 MB), APPMEMALLOC (3dbrew « Configuration
     * Memory »); the shared page says « product » hardware (RUNNING_HW 1). */
    static const uint8_t ver[4] = {0, 0, 55, 2};
    memcpy(c + 0x00, ver, 4);
    memcpy(c + 0x60, ver, 4);
    ctr_wr32(c, 0x30, 0);
    ctr_wr32(c, 0x40, CTR_APP_MEMORY);
    ctr_wr32(c, 0x44, 0x02C00000u);
    ctr_wr32(c, 0x48, 0x01400000u);
    c[0x1004] = 1;
}

ctr_machine *ctr_create(const uint8_t *data, size_t size, char *why, size_t why_n)
{
    ctr_machine *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(why, why_n, "out of memory");
        return NULL;
    }
    if (ctr_mem_init(m)) {
        snprintf(why, why_n, "cannot reserve the 4 GB guest address space (needs a 64-bit host)");
        free(m);
        return NULL;
    }
    if (ctr_mem_map(m, CTR_CONFIG_MEM, 0x2000, 3, 11) || ctr_mem_map(m, CTR_TLS_BASE, 0x200 * CTR_MAX_THREADS, 3, 10)
        || ctr_mem_map(m, CTR_STACK_TOP - CTR_STACK_SIZE, CTR_STACK_SIZE, 3, 10)) {
        snprintf(why, why_n, "cannot map the process's fixed regions");
        ctr_destroy(m);
        return NULL;
    }
    config_memory(m);
    ctr_mem_protect(m, CTR_CONFIG_MEM, 0x2000, 1);
    if (ctr_load_3dsx(m, data, size, why, why_n)) {
        ctr_destroy(m);
        return NULL;
    }
    ctr_kernel_init(m);
    if (ctr_kernel_start(m, m->entry)) {
        snprintf(why, why_n, "cannot create the main thread");
        ctr_destroy(m);
        return NULL;
    }
    for (int i = 0; i < 1024; i++)
        m->rc_cache_pc[i] = 0xFFFFFFFFu;
    return m;
}

void ctr_destroy(ctr_machine *m)
{
    if (!m)
        return;
    for (int i = 0; i < CTR_MAX_OBJECTS; i++)
        if (m->objs[i].type == CTR_OBJ_SHMEM)
            free(m->objs[i].u.shm.buf);
    ctr_mem_free(m);
    free(m->missed);
    free(m);
}

void ctr_set_verbose(ctr_machine *m, int level)
{
    m->verbose = level;
}

const char *ctr_error(const ctr_machine *m)
{
    return m->error;
}

uint64_t ctr_frames(const ctr_machine *m)
{
    return m->frames;
}

void ctr_dump_trace(const ctr_machine *m, FILE *f)
{
    fprintf(f, "last interpreted PCs (oldest first):");
    for (unsigned i = 0; i < 64; i++) {
        uint32_t pc = m->trace[(m->trace_at + i) & 63];
        if (pc)
            fprintf(f, "%s%08X", i % 8 ? " " : "\n  ", pc);
    }
    const ctr_cpu *c = &m->h.cpu;
    fprintf(f, "\nthread %d registers:", m->cur);
    for (int i = 0; i < 16; i++)
        fprintf(f, "%s r%d=%08X", i % 4 ? "" : "\n ", i, c->r[i]);
    fprintf(f, "\n cpsr=%08X fpscr=%08X tls=%08X now=%llu\n", ctr_cpu_cpsr(c), c->fpscr, c->tls,
            (unsigned long long)m->h.now);
}

/* ---- the screens ---- */

static void to_rgb(ctr_machine *m, const ctr_fb_info *fb, int w, int h, uint8_t *out, bool *valid)
{
    static const unsigned bpp_of[8] = {4, 3, 2, 2, 2, 3, 3, 3};
    unsigned format = fb->format & 7, bpp = bpp_of[format];
    /* The LCDs are portrait: a framebuffer holds columns of h pixels, bottom to top, one column per screen x. */
    /* the last column ends h pixels after its start, past stride * w when the guest's stride is shorter than a column */
    if (!fb->valid || !fb->stride || !ctr_mem_mapped(m, fb->addr, fb->stride * (uint32_t)(w - 1) + (uint32_t)h * bpp))
        return;
    for (int x = 0; x < w; x++) {
        const uint8_t *col = m->h.mem + fb->addr + fb->stride * (uint32_t)x;
        for (int y = 0; y < h; y++) {
            const uint8_t *p = col + (uint32_t)(h - 1 - y) * bpp;
            uint8_t *o = out + ((size_t)y * (size_t)w + (size_t)x) * 3;
            uint32_t v = bpp == 2 ? (uint32_t)(p[0] | p[1] << 8) : 0;
            switch (format) {
            case 0: /* RGBA8: bytes A B G R */
                o[0] = p[3], o[1] = p[2], o[2] = p[1];
                break;
            case 2: /* RGB565 */
                o[0] = (uint8_t)((v >> 11) << 3 | (v >> 13)), o[1] = (uint8_t)(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
                o[2] = (uint8_t)((v & 31) << 3 | ((v >> 2) & 7));
                break;
            case 3: /* RGB5A1 */
                o[0] = (uint8_t)((v >> 11) << 3 | (v >> 13)), o[1] = (uint8_t)(((v >> 6) & 31) << 3 | ((v >> 8) & 7));
                o[2] = (uint8_t)(((v >> 1) & 31) << 3 | ((v >> 3) & 7));
                break;
            case 4: /* RGBA4 */
                o[0] = (uint8_t)((v >> 12) * 17), o[1] = (uint8_t)(((v >> 8) & 15) * 17);
                o[2] = (uint8_t)(((v >> 4) & 15) * 17);
                break;
            default: /* BGR8: bytes B G R */
                o[0] = p[2], o[1] = p[1], o[2] = p[0];
                break;
            }
        }
    }
    *valid = true;
}

const uint8_t *ctr_screen(const ctr_machine *m, int top, int *w, int *h)
{
    *w = top ? CTR_TOP_W : CTR_BOTTOM_W;
    *h = top ? CTR_TOP_H : CTR_BOTTOM_H;
    if (top)
        return m->top_valid ? m->top_rgb : NULL;
    return m->bottom_valid ? m->bottom_rgb : NULL;
}

/* ---- the frame loop ---- */

int ctr_run_frame(ctr_machine *m)
{
    if (m->error[0])
        return -1;
    if (m->exited)
        return 1;
    m->frame_end += CTR_FRAME_CYCLES;
    while (!m->error[0] && m->h.now < m->frame_end && !m->exited) {
        if (m->resched)
            ctr_schedule(m);
        uint64_t to = ctr_next_timeout(m);
        if (to <= m->h.now) {
            ctr_timeouts(m);
            ctr_schedule(m);
            continue;
        }
        uint64_t ne = to < m->frame_end ? to : m->frame_end;
        if (m->cur < 0) { /* every thread waits: time skips to what comes first */
            m->h.now = ne;
            continue;
        }
        m->h.next_event = ne;
        while (m->h.now < m->h.next_event && !m->resched && m->cur >= 0 && !m->error[0]) {
            if (!ctr_recomp_dispatch(m)) {
                ctr_cpu_step(m);
                m->stats.interp++;
            }
        }
    }
    if (m->error[0])
        return -1;
    ctr_services_vblank(m);
    to_rgb(m, &m->fb[0], CTR_TOP_W, CTR_TOP_H, m->top_rgb, &m->top_valid);
    to_rgb(m, &m->fb[1], CTR_BOTTOM_W, CTR_BOTTOM_H, m->bottom_rgb, &m->bottom_valid);
    m->frames++;
    return m->exited ? 1 : 0;
}
