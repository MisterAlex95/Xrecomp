/* WP7: the "preparing data" indicator (src/kernel/prep_progress.h).
 *
 * 1. The burst, with numbers: small writes never show; 8 MB shows; a log
 *    step every 64 MB; 5 s without a write ends it and says how much in how
 *    long; a write after an unseen end reports that end and starts anew.
 * 2. On POSIX, through xbox_NtWriteFile: bytes written to Z: (the utility
 *    drive's folder) count, bytes written to T: do not. */
#define _GNU_SOURCE
#include "prep_progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define MB (1ull << 20)

static void burst(void)
{
    PrepProgress p = { 0 };
    uint64_t bytes = 0, ms = 0, t = 1000;
    char buf[64];
    int i, steps = 0;

    /* A save: 2 MB, then quiet. Never shown, no end line. */
    CHECK(prep_progress_write(&p, t, 2 * MB, &bytes, &ms) == PREP_EV_NONE);
    CHECK(!prep_progress_text(&p, buf, sizeof buf) && !buf[0]);
    CHECK(prep_progress_expire(&p, t + PREP_IDLE_MS, &bytes, &ms) == PREP_EV_NONE);
    CHECK(p.bytes == 0);

    /* An unpacking: 1 MB every 100 ms. Shown at 8 MB. */
    t = 20000;
    for (i = 1; i <= 7; i++)
        CHECK(prep_progress_write(&p, t + (uint64_t)i * 100, MB, &bytes, &ms) == PREP_EV_NONE);
    CHECK(prep_progress_write(&p, t + 800, MB, &bytes, &ms) == PREP_EV_START);
    CHECK(prep_progress_text(&p, buf, sizeof buf) && !strcmp(buf, "preparing data 8 MB"));
    /* Up to 200 MB: a step at 72, 136 and 200 MB (every 64 MB after the start). */
    for (i = 9; i <= 200; i++) {
        int ev = prep_progress_write(&p, t + (uint64_t)i * 100, MB, &bytes, &ms);
        CHECK(ev == PREP_EV_NONE || ev == PREP_EV_STEP);
        steps += ev == PREP_EV_STEP;
    }
    CHECK(steps == 3);
    CHECK(p.logged == 200 * MB);
    CHECK(prep_progress_text(&p, buf, sizeof buf) && !strcmp(buf, "preparing data 200 MB"));
    /* A pause shorter than the idle time keeps it. */
    CHECK(prep_progress_expire(&p, t + 20000 + PREP_IDLE_MS - 1, &bytes, &ms) == PREP_EV_NONE);
    CHECK(p.shown);
    /* Quiet for the idle time: the end, 200 MB over the 19.9 s between the first and last write. */
    bytes = ms = 0;
    CHECK(prep_progress_expire(&p, t + 20000 + PREP_IDLE_MS, &bytes, &ms) == PREP_EV_END);
    CHECK(bytes == 200 * MB && ms == 19900);
    CHECK(!prep_progress_text(&p, buf, sizeof buf) && !p.shown && !p.bytes);

    /* Ended unseen (no poll): the next write reports the end and starts a new burst. */
    t = 100000;
    for (i = 0; i < 10; i++)
        prep_progress_write(&p, t + (uint64_t)i, MB, &bytes, &ms);
    CHECK(p.shown);
    bytes = ms = 0;
    CHECK(prep_progress_write(&p, t + 60000, MB, &bytes, &ms) == PREP_EV_END);
    CHECK(bytes == 10 * MB && ms == 9);
    CHECK(!p.shown && p.bytes == MB && p.t_first == t + 60000);
}

#ifdef PREP_TEST_FILES
#include "kernel.h"
#include <unistd.h>

extern void xbox_path_set_title_id(uint32_t title_id);

/* What the rest of the runtime would provide: no log, no symbolic links. */
void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}
const char *xbox_LookupSymbolicLink(const char *link)
{
    (void)link;
    return NULL;
}

static HANDLE create(const char *path)
{
    XBOX_ANSI_STRING name = {(USHORT)strlen(path), (USHORT)(strlen(path) + 1), (PCHAR)path};
    XBOX_OBJECT_ATTRIBUTES oa = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    HANDLE h = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    if (xbox_NtCreateFile(&h, 0xC0100080u /* GENERIC_READ|WRITE|SYNC|READ_ATTR */, &oa, &ios, NULL, 0x80, 0,
                          XBOX_FILE_OVERWRITE_IF, XBOX_FILE_NON_DIRECTORY_FILE) != STATUS_SUCCESS)
        return NULL;
    return h;
}

static void write_mb(HANDLE h, int n)
{
    static char chunk[1 << 20];
    XBOX_IO_STATUS_BLOCK ios = {0};
    int i;
    for (i = 0; i < n; i++)
        CHECK(xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, chunk, sizeof chunk, NULL) == STATUS_SUCCESS);
}

static void files(void)
{
    char root[] = "/tmp/xbox-prep-XXXXXX", buf[64];
    HANDLE z, e;

    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    xbox_path_init(root, root);
    xbox_path_set_title_id(0x4D5300D1u);

    e = create("T:\\notz.bin");
    z = create("Z:\\data.big");
    CHECK(e != NULL && z != NULL);
    if (!e || !z)
        return;
    write_mb(e, 20);                                /* not the utility drive: nothing */
    CHECK(!recomp_prep_status(buf, sizeof buf) && !buf[0]);
    write_mb(z, 4);
    CHECK(!recomp_prep_status(buf, sizeof buf));    /* 4 MB: not yet */
    write_mb(z, 6);
    CHECK(recomp_prep_status(buf, sizeof buf));
    printf("status after 10 MB to Z: \"%s\"\n", buf);
    CHECK(!strcmp(buf, "preparing data 10 MB"));
    xbox_NtClose(z);
    xbox_NtClose(e);
}
#endif

int main(void)
{
    burst();
#ifdef PREP_TEST_FILES
    files();
#endif
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
