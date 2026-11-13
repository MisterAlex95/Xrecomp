/* The PlayStation runtime links and answers (XBA-146). */
#include "ps1_runtime.h"
#include "host_time.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *v = ps1_runtime_version();

    if (!v || strncmp(v, "ps1_runtime ", 12) != 0) {
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
