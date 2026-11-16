/* win32_runtime.h - the Win32 runtime of a recompiled Windows PE (XBA-233, epic XBA-231).
 *
 * The spike's runtime for 32-bit Windows console programs lifted by the x86 pipeline (tools/pe_parser, then
 * tools.disasm ... tools.recomp, as for an XBE). It is not the Xbox kernel and links none of it: it shares only the
 * lifter's ABI (templates/runtime/recomp_types.h: g_cpu, the MEM macros over g_xbox_mem_offset, the indirect-call
 * dispatch) and the same guest memory model (guest address + g_xbox_mem_offset = host address).
 *
 *   - Guest memory: the whole 32-bit space reserved up front, pages committed where the loader, the stack, the heap
 *     and the TEB/PEB need them; a stray access faults instead of reading zeros.
 *   - The loader maps the .exe the user owns at its preferred base (the lifted code has its absolute addresses
 *     baked in, so the base relocations are never applied to an EXE), resolves the import table by name or ordinal
 *     against the HLE table (win32_hle.c) and writes a synthetic address per import into its IAT slot:
 *     W32_THUNK_BASE + 4 * index, which the lifted `call [slot]` reaches through recomp_lookup_kernel.
 *   - The TEB (fs:), the PEB, the TLS directory (template and callbacks) and the command line.
 *
 * Usage (a title's main, see tests/win32_hello): return w32_main(argc, argv); argv[1] is the .exe, the rest its
 * command line. Exit status: the guest's ExitProcess / exit code; 127 for an import with no HLE. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synthetic import addresses: at or above 0xFE000000, which RECOMP_ICALL_IS_CODE always lets through. */
#define W32_THUNK_BASE 0xFEE00000u
#define W32_THUNK_MAX  4096u

/* Exit status of a run stopped by an import (or an instruction) the runtime does not implement. */
#define W32_EXIT_MISSING 127

/* ── Guest memory ─────────────────────────────────────────── */
int      w32_mem_init(void);                    /* 0 on success */
int      w32_commit(uint32_t va, uint32_t size); /* read/write, page granular; 0 on success */
void    *w32_host(uint32_t va);                 /* host pointer of a guest address */
uint32_t w32_alloc(uint32_t size);              /* guest heap, zeroed, 16-byte aligned; 0 when out of memory */
uint32_t w32_strdup(const char *s);             /* a guest copy of a host string */

static inline uint32_t w32_rd32(uint32_t va) { uint32_t v; __builtin_memcpy(&v, w32_host(va), 4); return v; }
static inline void     w32_wr32(uint32_t va, uint32_t v) { __builtin_memcpy(w32_host(va), &v, 4); }

/* ── The loaded image ─────────────────────────────────────── */
typedef struct w32_image {
    uint32_t base, size, entry;
    uint32_t stack_reserve;
    uint32_t code_lo, code_hi;      /* executable sections */
    uint32_t tls_dir;               /* VA of the TLS directory, 0 when none */
    unsigned imports, missing;      /* resolved / without an HLE */
} w32_image;

int w32_load(const char *path, w32_image *img);  /* 0 on success; prints why not */

/* ── HLE (win32_hle.c) ────────────────────────────────────── */
typedef uint32_t (*w32_hle_fn)(void);           /* reads its arguments with w32_arg, returns eax */
typedef struct w32_export {
    const char *dll;                /* lower case, with .dll */
    const char *name;
    w32_hle_fn fn;                  /* NULL for a data export */
    int stdcall_args;               /* arguments the callee pops (stdcall); -1: cdecl, the caller pops */
    uint32_t data_size;             /* data export: bytes the runtime allocates; its address goes in the IAT */
} w32_export;

const w32_export *w32_find_export(const char *dll, const char *name);
void w32_hle_data_init(const w32_export *e, uint32_t va);  /* fill a data export the loader allocated */
void w32_hle_set_args(int argc, char **argv);

/* The i-th argument of the HLE call being made (0 = the first, at esp+4). */
uint32_t w32_arg(int i);
uint32_t w32_arg_va(int i);           /* its guest address (a cdecl function's `...` starts there) */
/* Call a lifted guest function with stdcall/cdecl arguments; returns its eax. */
uint32_t w32_call_guest(uint32_t va, const uint32_t *args, int n);
/* Stop the guest: ExitProcess, exit, abort. Does not return. */
void w32_exit(uint32_t code) __attribute__((noreturn));
extern int g_w32_trace;
extern uint32_t g_w32_image_base;              /* XBR_WIN32_TRACE=1: one line per import call */

int w32_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
