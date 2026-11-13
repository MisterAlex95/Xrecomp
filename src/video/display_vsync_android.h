/* display_vsync_android.h -- the display's refresh grid from AChoreographer
 * (XBA-136), published to the executor for RECOMP_PACING=auto's flips in
 * phase with the panel. Android only. */
#ifndef XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H
#define XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H

#include <stdint.h>

/* Starts the vsync thread (nominal_ns: the refresh SDL reports, 0 unknown).
 * Returns 0 when there is no Choreographer. */
int display_vsync_android_start(uint64_t nominal_ns);
void display_vsync_android_stop(void);
/* "frame deadlines" (Android 13+) or "vsync ticks" (11-12), for the log. */
const char *display_vsync_android_source(void);

#endif /* XBOXRECOMP_DISPLAY_VSYNC_ANDROID_H */
