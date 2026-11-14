/* The PS1 HLE BIOS (P-5 HLE, XBA-163): the kernel and the A / B / C function tables in C, so a program boots without
 * a BIOS image. Not a public API (ps1_core.h is): the core calls it, the tests include it for the call list.
 *
 * Kernel RAM (0x00000000..0x0000FFFF) is laid out the way the retail BIOS / OpenBIOS lay it out where games look:
 * the exception vector at 0x80 (lui k0 / addiu k0 / jr k0: the halfword at 0x86 is 0x275A, the retail value), the
 * A0 / B0 / C0 vectors, the table of tables at 0x100 (handlers, processes, threads, events), the command line at
 * 0x180, the A0 table at 0x200, the C0 table at 0x674 and the B0 table at 0x874, TCBs (0xC0 bytes: registers at +8,
 * EPC +0x88, HI +0x8C, LO +0x90, SR +0x94, CAUSE +0x98) and EvCBs (0x1C bytes: class, status, spec, mode, handler).
 * Each table entry points to a 4-byte stub holding a trap word (opcode 3Fh, reserved on the R3000A) that the
 * interpreter hands to ps1_hle_trap when it runs from kernel RAM in HLE mode: the C function runs and returns to ra.
 * A game that patches a table entry or calls an entry it read from a table works as on hardware, and so does lifted
 * code (packs): kernel RAM is never lifted, its jumps to 0xA0 / 0xB0 / 0xC0 land in the interpreter.
 *
 * The exception handler is MIPS in kernel RAM like OpenBIOS's: a trap saves the registers into the current TCB, the
 * handler walks the four SysEnqIntRP priority chains (verifier, then handler) natively, then a second trap longjmps
 * to the exception jmp_buf (B(18h) / B(19h); the default one returns from the exception). The built-in handlers
 * (syscall, IRQ events, root counters, the pad / card VBlank handler) are stubs in those chains. */
#ifndef PS1_HLE_H
#define PS1_HLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct ps1_machine ps1_machine;
typedef struct ps1_hle ps1_hle;

/* Kernel RAM. */
#define PS1_HLE_ATAB 0x0200u   /* A0 table, 0xC0 entries */
#define PS1_HLE_CTAB 0x0674u   /* C0 table, 0x20 entries */
#define PS1_HLE_BTAB 0x0874u   /* B0 table, 0x60 entries */
#define PS1_HLE_STUBS 0x1000u  /* trap stubs: 4 bytes per (table, function), 0x100 functions per table */
#define PS1_HLE_TCB 0x2000u    /* TCBs, 0xC0 bytes each */
#define PS1_HLE_EVCB 0x3000u   /* EvCBs, 0x1C bytes each */

/* The trap word: opcode 3Fh, magic 0xFF5xxxxx, (table << 8) | function in the low 16 bits. Tables 0 = A, 1 = B,
 * 2 = C, 3 = the kernel's own entry points. */
#define PS1_HLE_TRAP 0xFF500000u
#define PS1_HLE_TRAP_MASK 0xFFF00000u

int ps1_hle_create(ps1_machine *m);   /* 0, or -1 (out of memory) */
void ps1_hle_destroy(ps1_machine *m);
/* Sets up kernel RAM and boots: the EXE (m->exe) if any, else the disc's SYSTEM.CNF boot file, else an idle loop. */
void ps1_hle_boot(ps1_machine *m);
/* The interpreter ran trap word `code` (the low 16 bits) from kernel RAM: the call runs. */
void ps1_hle_trap(ps1_machine *m, uint32_t code);

/* The call list (tests, docs): 1 when (table 0..2, function) is implemented, its name in *name (may be NULL). */
int ps1_hle_implemented(int table, int fn, const char **name);
/* Distinct unimplemented calls the program made so far (each reported once on stderr). */
int ps1_hle_unimplemented_count(ps1_machine *m);

/* Save states (XBA-182, ps1_state.c): the HLE BIOS's state as bytes (0 without the HLE BIOS). busy: a guest call
 * from C is in progress (C frames on the host stack): no state can be taken then. load: 0, or -1 on a size
 * mismatch. */
size_t ps1_hle_state_size(ps1_machine *m);
int ps1_hle_state_busy(ps1_machine *m);
void ps1_hle_state_save(ps1_machine *m, uint8_t *out);
int ps1_hle_state_load(ps1_machine *m, const uint8_t *in, size_t n);

#endif
