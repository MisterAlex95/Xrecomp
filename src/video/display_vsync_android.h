/* display_vsync_android.h -- the display's refresh grid from AChoreographer
 * (XBA-136), published to a sink (the Xbox executor, for RECOMP_PACING=auto's
 * flips in phase with the panel). Android only; part of the host layer. */
#ifndef XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H
#define XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H

#include <stdint.h>

/* Where each sample of the grid goes, on the vsync thread: the last vsync
 * (or latch deadline) and the period in ns, 0 0 when the grid is lost; gen
 * changes when the grid restarts. Console-agnostic since XBA-145: the Xbox
 * presenter passes the executor's nv2a_pb_exec_display_clock. */
typedef void (*display_vsync_sink)(uint64_t vsync_ns, uint64_t period_ns, uint32_t gen);

/* Starts the vsync thread (nominal_ns: the refresh SDL reports, 0 unknown).
 * Returns 0 when there is no Choreographer (or sink is NULL). */
int display_vsync_android_start(uint64_t nominal_ns, display_vsync_sink sink);
void display_vsync_android_stop(void);
/* "frame deadlines" (Android 13+) or "vsync ticks" (11-12), for the log. */
const char *display_vsync_android_source(void);

#endif /* XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H */
