/* The Nintendo 64 runtime links and answers (XBA-146). */
#include "n64_runtime.h"
#include "host_time.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *v = n64_runtime_version();

    if (!v || strncmp(v, "n64_runtime ", 12) != 0) {
        fprintf(stderr, "FAIL: version \"%s\"\n", v ? v : "(null)");
        return 1;
    }
    if (xbox_HostNowNs() == 0) {   /* the host layer is linked in */
        fprintf(stderr, "FAIL: no host clock\n");
        return 1;
    }
    printf("%s: ok\n", v);
    return 0;
}
