/* x360_core.h -- the Xbox 360 runtime (XBA-270, epic XBA-229): an XEX2 title, statically recompiled, run headless
 * on Linux up to and past its main(): a 4 GB guest address space with the physical mirrors, the XEX loader and its
 * import thunks, cooperative guest threads, and a minimal xboxkrnl / xam HLE (debug output, memory, threads,
 * events / semaphores / mutants / timers, critical sections, TLS). No rendering (Xenos), no audio, no input yet.
 *
 * The title's code comes from a pack: the C x360_tools.recomp writes, built as a shared library with -DXE_RUNTIME
 * (xe_rc_resume: every resume point -> lifted function, and the CRC of the image it was made from). Instructions
 * the lifter hands to RC_INTERP run in a small interpreter (x360_interp.c: traps, FPSCR moves). See
 * consoles/README.md « Xbox 360 ». */
#ifndef X360_CORE_H
#define X360_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x360_machine x360_machine;

typedef struct x360_pack {
    const void *resume; /* const xe_rc_entry[] sorted by address */
    unsigned count;
    uint32_t image_crc;
    const uint32_t *helpers; /* XBA-283: the register save / restore helpers' entries (xe_rc_helpers), sorted, or NULL:
                              * interpreted code calls them, they are not functions to lift */
    unsigned nhelpers;
    unsigned features;       /* xe_rc_features (XBA-283; bit 0: lwarx / stwcx. atomic), 0 for older packs */
    const void *parts;       /* XBA-284: const xe_rc_part[] (xe_rc_resume_parts) in place of resume, or NULL */
    unsigned nparts;
} x360_pack;

enum {
    X360_RUNNING = 0,
    X360_EXITED = 1,   /* the title ended (main thread returned, ExTerminateThread of the last thread, ...) */
    X360_ERROR = 2,    /* see x360_error() */
    X360_BUDGET = 3,   /* the instruction budget ran out */
    X360_MISSED = 4,   /* execution reached an address the pack does not cover (x360_missed()) */
    X360_FRAME = 5,    /* x360_run_until: the virtual time asked for was reached (the machine goes on running) */
};

/* Load an XEX2 (unencrypted, compression none or basic: python3 -m tools.xex FILE --plain OUT makes one from an
 * encrypted / LZX title with the user's key). NULL on error (why says it). */
x360_machine *x360_create(const uint8_t *xex, size_t size, char *why, size_t why_len);
void x360_destroy(x360_machine *x);
/* Bind the lifted code; 0 on success, -1 when the pack was made from another image (stale). */
int x360_bind(x360_machine *x, const x360_pack *pack);
/* The pack's tables from a dlopen'ed library: xe_rc_resume or its parts, the CRC, the helpers, the features. 0 when
 * the library is an x360_tools.recomp pack. */
int x360_pack_from_lib(void *lib, x360_pack *pack);
/* The disc: a disc image (XDVDFS / GDFX, also split or CSO / ZSO / CCI, read in place by src/xiso) or a directory of
 * its files; the data directory (cache:, hdd:...: writable), optional. 0 on success. */
int x360_set_disc(x360_machine *x, const char *path, char *why, size_t why_len);
int x360_set_data_dir(x360_machine *x, const char *path);
/* Stop a run (X360_BUDGET) once this much virtual time has passed since the title started, 0 = never: a title waiting
 * forever on timers or vertical blanks (no input, no rendering yet) would otherwise skip time without end. */
void x360_set_time_limit(x360_machine *x, uint32_t seconds);
/* Stop at the first call of an import without an HLE (default: log it once, return 0). */
void x360_set_strict_imports(x360_machine *x, int on);
/* Write each distinct shader microcode the title loads to DIR (vs_/ps_<crc>.bin, big-endian words: python3 -m
 * x360_tools.xenos_ucode disassembles them); the type-3 packet counts of the GPU stream. */
void x360_set_shader_dump(x360_machine *x, const char *dir);
void x360_gpu_opcode_histogram(x360_machine *x, FILE *out);
/* Draw with the GLES 3.0 renderer (xenos_gl.c): shaders from SHADER_CACHE (GLSL ES by x360_tools.xenos_glsl, named
 * by microcode CRC), a width x height target, the first max_frames swaps written to FRAMES_DIR as PPM (NULL: none).
 * -1 with a reason when EGL / GLES cannot be had (the run goes on without drawing). x360_render_stats: one line,
 * nothing when off. */
int x360_set_renderer(x360_machine *x, const char *shader_cache, const char *frames_dir, unsigned max_frames,
                      int width, int height, char *why, size_t why_n);
void x360_render_stats(x360_machine *x, FILE *out);
/* The same with an internal resolution (scale 1-8: every eDRAM surface scale x its guest size) and, attach = 1, on
 * the caller's current GLES 3 context (a window: no EGL of its own, no frame read back unless frames_dir is set or
 * x360_render_set_readback asks). XBA-280. */
int x360_set_renderer_ex(x360_machine *x, const char *shader_cache, const char *frames_dir, unsigned max_frames,
                         int width, int height, int scale, int attach, char *why, size_t why_n);
/* The last frame as a GL texture (*width x *height host pixels, top row first, the front buffer's swizzle set on it),
 * 0 when there is none. */
unsigned x360_render_texture(x360_machine *x, int *width, int *height);
void x360_render_set_readback(x360_machine *x, int on);
int x360_render_scale(x360_machine *x);
/* after the frames a window shows ran: its context's state back as the window's drawing expects (attach mode) */
void x360_render_end_frame(x360_machine *x);
/* After a state load (x360_state_load calls it): the eDRAM is not in states, surfaces start clear. */
void x360_render_reset(x360_machine *x);
/* The renderer's last frame (RGBA, top row first) and its counters, or NULL / zeros when it is off. */
const uint8_t *x360_render_frame(x360_machine *x, int *width, int *height);
void x360_render_counts(x360_machine *x, uint64_t *drawn, uint64_t *frames, uint64_t *front_buffer_frames);
/* The audio the title submits to the render driver (256-sample frames of 6 channels at 48 kHz), written to F as a
 * 32-bit float WAV, sizes filled in when the machine is freed. XMA is not decoded: XMA voices stay silent. */
void x360_set_audio_out(x360_machine *x, FILE *f);
/* The audio the title plays (XBA-280): the render driver clients' frames mixed and folded down to 48 kHz 16-bit
 * stereo (front + 0.707 x (centre + surround) per side), up to `frames` taken from the ring (interleaved L, R);
 * returns the frames given. About a third of a second is kept; older frames are dropped when nothing pulls. */
size_t x360_audio_pull(x360_machine *x, int16_t *out, size_t frames);
/* XMA (x360_xma.c): the decoder of XMA2 packets behind the contexts. The default backend is libavcodec where the build
 * found it (x360_xma_av.c), else each frame is 512 samples of silence; a host may register another for every machine.
 * decode: one 2048-byte packet (header included) to at most max_samples 16-bit samples (interleaved when the stream
 * is stereo); returns the samples written. */
typedef struct x360_xma_backend {
    const char *name;
    void *(*open)(int channels, int sample_rate);
    int (*decode)(void *dec, const uint8_t *packet, int16_t *out, int max_samples);
    void (*close)(void *dec);
    /* optional: the samples a decoder still holds once its input has ended (libavcodec keeps 4096 back); returns them */
    int (*drain)(void *dec, int16_t *out, int max_samples);
} x360_xma_backend;
void x360_xma_set_backend(const x360_xma_backend *b); /* NULL: the silent one */
const char *x360_xma_backend_name(void);
void x360_xma_get_stats(x360_machine *x, uint64_t *packets, uint64_t *frames, uint64_t *blocks, unsigned *contexts);
/* The pad motors XamInputSetState last asked for (0-255: left low frequency, right high frequency), user 0..3. */
void x360_pad_motors(x360_machine *x, int user, unsigned *left, unsigned *right);
/* A trace of the GPU command stream (PM4 packets, register blocks, draws, swaps), or NULL. */
void x360_set_gpu_trace(x360_machine *x, FILE *f);
/* Where DbgPrint and the runtime's own messages go (default stdout / stderr), verbosity (0 quiet, 1 HLE calls). */
void x360_set_output(x360_machine *x, FILE *dbg, FILE *log, int verbose);
/* Run until the title exits, an error, a missed address or `insns` more instructions. Returns X360_*. */
int x360_run(x360_machine *x, uint64_t insns);
const char *x360_error(const x360_machine *x);
/* Run until the virtual clock reaches `t` (100 ns units since the title started: x360_time + one frame for a
 * player's frame) or the title stops; X360_FRAME when it was reached (the status stays running), else as x360_run.
 * Waiting threads never take the clock past t. XBA-280. */
int x360_run_until(x360_machine *x, uint64_t t);
uint64_t x360_time(x360_machine *x);
uint32_t x360_exit_code(const x360_machine *x);
/* Addresses execution reached without lifted code (for x360_tools.recomp --discovered). */
unsigned x360_missed(const x360_machine *x, const uint32_t **addrs);
/* XBA-283: on (the default), code the pack does not hold is interpreted and recorded (x360_missed), and the run goes
 * on; off, the machine stops at the first missed address (X360_MISSED). */
void x360_set_interp(x360_machine *x, int on);
/* XBA-283, experimental (x360_run --host-threads): each guest thread runs its lifted code on a host thread of its
 * own, in parallel; the kernel, the interpreter and the scheduler stay serial under one lock. Not deterministic
 * (virtual time follows the host clock), no save states. Needs a bound pack made with atomic reservations
 * (xe_rc_features bit 0). 0 on success, -1 (x360_error says why) otherwise. Off: the default, cooperative and
 * deterministic. */
int x360_set_host_threads(x360_machine *x, int on);
/* image base, entry point, image size */
void x360_image_info(const x360_machine *x, uint32_t *base, uint32_t *entry, uint32_t *size);

/* Save states (x360_state.c), in the format of the other consoles' (gc_state.c): taken between time slices (after
 * x360_run returned), loaded into a machine made from the same XEX with the same pack bound. key: the title key
 * (NULL: the image's CRC). Results: */
#define X360_STATE_VERSION 1
enum { X360_STATE_OK = 0, X360_STATE_EGAME = -3, X360_STATE_EVERSION = -4, X360_STATE_ECORRUPT = -7 };
size_t x360_state_size(x360_machine *x);
size_t x360_state_save(x360_machine *x, const char *key, uint8_t *out, size_t cap);
int x360_state_load(x360_machine *x, const char *key, const uint8_t *data, size_t size, char *why, size_t why_n);

/* Controllers (x360_input.c): an XInput-style gamepad, set now or from a script of timed states (virtual ms). NULL
 * disconnects. user 0..3. */
typedef struct x360_pad {
    uint16_t buttons; /* XINPUT_GAMEPAD_*: up 0x1, down 0x2, left 0x4, right 0x8, start 0x10, back 0x20, LS 0x40,
                       * RS 0x80, LB 0x100, RB 0x200, A 0x1000, B 0x2000, X 0x4000, Y 0x8000 */
    uint8_t left_trigger, right_trigger;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} x360_pad;
typedef struct x360_pad_event {
    uint32_t ms;
    int user, connected;
    x360_pad pad;
} x360_pad_event;
void x360_set_pad(x360_machine *x, int user, const x360_pad *p);
int x360_set_pad_script(x360_machine *x, const x360_pad_event *events, unsigned n);
/* Cheats: big-endian words at guest addresses, allocated memory only (reads 0 / drops the write elsewhere). */
uint32_t x360_cheat_read32(x360_machine *x, uint32_t addr);
void x360_cheat_write32(x360_machine *x, uint32_t addr, uint32_t v);

typedef struct x360_stats {
    uint64_t native;       /* instructions run in lifted code */
    uint64_t interpreted;  /* instructions the interpreter ran */
    uint64_t hle_calls;
    uint64_t switches;     /* thread switches */
    uint32_t threads;      /* created so far */
    uint32_t unimplemented; /* distinct imports called without an HLE */
    uint64_t gpu_packets, gpu_draws, gpu_swaps;
    uint32_t vblanks;
    uint32_t seh_raised, seh_handlers, seh_unwinds; /* XBA-282: exceptions raised, handlers called, unwinds done */
} x360_stats;
void x360_get_stats(const x360_machine *x, x360_stats *s);

#ifdef __cplusplus
}
#endif

#endif
