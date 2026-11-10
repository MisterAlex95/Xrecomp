/* recomp_quit.h (XBA-52). */
#include "recomp_quit.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "android_log.h"
#include "recomp_exit_hook.h"

void recomp_quit(int code)
{
#if defined(__ANDROID__)
    recomp_exit_hook_run("quit");
    printf("xboxrecomp: clean exit\n");
    fflush(stdout);
    fflush(stderr);
    xbox_log_drain(RECOMP_QUIT_DRAIN_MS);
    _exit(code);
#else
    exit(code);
#endif
}
