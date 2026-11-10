/* Extract an Xbox disc image (XISO) on a desktop with the library the Android app uses (src/xiso, XBA-40).
 *
 *     xiso_extract image.iso outdir
 *
 * Prints the title found in default.xbe, then one progress line (rewritten in place on a terminal). outdir is
 * replaced as a whole on success (xiso.h). Built by tests/xiso. Exit status: 0, 1 on an error, 2 on bad usage.
 */
#include "xiso.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int tty;

static int progress(uint64_t done, uint64_t total, const char *path, void *user)
{
    (void)user;
    double pct = total ? 100.0 * (double)done / (double)total : 100.0;
    if (tty)
        printf("\r\033[K%5.1f%%  %" PRIu64 " / %" PRIu64 " MB  %.60s", pct, done >> 20, total >> 20, path);
    else if (!path[0])
        printf("100.0%%  %" PRIu64 " bytes\n", total);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s image.iso outdir\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    tty = isatty(1);
    char title[128];
    uint64_t total;
    int xbe;
    int rc = xiso_probe_fd(fd, title, sizeof title, &total, &xbe);
    if (rc == 0) {
        printf("%s: \"%s\", %" PRIu64 " bytes, %s\n", argv[1], title, total, xbe ? "default.xbe" : "no default.xbe");
        rc = xiso_extract_fd(fd, argv[2], progress, NULL);
        if (tty)
            printf("\n");
    }
    close(fd);
    if (rc != 0) {
        fprintf(stderr, "%s: %s\n", argv[1], xiso_strerror(rc));
        return 1;
    }
    printf("extracted to %s\n", argv[2]);
    return 0;
}
