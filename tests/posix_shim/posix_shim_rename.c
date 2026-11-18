/* rename onto an existing file replaces it, as on POSIX: every save, state and memory card is written as "path.tmp"
 * and renamed over the previous one. The Microsoft C runtime's own rename refuses an existing destination. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static const char TMP[] = "posix_shim_rename.tmp", DST[] = "posix_shim_rename.sav";

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int holds(const char *path, const char *text)
{
    char buf[32] = "";
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return !strcmp(buf, text);
}

int main(void)
{
    remove(TMP);
    remove(DST);

    put(TMP, "first");
    CHECK(rename(TMP, DST) == 0); /* no destination yet */
    CHECK(holds(DST, "first"));

    put(TMP, "second");
    CHECK(rename(TMP, DST) == 0); /* over the previous save */
    CHECK(holds(DST, "second"));
    FILE *gone = fopen(TMP, "rb");
    CHECK(gone == NULL); /* moved, not copied */
    if (gone)
        fclose(gone);

    errno = 0;
    CHECK(rename("posix_shim_rename.none", DST) == -1); /* a missing source fails and leaves the save alone */
    CHECK(errno == ENOENT);
    CHECK(holds(DST, "second"));

    remove(TMP);
    remove(DST);
    if (!failures)
        puts("posix_shim_rename: ok");
    return failures ? 1 : 0;
}
