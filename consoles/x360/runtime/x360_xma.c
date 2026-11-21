/* x360_xma.c -- the XMA decoder's plumbing, the audio the title plays and vibration (XBA-280).
 *
 * XMA contexts (layouts and semantics from Xenia's descriptions -- xma_context.h, xboxkrnl_audio_xma.cc, the APU
 * register table --; no code taken): 320 contexts of 64 bytes in one array of physical memory, which XMACreateContext
 * hands out. A context (big-endian dwords, bit 0 the low bit) says:
 *   dword 0  input buffer 0 packet count (12), loop count (8), input buffer 0 / 1 valid, output buffer block count
 *            (5), output buffer write offset (5, in 256-byte blocks)
 *   dword 1  input buffer 1 packet count (12), loop subframes, subframe decode count, output padding, sample rate (2),
 *            stereo (bit 29), output buffer valid (bit 31)
 *   dword 2  input buffer read offset (26 bits, in bits from the buffer's start)
 *   dword 3 / 4  loop start / end (26 bits each); dword 4 bits 26-30 packet metadata, bit 31 the current buffer
 *   dword 5 / 6 / 7 / 8  input buffer 0 / 1, output buffer, work buffer (physical addresses)
 *   dword 9  output buffer read offset (5 bits, blocks)
 * Titles drive them through the kernel's XMA* exports or straight through the APU registers (0x7FEA0000 + 4 * index:
 * 0x650-0x659 kick, 0x690-0x699 lock, 0x6A0-0x6A9 clear, a bit per context): that page is ordinary memory the
 * scheduler reads between slices, as the GPU's. The APU registers are little-endian (titles use lwbrx / stwbrx):
 * register 0x600 holds the array's physical address, which XMCORE and XAudio2 read when they start to turn a
 * context's address into its hardware index ((context - array) / 64), so the array exists from x360_xma_init on and
 * the register is written again after a state load; in a kick or clear register bit b of the little-endian word is
 * context 32 * row + b. Register 0x606 (the context the hardware is working on) stays 0: XMCORE waits while it reads
 * a live context's index ^ 0x200. The lock registers need nothing.
 *
 * Decoding works a 2048-byte packet at a time (an XMA2 packet: a 32-bit header -- frame count (6), first frame's bit
 * offset (15), metadata (3), packets to skip after this one (8) -- then the bit-packed frames, 512 samples per channel
 * each): the input buffer the context's current-buffer bit names is read from its read offset, each packet goes to
 * the frame backend, the PCM it gives (16-bit, big-endian, interleaved when stereo) fills the output ring's free
 * blocks (between the write offset and the read offset the title moves), a buffer read to its end loses its valid
 * bit and the other one is current. The backend: libavcodec where the build found it (x360_xma_av.c, XBA-299;
 * RECOMP_X360_XMA=silence turns it off), else each frame is 512 samples of silence -- the buffers keep flowing as the
 * title expects --; a host may register another decoder (x360_xma_set_backend). A loop (loop count in dword 0, start
 * and end in bits) goes back at packet granularity: the packet holding the loop end is played whole, then the one
 * holding the loop start, from a fresh decoder. The sub-packet start and end bits are not followed.
 *
 * The audio out: what render driver clients submit (256-sample frames, 6 channels of float: front left / right,
 * centre, LFE, surround left / right) is mixed (several clients summed) and folded down to 48 kHz 16-bit stereo in a
 * ring the host pulls (x360_audio_pull). Vibration: XamInputSetState's motors (x360_pad_motors). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "x360_internal.h"

#define A(n) ((uint32_t)c->gpr[3 + (n)])
#define RET(v) (c->gpr[3] = (uint64_t)(uint32_t)(v))
#define X_STATUS_NO_MEMORY 0xC0000017u

#define APU_MMIO 0x7FEA0000u
#define XMA_CONTEXTS 320
#define XMA_CONTEXT_BYTES 64
#define XMA_PACKET 2048u
#define XMA_FRAME_SAMPLES 512
#define MIX_RING 16384 /* stereo frames */

typedef struct x360_xma {
    uint32_t array;                   /* physical address of the context array (0: not made yet) */
    uint8_t used[XMA_CONTEXTS];
    uint8_t enabled[XMA_CONTEXTS];
    int live_end;                     /* WP-32: one past the last context in use and enabled (0: none; not state) */
    void *dec[XMA_CONTEXTS];          /* the backend's decoder per context (not state) */
    int16_t *pending[XMA_CONTEXTS];   /* PCM decoded but not yet written to the output ring (not state) */
    uint32_t npending[XMA_CONTEXTS];  /* in samples (all channels) */
    uint8_t starved[XMA_CONTEXTS];    /* polls since the input ran out with a decoder open (not state) */
    uint64_t packets, frames, blocks_out, kicks;
    /* the mix of the render driver clients' frames, then the ring the host pulls */
    float acc[256 * 2];
    uint8_t acc_clients;
    int16_t ring[MIX_RING * 2];
    uint32_t ring_r, ring_w, dropped;
    uint8_t motor[4][2];              /* XamInputSetState's left / right motor speeds (0-255), per user */
} x360_xma;

/* ---- the frame backend ---- */

static void *silence_open(int channels, int rate)
{
    (void)rate;
    return (void *)(uintptr_t)(channels > 1 ? 2 : 1);
}

static int silence_decode(void *h, const uint8_t *packet, int16_t *out, int max_samples)
{
    int ch = (int)(uintptr_t)h, frames = packet[0] >> 2; /* the header's frame count */
    int n = frames * XMA_FRAME_SAMPLES * ch;
    if (n > max_samples)
        n = max_samples;
    memset(out, 0, (size_t)n * 2);
    return n;
}

static void silence_close(void *h)
{
    (void)h;
}

static const x360_xma_backend k_silence = {"silence", silence_open, silence_decode, silence_close, NULL};
#ifdef X360_HAVE_AVCODEC
extern const x360_xma_backend x360_xma_av_backend;
static const x360_xma_backend *s_backend = &x360_xma_av_backend;
#else
static const x360_xma_backend *s_backend = &k_silence;
#endif

void x360_xma_set_backend(const x360_xma_backend *b)
{
    s_backend = b ? b : &k_silence;
}

const char *x360_xma_backend_name(void)
{
    return s_backend->name;
}

/* ---- contexts ---- */

static x360_xma *xma_of(x360_machine *x)
{
    if (!x->xma)
        x->xma = calloc(1, sizeof(x360_xma));
    return x->xma;
}

/* the context array, made once, and register 0x600 naming it */
static int array_publish(x360_machine *x)
{
    x360_xma *m = xma_of(x);
    if (!m)
        return -1;
    if (!m->array)
        m->array = x360_heap_alloc(x, &x->heap_phys, XMA_CONTEXTS * XMA_CONTEXT_BYTES, 0);
    if (!m->array)
        return -1;
    x360_w32(x, APU_MMIO + 4 * 0x600u, __builtin_bswap32(m->array));
    return 0;
}

int x360_xma_init(x360_machine *x)
{
    const char *env = getenv("RECOMP_X360_XMA");
    if (env && !strcmp(env, "silence"))
        s_backend = &k_silence;
    return xma_of(x) && !x360_commit(x, APU_MMIO, 0x10000) ? array_publish(x) : -1;
}

void x360_xma_free(x360_machine *x)
{
    x360_xma *m = x->xma;
    if (!m)
        return;
    for (int i = 0; i < XMA_CONTEXTS; i++) {
        if (m->dec[i])
            s_backend->close(m->dec[i]);
        free(m->pending[i]);
    }
    free(m);
    x->xma = NULL;
}

static int ctx_index(x360_machine *x, uint32_t guest)
{
    x360_xma *m = xma_of(x);
    uint32_t p = x360_v2p(guest);
    if (!m || !m->array || p < m->array || p >= m->array + XMA_CONTEXTS * XMA_CONTEXT_BYTES)
        return -1;
    return (int)((p - m->array) / XMA_CONTEXT_BYTES);
}

static uint32_t ctx_addr(x360_machine *x, int i)
{
    return x360_p2v(X360_VIEW_64K, x->xma->array) + (uint32_t)i * XMA_CONTEXT_BYTES;
}

static uint32_t rd(x360_machine *x, int i, int dw)
{
    return x360_r32(x, ctx_addr(x, i) + 4 * (uint32_t)dw);
}

static void wr(x360_machine *x, int i, int dw, uint32_t v)
{
    x360_w32(x, ctx_addr(x, i) + 4 * (uint32_t)dw, v);
}

static uint32_t field(uint32_t v, int at, int n)
{
    return (v >> at) & ((1u << n) - 1);
}

static uint32_t with(uint32_t v, int at, int n, uint32_t f)
{
    uint32_t m = ((1u << n) - 1) << at;
    return (v & ~m) | ((f << at) & m);
}

static void drop_decoder(x360_xma *m, int i)
{
    if (m->dec[i])
        s_backend->close(m->dec[i]);
    m->dec[i] = NULL;
    m->npending[i] = 0;
}

static const int k_rates[4] = {24000, 32000, 44100, 48000}; /* the context's sample rate field */
#define XMA_MAX_PCM (64 * XMA_FRAME_SAMPLES * 2)
#define XMA_STARVED_POLLS 8 /* a title refilling its buffers in time is not an end of stream */

/* n samples to the context's pending PCM, big-endian as the title reads them */
static void pending_add(x360_xma *m, int i, const int16_t *pcm, int n)
{
    if (n <= 0)
        return;
    int16_t *p = realloc(m->pending[i], ((size_t)m->npending[i] + (size_t)n) * 2);
    if (!p)
        return;
    for (int k = 0; k < n; k++)
        p[m->npending[i] + (uint32_t)k] = (int16_t)(((uint16_t)pcm[k] >> 8) | ((uint16_t)pcm[k] << 8));
    m->pending[i] = p;
    m->npending[i] += (uint32_t)n;
}

/* the end of a run of packets: what the decoder still holds goes out, and the next packet starts a new decoder */
static void decoder_finish(x360_xma *m, int i)
{
    if (!m->dec[i])
        return;
    int16_t *pcm = s_backend->drain ? malloc(XMA_MAX_PCM * 2) : NULL;
    if (pcm)
        pending_add(m, i, pcm, s_backend->drain(m->dec[i], pcm, XMA_MAX_PCM));
    free(pcm);
    s_backend->close(m->dec[i]);
    m->dec[i] = NULL;
    m->starved[i] = 0;
}

/* WP-32: live_end again, after a change of used / enabled (rare: the poll walks contexts up to it every turn) */
static void live_recount(x360_xma *m)
{
    int e = 0;
    for (int i = 0; i < XMA_CONTEXTS; i++)
        if (m->used[i] && m->enabled[i])
            e = i + 1;
    m->live_end = e;
}

/* decode what context i can: packets of its current input buffer while the output ring has room */
static void xma_work(x360_machine *x, int i)
{
    x360_xma *m = x->xma;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t d0 = rd(x, i, 0), d1 = rd(x, i, 1);
        if (!field(d1, 31, 1))
            return; /* no output buffer */
        uint32_t blocks = field(d0, 22, 5), wo = field(d0, 27, 5), ro = field(rd(x, i, 9), 0, 5);
        if (!blocks)
            return;
        int stereo = field(d1, 29, 1);
        /* the pending PCM first: whole 256-byte blocks into the ring's free space */
        while (m->npending[i]) {
            uint32_t free_blocks = (ro + blocks - wo - 1) % blocks;
            if (!free_blocks)
                return;
            uint32_t out = rd(x, i, 7);
            uint32_t n = m->npending[i] < 128 ? m->npending[i] : 128; /* 128 samples = one block */
            uint32_t at = x360_p2v(X360_VIEW_64K, out & 0x1FFFFFFFu) + wo * 256;
            for (uint32_t k = 0; k < 128; k++)
                x360_w16(x, at + 2 * k, k < n ? (uint16_t)m->pending[i][k] : 0);
            memmove(m->pending[i], m->pending[i] + n, (m->npending[i] - n) * 2);
            m->npending[i] -= n;
            wo = (wo + 1) % blocks;
            d0 = with(rd(x, i, 0), 27, 5, wo);
            wr(x, i, 0, d0);
            m->blocks_out++;
        }
        /* the next packet of the current input buffer */
        uint32_t d4 = rd(x, i, 4), cur = field(d4, 31, 1);
        int valid_bit = cur ? 21 : 20;
        if (!field(d0, valid_bit, 1)) {
            if (field(d0, cur ? 20 : 21, 1)) { /* the other buffer is ready: it is current now */
                wr(x, i, 4, with(d4, 31, 1, !cur));
                wr(x, i, 2, with(rd(x, i, 2), 0, 26, 0));
                continue;
            }
            if (m->dec[i] && ++m->starved[i] >= XMA_STARVED_POLLS) {
                decoder_finish(m, i);
                continue; /* its last samples to the ring */
            }
            return;
        }
        m->starved[i] = 0;
        uint32_t base = rd(x, i, cur ? 6 : 5) & 0x1FFFFFFFu;
        uint32_t count = field(cur ? d1 : d0, 0, 12);
        uint32_t off = field(rd(x, i, 2), 0, 26), pkt = off / (XMA_PACKET * 8);
        if (pkt >= count) { /* read to its end: no longer valid, the other buffer next */
            wr(x, i, 0, with(d0, valid_bit, 1, 0));
            wr(x, i, 4, with(rd(x, i, 4), 31, 1, !cur));
            wr(x, i, 2, with(rd(x, i, 2), 0, 26, 0));
            continue;
        }
        uint8_t packet[XMA_PACKET];
        uint32_t src = x360_p2v(X360_VIEW_64K, base) + pkt * XMA_PACKET;
        for (uint32_t k = 0; k < XMA_PACKET; k++)
            packet[k] = x360_r8(x, src + k);
        if (!m->dec[i])
            m->dec[i] = s_backend->open(stereo ? 2 : 1, k_rates[field(d1, 27, 2)]);
        int16_t *pcm = malloc(XMA_MAX_PCM * 2);
        if (pcm)
            pending_add(m, i, pcm, s_backend->decode(m->dec[i], packet, pcm, XMA_MAX_PCM));
        free(pcm);
        m->packets++;
        m->frames += packet[0] >> 2;
        /* the next packet: after this one's skip count; metadata kept for XMAGetPacketMetadata */
        uint32_t skip = packet[3];
        uint32_t next = (pkt + 1 + (skip == 0xFF ? 0 : skip)) * XMA_PACKET * 8;
        /* a loop: after the packet holding its last bit, back to the packet holding its first */
        uint32_t loops = field(rd(x, i, 0), 12, 8), first = field(rd(x, i, 3), 0, 26), end = field(rd(x, i, 4), 0, 26);
        if (loops && end > first && pkt >= (end - 1) / (XMA_PACKET * 8)) {
            next = first / (XMA_PACKET * 8) * XMA_PACKET * 8;
            if (loops != 255) /* 255: for ever */
                wr(x, i, 0, with(rd(x, i, 0), 12, 8, loops - 1));
            decoder_finish(m, i); /* the frames after the jump do not follow the ones before it */
        }
        wr(x, i, 2, with(rd(x, i, 2), 0, 26, next));
        wr(x, i, 4, with(rd(x, i, 4), 26, 5, field((uint32_t)packet[2], 0, 3)));
    }
}

void x360_xma_poll(x360_machine *x)
{
    x360_xma *m = x->xma;
    if (!m || !m->array)
        return;
    /* the APU registers: kicks enable contexts, clears reset them. Nearly every turn all 20 words are 0: their OR
     * first, without a branch per word (WP-32's per-word test then runs only when one is set) */
    uint32_t any = 0;
    for (int r = 0; r < 10; r++)
        any |= x360_r32(x, APU_MMIO + 4 * (0x650u + (uint32_t)r)) | x360_r32(x, APU_MMIO + 4 * (0x6A0u + (uint32_t)r));
    int changed = 0;
    for (int r = 0; any && r < 10; r++) { /* little-endian, as the title's stwbrx wrote them */
        uint32_t kick = __builtin_bswap32(x360_r32(x, APU_MMIO + 4 * (0x650u + (uint32_t)r)));
        uint32_t clear = __builtin_bswap32(x360_r32(x, APU_MMIO + 4 * (0x6A0u + (uint32_t)r)));
        if (!(kick | clear))
            continue; /* WP-32: nearly every word of nearly every turn */
        changed = 1;
        for (int b = 0; b < 32; b++) {
            int i = r * 32 + b;
            if ((kick >> b) & 1)
                m->enabled[i] = 1, m->kicks++;
            if ((clear >> b) & 1) {
                m->enabled[i] = 0;
                drop_decoder(m, i);
            }
        }
        if (kick)
            x360_w32(x, APU_MMIO + 4 * (0x650u + (uint32_t)r), 0);
        if (clear)
            x360_w32(x, APU_MMIO + 4 * (0x6A0u + (uint32_t)r), 0);
    }
    if (changed)
        live_recount(m);
    for (int i = 0; i < m->live_end; i++)
        if (m->used[i] && m->enabled[i])
            xma_work(x, i);
}

/* ---- the kernel's XMA exports ---- */

static void k_XMACreateContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    x360_xma *m = xma_of(x);
    if (m && !m->array)
        array_publish(x);
    int i = 0;
    while (m && m->array && i < XMA_CONTEXTS && m->used[i])
        i++;
    if (!m || !m->array || i == XMA_CONTEXTS) {
        if (A(0))
            x360_w32(x, A(0), 0);
        RET(X_STATUS_NO_MEMORY);
        return;
    }
    m->used[i] = 1, m->enabled[i] = 0;
    live_recount(m);
    for (int dw = 0; dw < 16; dw++)
        wr(x, i, dw, 0);
    if (A(0))
        x360_w32(x, A(0), ctx_addr(x, i));
    RET(0);
}

static void k_XMAReleaseContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    if (i >= 0) {
        x->xma->used[i] = 0, x->xma->enabled[i] = 0;
        live_recount(x->xma);
        drop_decoder(x->xma, i);
    }
    RET(0);
}

/* XMA_CONTEXT_INIT (Xenia's description): input buffer 0 address and packet count, input buffer 1 address and
 * packet count, the read offset, the output buffer address and block count, the work buffer, the subframe decode
 * count, channel count, sample rate, then the loop data (start, end, count / subframe end / subframe skip bytes) */
static void k_XMAInitializeContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    uint32_t p = A(1);
    if (i < 0 || !p) {
        RET(0);
        return;
    }
    for (int dw = 0; dw < 16; dw++)
        wr(x, i, dw, 0);
    uint32_t in0 = x360_r32(x, p), n0 = x360_r32(x, p + 4), in1 = x360_r32(x, p + 8), n1 = x360_r32(x, p + 12);
    uint32_t roff = x360_r32(x, p + 16), out = x360_r32(x, p + 20), blocks = x360_r32(x, p + 24);
    uint32_t work = x360_r32(x, p + 28), subframes = x360_r32(x, p + 32), channels = x360_r32(x, p + 36);
    uint32_t rate = x360_r32(x, p + 40), loop_start = x360_r32(x, p + 44), loop_end = x360_r32(x, p + 48);
    uint32_t d0 = with(0, 0, 12, n0), d1 = with(0, 0, 12, n1);
    d0 = with(d0, 22, 5, blocks);
    d1 = with(d1, 20, 4, subframes);
    d1 = with(d1, 27, 2, rate);
    d1 = with(d1, 29, 1, channels > 1);
    d0 = with(d0, 12, 8, x360_r8(x, p + 52));
    wr(x, i, 0, d0);
    wr(x, i, 1, d1);
    wr(x, i, 2, roff & 0x3FFFFFF);
    wr(x, i, 3, loop_start & 0x3FFFFFF);
    wr(x, i, 4, loop_end & 0x3FFFFFF);
    wr(x, i, 5, x360_v2p(in0));
    wr(x, i, 6, x360_v2p(in1));
    wr(x, i, 7, x360_v2p(out));
    wr(x, i, 8, x360_v2p(work));
    drop_decoder(x->xma, i);
    RET(0);
}

static void k_XMAEnableContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    if (i >= 0) {
        x->xma->enabled[i] = 1;
        x->xma->kicks++;
        live_recount(x->xma);
        xma_work(x, i);
    }
    RET(0);
}

static void k_XMADisableContext(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    if (i >= 0) {
        x->xma->enabled[i] = 0;
        live_recount(x->xma);
    }
    RET(0);
}

/* the getters and setters of one field: (dword, bit, width) */
#define XMA_GET(name, dw, at, n)                                                                                       \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        int i = ctx_index(x, A(0));                                                                                    \
        RET(i >= 0 ? field(rd(x, i, dw), at, n) : 0);                                                                  \
    }
#define XMA_SET(name, dw, at, n, value)                                                                                \
    static void k_##name(x360_machine *x, x360_thread *t, xe_cpu *c)                                                   \
    {                                                                                                                  \
        (void)t;                                                                                                       \
        int i = ctx_index(x, A(0));                                                                                    \
        if (i >= 0)                                                                                                    \
            wr(x, i, dw, with(rd(x, i, dw), at, n, (value)));                                                          \
        RET(0);                                                                                                        \
    }
XMA_GET(XMAGetOutputBufferWriteOffset, 0, 27, 5)
XMA_GET(XMAGetOutputBufferReadOffset, 9, 0, 5)
XMA_SET(XMASetOutputBufferReadOffset, 9, 0, 5, A(1))
XMA_SET(XMASetOutputBufferValid, 1, 31, 1, 1)
XMA_GET(XMAIsOutputBufferValid, 1, 31, 1)
XMA_SET(XMASetInputBuffer0Valid, 0, 20, 1, 1)
XMA_GET(XMAIsInputBuffer0Valid, 0, 20, 1)
XMA_SET(XMASetInputBuffer1Valid, 0, 21, 1, 1)
XMA_GET(XMAIsInputBuffer1Valid, 0, 21, 1)
XMA_GET(XMAGetPacketMetadata, 4, 26, 5)
XMA_SET(XMASetInputBufferReadOffset, 2, 0, 26, A(1))
XMA_GET(XMAGetInputBufferReadOffset, 2, 0, 26)

static void k_XMASetInputBuffer0(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    if (i >= 0) {
        wr(x, i, 5, x360_v2p(A(1)));
        wr(x, i, 0, with(rd(x, i, 0), 0, 12, A(2)));
    }
    RET(0);
}

static void k_XMASetInputBuffer1(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    if (i >= 0) {
        wr(x, i, 6, x360_v2p(A(1)));
        wr(x, i, 1, with(rd(x, i, 1), 0, 12, A(2)));
    }
    RET(0);
}

static void k_XMASetLoopData(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)t;
    int i = ctx_index(x, A(0));
    uint32_t p = A(1);
    if (i >= 0 && p) {
        wr(x, i, 3, x360_r32(x, p) & 0x3FFFFFF);
        wr(x, i, 4, (rd(x, i, 4) & ~0x3FFFFFFu) | (x360_r32(x, p + 4) & 0x3FFFFFF));
        wr(x, i, 0, with(rd(x, i, 0), 12, 8, x360_r8(x, p + 8)));
    }
    RET(0);
}

static void k_XMABlockWhileInUse(x360_machine *x, x360_thread *t, xe_cpu *c)
{
    (void)x, (void)t;
    RET(0); /* decoding happens between slices, never under the title's feet */
}

static const x360_hle_row XMA[] = {
#define E(name) X360_HK(name, k_##name)
    E(XMACreateContext), E(XMAReleaseContext), E(XMAInitializeContext), E(XMAEnableContext), E(XMADisableContext),
    E(XMAGetOutputBufferWriteOffset), E(XMAGetOutputBufferReadOffset), E(XMASetOutputBufferReadOffset),
    E(XMASetOutputBufferValid), E(XMAIsOutputBufferValid), E(XMASetInputBuffer0Valid), E(XMAIsInputBuffer0Valid),
    E(XMASetInputBuffer1Valid), E(XMAIsInputBuffer1Valid), E(XMASetInputBuffer0), E(XMASetInputBuffer1),
    E(XMAGetPacketMetadata), E(XMABlockWhileInUse), E(XMASetLoopData), E(XMASetInputBufferReadOffset),
    E(XMAGetInputBufferReadOffset),
#undef E
};

void x360_xma_register(x360_machine *x)
{
    x360_hle_add(x, XMA, sizeof XMA / sizeof XMA[0]);
}

void x360_xma_get_stats(x360_machine *x, uint64_t *packets, uint64_t *frames, uint64_t *blocks, unsigned *contexts)
{
    x360_xma *m = x->xma;
    unsigned n = 0;
    for (int i = 0; m && i < XMA_CONTEXTS; i++)
        n += m->used[i];
    *packets = m ? m->packets : 0, *frames = m ? m->frames : 0, *blocks = m ? m->blocks_out : 0, *contexts = n;
}

/* state: the context array's place, which contexts are in use and enabled (the contexts themselves are guest
 * memory, in MEM); decoders and pending PCM start again empty after a load. x360_xma_loaded, once every section is
 * in: a state made before the array existed from the start (none, or memory without register 0x600) gets it
 * again. */
void x360_xma_state(x360_machine *x, uint32_t *array, uint8_t *used, uint8_t *enabled, int load)
{
    x360_xma *m = xma_of(x);
    if (!m)
        return;
    if (!load) {
        *array = m->array;
        memcpy(used, m->used, XMA_CONTEXTS);
        memcpy(enabled, m->enabled, XMA_CONTEXTS);
        return;
    }
    m->array = *array;
    for (int i = 0; i < XMA_CONTEXTS; i++) {
        drop_decoder(m, i);
        m->used[i] = used[i] & 1, m->enabled[i] = enabled[i] & 1;
    }
    live_recount(m);
    m->ring_r = m->ring_w = 0;
    memset(m->acc, 0, sizeof m->acc); /* the frame being mixed starts again empty */
    m->acc_clients = 0;
}

void x360_xma_loaded(x360_machine *x)
{
    x360_xma *m = xma_of(x);
    if (!m)
        return;
    x360_heap *h = &x->heap_phys;
    if (m->array && (m->array >= h->base + h->size || !h->used[(m->array - h->base) / h->page]))
        m->array = 0; /* not the state's memory (a state without an XMA section): another one */
    array_publish(x);
}

/* ---- the audio out: the render driver's frames mixed, folded to stereo, in a ring ---- */

static void mix_flush(x360_xma *m)
{
    if (!m->acc_clients)
        return;
    for (int s = 0; s < 256; s++) {
        if (((m->ring_w + 1) % MIX_RING) == m->ring_r) { /* the host does not pull: the oldest frame goes */
            m->ring_r = (m->ring_r + 1) % MIX_RING;
            m->dropped++;
        }
        for (int ch = 0; ch < 2; ch++) {
            float v = m->acc[2 * s + ch] * 32767.0f;
            m->ring[2 * m->ring_w + (uint32_t)ch] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
        }
        m->ring_w = (m->ring_w + 1) % MIX_RING;
    }
    memset(m->acc, 0, sizeof m->acc);
    m->acc_clients = 0;
}

void x360_audio_mix_frame(x360_machine *x, int client, const float *planar6)
{
    x360_xma *m = xma_of(x);
    if (!m)
        return;
    if (m->acc_clients & (1u << (client & 7)))
        mix_flush(m); /* this client's next frame: the last one is complete */
    const float k = 0.70710678f;
    for (int s = 0; s < 256; s++) {
        float fl = planar6[s], fr = planar6[256 + s], cc = planar6[512 + s], sl = planar6[1024 + s],
              sr = planar6[1280 + s];
        m->acc[2 * s] += fl + k * cc + k * sl;
        m->acc[2 * s + 1] += fr + k * cc + k * sr;
    }
    m->acc_clients |= (uint8_t)(1u << (client & 7));
}

void x360_audio_tick(x360_machine *x)
{
    if (x->xma)
        mix_flush(x->xma); /* a new render driver frame starts: the last one is complete */
}

size_t x360_audio_pull(x360_machine *x, int16_t *out, size_t frames)
{
    x360_xma *m = x->xma;
    size_t n = 0;
    if (!m)
        return 0;
    while (n < frames && m->ring_r != m->ring_w) {
        out[2 * n] = m->ring[2 * m->ring_r];
        out[2 * n + 1] = m->ring[2 * m->ring_r + 1];
        m->ring_r = (m->ring_r + 1) % MIX_RING;
        n++;
    }
    return n;
}

/* ---- vibration ---- */

void x360_pad_set_motors(x360_machine *x, int user, unsigned left, unsigned right)
{
    x360_xma *m = xma_of(x);
    if (m && user >= 0 && user < 4)
        m->motor[user][0] = (uint8_t)(left > 255 ? 255 : left), m->motor[user][1] = (uint8_t)(right > 255 ? 255 : right);
}

void x360_pad_motors(x360_machine *x, int user, unsigned *left, unsigned *right)
{
    x360_xma *m = x->xma;
    *left = m && user >= 0 && user < 4 ? m->motor[user][0] : 0;
    *right = m && user >= 0 && user < 4 ? m->motor[user][1] : 0;
}
