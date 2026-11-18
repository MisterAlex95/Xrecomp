/* gx_ring.h -- the bookkeeping of a streaming ring whose memory the GPU reads while the CPU writes further on
 * (RECOMP_GC_GX_STREAM=storage, gx_gl.c). No GL in here: fences go through gx_ring_ops, so that gx_ring_test can run it
 * against a simulated GPU.
 *
 * Positions (head, tail, fenced) count bytes since the start and never wrap; a position's place in the buffer is
 * position % size. The bytes in [tail, head) may still be read by the GPU, and a write must stay out of them:
 *
 *      tail            fenced             head
 *       | fences queued  | no fence yet    | free ...
 *
 * A reservation that does not fit before the end of the buffer skips the rest of it, and the skipped bytes count as
 * written. It may proceed once end - tail <= size; until then the oldest fences are waited for, in order, each one
 * moving tail to the end of the region it covers. That one comparison covers wraps too.
 *
 * A fence covers what was written before it only if the draws reading those bytes were issued before it. The caller
 * therefore reserves at most once per draw in each ring and calls gx_ring_after_draw once the draw is issued: at any
 * reservation, all the ring's earlier bytes belong to draws already issued. */
#ifndef GX_RING_H
#define GX_RING_H

#include <stddef.h>
#include <stdint.h>

#define GX_RING_FENCES 64u /* regions in flight at most */
#define GX_RING_SLICES 16u /* a fence each time this fraction of the ring was written */

typedef struct gx_ring_ops {
    void *(*fence)(void *user);               /* a fence after every command issued so far; NULL: they are complete */
    void (*retire)(void *user, void *sync);   /* blocks until sync has signalled, then deletes it (NULL: nothing) */
    void (*discard)(void *user, void *sync);  /* deletes sync without waiting (NULL: nothing) */
} gx_ring_ops;

typedef struct gx_ring {
    uint64_t size, head, tail, fenced;
    uint64_t wraps;
    struct {
        void *sync;
        uint64_t end; /* the region's end: tail moves here once sync has signalled */
    } q[GX_RING_FENCES];
    unsigned q_first, q_n;
} gx_ring;

static inline void gx_ring_init(gx_ring *r, uint64_t size)
{
    *r = (gx_ring){.size = size};
}

/* Whether n bytes may be reserved. A quarter of the ring at most, so that the bytes skipped at a wrap plus the
 * reservation always fit an idle ring and one upload never has to wait for the whole ring. */
static inline int gx_ring_fits(const gx_ring *r, uint64_t n)
{
    return n <= r->size / 4;
}

static inline void gx_ring_retire_oldest(gx_ring *r, const gx_ring_ops *ops, void *user)
{
    ops->retire(user, r->q[r->q_first].sync);
    r->tail = r->q[r->q_first].end;
    r->q_first = (r->q_first + 1) % GX_RING_FENCES;
    r->q_n--;
}

/* Puts a fence over everything written and not fenced yet. */
static inline void gx_ring_close(gx_ring *r, const gx_ring_ops *ops, void *user)
{
    if (r->fenced == r->head)
        return;
    if (r->q_n == GX_RING_FENCES)
        gx_ring_retire_oldest(r, ops, user);
    unsigned slot = (r->q_first + r->q_n) % GX_RING_FENCES;
    r->q[slot].sync = ops->fence(user);
    r->q[slot].end = r->head;
    r->q_n++;
    r->fenced = r->head;
}

/* The buffer offset of n free bytes aligned to align, waiting for the GPU when the ring is full. n: gx_ring_fits. */
static inline uint64_t gx_ring_reserve(gx_ring *r, const gx_ring_ops *ops, void *user, uint64_t n, uint64_t align)
{
    uint64_t off = r->head % r->size;
    uint64_t at = (off + align - 1) / align * align;
    uint64_t start = r->head + (at - off);
    if (at + n > r->size) {
        start = r->head + (r->size - off);
        at = 0;
        r->wraps++;
    }
    uint64_t end = start + n;
    while (end - r->tail > r->size) {
        if (!r->q_n)
            gx_ring_close(r, ops, user);
        if (!r->q_n)
            break; /* nothing in flight: unreachable for a size that fits */
        gx_ring_retire_oldest(r, ops, user);
    }
    r->head = end;
    return at;
}

/* After a draw reading this ring's latest reservation was issued. */
static inline void gx_ring_after_draw(gx_ring *r, const gx_ring_ops *ops, void *user)
{
    if (r->head - r->fenced >= r->size / GX_RING_SLICES)
        gx_ring_close(r, ops, user);
}

/* Deletes the fences without waiting; the ring is empty afterwards. */
static inline void gx_ring_release(gx_ring *r, const gx_ring_ops *ops, void *user)
{
    for (; r->q_n; r->q_n--, r->q_first = (r->q_first + 1) % GX_RING_FENCES)
        ops->discard(user, r->q[r->q_first].sync);
    r->head = r->tail = r->fenced = 0;
}

#endif
