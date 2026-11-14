/* The Game Boy Advance runtime links and answers (XBA-146). */
#include "gba_runtime.h"
#include "host_time.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *v = gba_runtime_version();

    if (!v || strncmp(v, "gba_runtime ", 12) != 0) {
        fprintf(stderr, "FAIL: version \"%s\"\n", v ? v : "(null)");
        return 1;
    }
    if (xbox_HostNowNs() == 0) {   /* the host layer is linked in */
        fprintf(stderr, "FAIL: no host clock\n");
        return 1;
    }
    if (gba_runtime_start("game", "libtitle.so", "KEY") != 0) {   /* the stub packs route to (XBA-147) */
        fprintf(stderr, "FAIL: gba_runtime_start\n");
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
