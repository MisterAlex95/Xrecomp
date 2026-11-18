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
} x360_pack;

enum {
    X360_RUNNING = 0,
    X360_EXITED = 1,   /* the title ended (main thread returned, ExTerminateThread of the last thread, ...) */
    X360_ERROR = 2,    /* see x360_error() */
    X360_BUDGET = 3,   /* the instruction budget ran out */
    X360_MISSED = 4,   /* execution reached an address the pack does not cover (x360_missed()) */
};

/* Load an XEX2 (unencrypted, compression none or basic: python3 -m tools.xex FILE --plain OUT makes one from an
 * encrypted / LZX title with the user's key). NULL on error (why says it). */
x360_machine *x360_create(const uint8_t *xex, size_t size, char *why, size_t why_len);
void x360_destroy(x360_machine *x);
/* Bind the lifted code; 0 on success, -1 when the pack was made from another image (stale). */
int x360_bind(x360_machine *x, const x360_pack *pack);
/* The disc: a disc image (XDVDFS / GDFX, also split or CSO / ZSO / CCI, read in place by src/xiso) or a directory of
 * its files; the data directory (cache:, hdd:...: writable), optional. 0 on success. */
int x360_set_disc(x360_machine *x, const char *path, char *why, size_t why_len);
int x360_set_data_dir(x360_machine *x, const char *path);
/* A trace of the GPU command stream (PM4 packets, register blocks, draws, swaps), or NULL. */
void x360_set_gpu_trace(x360_machine *x, FILE *f);
/* Where DbgPrint and the runtime's own messages go (default stdout / stderr), verbosity (0 quiet, 1 HLE calls). */
void x360_set_output(x360_machine *x, FILE *dbg, FILE *log, int verbose);
/* Run until the title exits, an error, a missed address or `insns` more instructions. Returns X360_*. */
int x360_run(x360_machine *x, uint64_t insns);
const char *x360_error(const x360_machine *x);
uint32_t x360_exit_code(const x360_machine *x);
/* Addresses execution reached without lifted code (for x360_tools.recomp --discovered). */
unsigned x360_missed(const x360_machine *x, const uint32_t **addrs);
/* image base, entry point, image size */
void x360_image_info(const x360_machine *x, uint32_t *base, uint32_t *entry, uint32_t *size);

typedef struct x360_stats {
    uint64_t native;       /* instructions run in lifted code */
    uint64_t interpreted;  /* instructions the interpreter ran */
    uint64_t hle_calls;
    uint64_t switches;     /* thread switches */
    uint32_t threads;      /* created so far */
    uint32_t unimplemented; /* distinct imports called without an HLE */
    uint64_t gpu_packets, gpu_draws, gpu_swaps;
    uint32_t vblanks;
} x360_stats;
void x360_get_stats(const x360_machine *x, x360_stats *s);

#ifdef __cplusplus
}
#endif

#endif
