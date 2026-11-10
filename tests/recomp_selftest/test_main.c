/* recomp_selftest (XBA-51): the app shell's self-test, run as a CMake test.
 *
 *   recomp_selftest_test <synthetic.xbe> <save dir>
 *
 * The same recomp_selftest_run() the shell calls under
 * RECOMP_SHELL_SELFTEST=1, so a check that passes here and fails in the app
 * is an Android difference (untrusted_app, ART, libsigchain), not a runtime
 * bug. The XBE is tools/conformance/test.xbe (ours, no game content). */
#include "recomp_selftest.h"
#include "kernel/xbox_memory_layout.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static unsigned char *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    size_t len = 0;
    unsigned char *xbe;
    int failed;

    if (argc < 3) {
        fprintf(stderr, "usage: %s synthetic.xbe save-dir\n", argv[0]);
        return 2;
    }
    xbe = load(argv[1], &len);
    if (!xbe) {
        fprintf(stderr, "cannot read %s (tools/conformance/mkxbe.py makes it)\n", argv[1]);
        return 2;
    }
    /* Nothing else installed a handler: the toolkit's chains to the default
     * action, as it chains to debuggerd in the app. */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    failed = recomp_selftest_run(xbe, len, argv[2]);
    free(xbe);
    return failed ? 1 : 0;
}
