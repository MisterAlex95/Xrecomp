/* What a run meets that the static analysis missed (XBA-64).
 *
 * The runtime appends one JSON object per line to the discovery file as it
 * happens (an O_APPEND write per entry: a crash of the process keeps what was
 * written), and only what is
 * new: entries already in the file, from this run or an earlier one, are not
 * written again. tools.prepare reads it to seed the next prepare (XBA-65);
 * tools.seed_from_log takes it too. Format: docs/technical/discovery.md.
 *
 *   {"t":"run","time":"2026-11-11T18:00:00Z","v":2}       once per process, before its first entry; "v":2:
 *                                                          this writer marks taint and writes "wild" (profile-free WP1)
 *   {"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}    indirect call to code with no dispatch entry
 *   {"t":"wild","va":"0xFF084E8B","ret":"0x000CFA7A"}     indirect call to an address that is not code
 *                                                          (a null or wild pointer; profile-free WP1): never a seed,
 *                                                          it names its call site (ret) as wild
 *
 * Taint (profile-free WP1): from the first wild, smc or exception entry of a process
 * (written or already in the file) on, its icall / thread / thread_ctx
 * entries end with ,"tainted":true: the run may be calling through garbage.
 * A tainted entry is keyed apart from an untainted one, so a later healthy
 * run still writes the address, untainted; tools.seed_from_log seeds only
 * what a healthy stretch of a run reported.
 *   {"t":"thread","va":"0x00012345","ctx1":"0x00045678"}  thread start routine with no dispatch entry
 *   {"t":"thread_ctx","va":"0x00045678","routine":"0x00012345"}
 *                                                          StartContext1 in the code range, not dispatchable
 *   {"t":"unimpl","va":"0x00012345","insn":"bsf eax, ecx"} untranslated instruction reached
 *   {"t":"d3d","va":"0x0085FDE0","slot":"0x00862288"}      D3D's device found at run time
 *                                                          (recomp_d3d_service.c)
 *   {"t":"wait_spin","va":"0x0027B91C","ret":"0x00025701","header":"0x00040001","status":"0xC0000001"}
 *                                                          a thread waits on it thousands of times a second
 *   {"t":"launch","va":"0x5A1C33E0","path":"d:\\game.xbe"}  the title launched another image (XBA-82);
 *                                                          va is a hash of the path
 *   {"t":"stack_switch","va":"0x00023450","to":"0x00023480","why":"..."}
 *                                                          a guest stack switch the runtime could not
 *                                                          carry (XBA-101, recomp_stack_switch.c);
 *                                                          va is the esp load, to where it went;
 *                                                          or an SEH resume refused across host
 *                                                          stacks (xbox_seh.c): va is the resume
 *                                                          target, to its function; or a parked
 *                                                          context resumed although no saved stack
 *                                                          matched (why "resumed a parked context
 *                                                          whose stack does not match"): va is where
 *                                                          it resumes, to the esp it parked at. A
 *                                                          sign the run went wrong from there on
 *   {"t":"stub","ord":136,"name":"KeRemoveQueue","ret":"0x00012345"}
 *                                                          a kernel route that is still a stub was
 *                                                          called (XBA-115; keyed by ord, no va)
 *   {"t":"smc","va":"0x00A01000","kind":"exec_outside_image","ret":"0x00012345","where":"heap","bytes":"5589E583EC10..."}
 *                                                          an indirect call or jump to mapped guest memory
 *                                                          outside the image's code (XBA-118, recomp_smc.h);
 *                                                          bytes: the first 8 at the target, hex
 *   {"t":"smc","va":"0x00012000","kind":"code_write","at":"0x00012345","section":".text","by":"a store","ret":"0x..."}
 *                                                          the first write into a lifted code page (va: the
 *                                                          page); ret: the word at the guest stack top
 *   {"t":"exception","va":"0x00012345","code":"0xC0000094","why":"..."}
 *                                                          a guest exception nothing handled: a
 *                                                          divide error that stops the title
 *                                                          (kernel_hal.c), a KeBugCheck the title
 *                                                          called (code: the bug-check code, va:
 *                                                          the call's return address, why
 *                                                          "KeBugCheck..."), or a raise no handler
 *                                                          took (xbox_seh.c); a Prepare cannot fix
 *                                                          it (XBA-129)
 *   {"t":"d3d_layout","va":"0x1A2B3C4D","lib":"D3D8LTCG","build":5933,"rule":"fence_event","used":"0x00001DCC"}
 *                                                          the D3D service met a D3D build its table does
 *                                                          not know and fell back to another build's value
 *                                                          for that rule (XBA-105, recomp_d3d_service.c);
 *                                                          va is a hash of lib, build and rule
 *   {"t":"cut_body","va":"0x001C1208"}                     a lifted body ran off its end: the analysis
 *                                                          cut it short (WP2, recomp_cut_body in
 *                                                          kernel_hal.c); va is its last instruction.
 *                                                          The title stops; not a seed
 *
 * The file: $RECOMP_DISCOVERY (recomp.cfg can set it); unset, empty or "0":
 * nothing is written. The desktop title project gets a default from
 * tools.prepare (<work>/discovered.jsonl), the Android shell one in the
 * title's work folder (<files>/work/<TITLEID>/discovered.jsonl). */
#ifndef XBOXRECOMP_RECOMP_DISCOVERY_H
#define XBOXRECOMP_RECOMP_DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_discovery_icall(uint32_t va, uint32_t ret_va);
void recomp_discovery_thread(uint32_t routine, uint32_t ctx1);
void recomp_discovery_thread_ctx(uint32_t ctx1, uint32_t routine);
void recomp_discovery_unimpl(uint32_t va, const char *insn);
void recomp_discovery_d3d(uint32_t device, uint32_t slot);
void recomp_discovery_wait_spin(uint32_t object, uint32_t ret_va, uint32_t header, uint32_t status);
void recomp_discovery_launch(const char *path);
void recomp_discovery_stack_switch(uint32_t site, uint32_t to, const char *why);
void recomp_discovery_stub(uint32_t ordinal, const char *name, uint32_t ret_va);
void recomp_discovery_smc_exec(uint32_t target, uint32_t ret_va, const char *where, const char *bytes);
void recomp_discovery_smc_write(uint32_t page, uint32_t at, const char *section, const char *by, uint32_t ret_va);
void recomp_discovery_exception(uint32_t va, uint32_t code, const char *why);
void recomp_discovery_d3d_layout(const char *lib, uint32_t build, const char *rule, uint32_t used);
void recomp_discovery_cut_body(uint32_t va);
void recomp_discovery_wild(uint32_t va, uint32_t ret_va);

/* 1 once the process went wrong (the taint above), else 0. */
int recomp_discovery_tainted(void);

/* The file in use, or NULL when discovery is off (opens it on first use). */
const char *recomp_discovery_path(void);

/* Entries written by this process (tests, the exit report). */
unsigned recomp_discovery_written(void);

/* Of them, the code addresses the next Prepare can seed (icall, thread,
 * thread_ctx, untainted): the status line's "seeds" (XBA-129). */
unsigned recomp_discovery_seeds(void);

/* Before the process ends on an error (KeBugCheck): what was written goes to
 * the storage now (fsync). No-op when nothing is open. */
void recomp_discovery_flush(void);

/* Tests: forget the open file and what was seen, read $RECOMP_DISCOVERY again. */
void recomp_discovery_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_DISCOVERY_H */
