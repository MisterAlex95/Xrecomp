/* What a run meets that the static analysis missed (XBA-64).
 *
 * The runtime appends one JSON object per line to the discovery file as it
 * happens (append + fsync: a crash keeps what was written), and only what is
 * new: entries already in the file, from this run or an earlier one, are not
 * written again. tools.prepare reads it to seed the next prepare (XBA-65);
 * tools.seed_from_log takes it too. Format: docs/technical/discovery.md.
 *
 *   {"t":"run","time":"2026-11-11T18:00:00Z"}             once per process, before its first entry
 *   {"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}    indirect call to code with no dispatch entry
 *   {"t":"thread","va":"0x00012345","ctx1":"0x00045678"}  thread start routine with no dispatch entry
 *   {"t":"thread_ctx","va":"0x00045678","routine":"0x00012345"}
 *                                                          StartContext1 in the code range, not dispatchable
 *   {"t":"unimpl","va":"0x00012345","insn":"bsf eax, ecx"} untranslated instruction reached
 *   {"t":"d3d","va":"0x0085FDE0","slot":"0x00862288"}      D3D's device found at run time
 *                                                          (profiles/xdk/d3d8/d3d8.c)
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

/* The file in use, or NULL when discovery is off (opens it on first use). */
const char *recomp_discovery_path(void);

/* Entries written by this process (tests, the exit report). */
unsigned recomp_discovery_written(void);

/* Tests: forget the open file and what was seen, read $RECOMP_DISCOVERY again. */
void recomp_discovery_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_DISCOVERY_H */
