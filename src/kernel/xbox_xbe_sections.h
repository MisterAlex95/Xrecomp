/* XBE sections loaded on demand (XBA-124): what XeLoadSection and
 * XeUnloadSection do on the console, on top of a layout that maps every
 * section at boot.
 *
 * The console's loader maps only the sections marked PRELOAD, with a
 * reference count of 1 (and one more on the shared head and tail pages'
 * counters). A title brings in the others with XAPI's XLoadSection, which
 * calls XeLoadSection: on 0 -> 1 the kernel commits the pages and reads the
 * section from the image -- data initialised from the file, the rest zero --
 * and XeUnloadSection on 1 -> 0 releases them. Sections that share a virtual
 * address are alternatives: one at a time.
 *
 * Here every section is copied at boot (xbox_MemoryLayoutInit) and stays
 * mapped. What this module adds:
 *   - the reference counts the console starts with (PRELOAD: 1, with the
 *     shared-page counters), kept in the guest's own section headers;
 *   - on 0 -> 1, a section whose bytes may have changed since boot is copied
 *     again from the image: a writable one (the title wrote it before it
 *     unloaded it, and expects fresh data and zeroed BSS back), or one that
 *     shares its addresses with another (whose bytes are there now);
 *   - at boot, sections that overlap: the PRELOAD one is the one in memory
 *     (the layout copies in table order, so the last one used to win), and
 *     each overlap is printed. Two code sections at one address are a
 *     blocker: the lifter has one function per address, so the C holds one
 *     section's code only (tools.triage and tools.prepare say so).
 * Read-only sections are not copied again: nothing writes them, and their
 * pages are never released here.
 *
 * RECOMP_XBE_SECTIONS=0: the old bookkeeping only (the count in the header
 * goes up and down from what the file says, nothing is copied). */
#ifndef XBOX_XBE_SECTIONS_H
#define XBOX_XBE_SECTIONS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After xbox_MemoryLayoutInit has copied the header and every section:
 * records the section table, keeps a copy of what may need copying again,
 * sets the boot reference counts, settles overlaps. */
void xbox_XbeSectionsInit(const uint8_t *xbe, size_t xbe_size);

/* Frees the copies (xbox_MemoryLayoutShutdown). */
void xbox_XbeSectionsShutdown(void);

/* XeLoadSection / XeUnloadSection on the section header at guest VA hdr_va.
 * NTSTATUS: 0, or STATUS_INVALID_PARAMETER for a null header. A header that
 * is not one of the image's gets the count bookkeeping only. */
uint32_t xbox_XbeSectionLoad(uint32_t hdr_va);
uint32_t xbox_XbeSectionUnload(uint32_t hdr_va);

/* Tests and the triage: sections recorded, overlapping pairs found. */
unsigned xbox_XbeSectionCount(void);
unsigned xbox_XbeSectionOverlaps(void);

#ifdef __cplusplus
}
#endif

#endif
