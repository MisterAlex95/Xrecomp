/* The discovery file (XBA-64): one JSON object per line, appended as it
 * happens, only what is new across runs, off without RECOMP_DISCOVERY. */
#define _POSIX_C_SOURCE 200809L
#include "recomp_discovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    static char buf[16384];
    size_t n;
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

int main(void)
{
    char dir[256], path[300];
    const char *tmp = getenv("TMPDIR");
    char *text;

    snprintf(dir, sizeof dir, "%s/xba_discovery_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    snprintf(path, sizeof path, "%s/discovered.jsonl", dir);   /* the folder does not exist yet */
    remove(path);

    /* Off: unset, empty, "0". */
    set_env("RECOMP_DISCOVERY", "0");
    recomp_discovery_reset();
    recomp_discovery_icall(0x00012345u, 0x00011111u);
    CHECK(recomp_discovery_path() == NULL && recomp_discovery_written() == 0);

    /* One run: each kind once, duplicates dropped, the folder made. */
    set_env("RECOMP_DISCOVERY", path);
    recomp_discovery_reset();
    recomp_discovery_icall(0x00012345u, 0x00011111u);
    recomp_discovery_icall(0x00012345u, 0x00022222u);          /* same target, another caller */
    recomp_discovery_thread(0x00034560u, 0x00045670u);
    recomp_discovery_thread_ctx(0x00045670u, 0x00034560u);
    recomp_discovery_unimpl(0x00056780u, "bsf eax, \"ecx\"\\\x01");
    recomp_discovery_unimpl(0x00056780u, "bsf eax, ecx");
    recomp_discovery_d3d(0x0085FDE0u, 0x00862288u);
    recomp_discovery_d3d(0x0085FDE0u, 0x00862288u);
    recomp_discovery_launch("d:\\game.xbe");
    recomp_discovery_launch("D:\\GAME.XBE");          /* the same image: one entry */
    recomp_discovery_stack_switch(0x00023450u, 0x00023480u, "a \"why\"");
    recomp_discovery_stack_switch(0x00023450u, 0x00012345u, "again");   /* the same site: one entry */
    recomp_discovery_stub(136, "KeRemoveQueue", 0x00012340u);   /* XBA-115: keyed by ordinal */
    recomp_discovery_stub(136, "KeRemoveQueue", 0x00099990u);
    recomp_discovery_exception(0x00012350u, 0xC0000094u, "divide by zero");   /* XBA-129 */
    recomp_discovery_exception(0x00012350u, 0xC0000094u, "again");
    CHECK(recomp_discovery_written() == 9);
    CHECK(recomp_discovery_seeds() == 3);                       /* icall, thread, thread_ctx */
    CHECK(recomp_discovery_path() && !strcmp(recomp_discovery_path(), path));
    text = slurp(path);                                         /* written as it happened: no close needed */
    CHECK(text != NULL);
    if (text) {
        CHECK(count(text, "\n") == 10);
        CHECK(strstr(text, "{\"t\":\"exception\",\"va\":\"0x00012350\",\"code\":\"0xC0000094\","
                           "\"why\":\"divide by zero\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"stack_switch\",\"va\":\"0x00023450\",\"to\":\"0x00023480\","
                           "\"why\":\"a \\\"why\\\"\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"stub\",\"ord\":136,\"name\":\"KeRemoveQueue\",\"ret\":\"0x00012340\"}\n") != NULL);
        CHECK(strstr(text, "\"path\":\"d:\\\\game.xbe\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"d3d\",\"va\":\"0x0085FDE0\",\"slot\":\"0x00862288\"}\n") != NULL);
        CHECK(count(text, "{\"t\":\"run\",\"time\":\"20") == 1);
        CHECK(strstr(text, "{\"t\":\"icall\",\"va\":\"0x00012345\",\"ret\":\"0x00011111\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"thread\",\"va\":\"0x00034560\",\"ctx1\":\"0x00045670\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"thread_ctx\",\"va\":\"0x00045670\",\"routine\":\"0x00034560\"}\n") != NULL);
        CHECK(strstr(text, "{\"t\":\"unimpl\",\"va\":\"0x00056780\",\"insn\":\"bsf eax, \\\"ecx\\\"\\\\?\"}\n") != NULL);
    }

    /* A second run (a new process): what the file holds is not written again. */
    recomp_discovery_reset();
    recomp_discovery_icall(0x00012345u, 0x00099999u);
    recomp_discovery_unimpl(0x00056780u, "bsf eax, ecx");
    recomp_discovery_stub(136, "KeRemoveQueue", 0x00012340u);   /* read back by "ord" */
    CHECK(recomp_discovery_written() == 0);
    recomp_discovery_stub(0x00012345u, "Other", 0);               /* an ordinal equal to an icall va: its own key */
    CHECK(recomp_discovery_written() == 1);
    CHECK(recomp_discovery_seeds() == 0);
    recomp_discovery_exception(0x00012350u, 0xC0000094u, "read back");     /* known from the file */
    CHECK(recomp_discovery_written() == 1);
    recomp_discovery_icall(0x00067890u, 0x00011111u);
    CHECK(recomp_discovery_written() == 2 && recomp_discovery_seeds() == 1);
    text = slurp(path);
    if (text) {
        CHECK(count(text, "{\"t\":\"run\"") == 2);
        CHECK(count(text, "\"va\":\"0x00012345\"") == 1);
        CHECK(strstr(text, "{\"t\":\"icall\",\"va\":\"0x00067890\"") != NULL);
    }

    /* An unwritable path: off, with one line on stderr, no crash. */
    set_env("RECOMP_DISCOVERY", "/nonexistent-dir/x/y/discovered.jsonl");
    recomp_discovery_reset();
    recomp_discovery_icall(0x1u, 0x2u);
    CHECK(recomp_discovery_path() == NULL);

    remove(path);
    remove(dir);
    if (failures)
        return 1;
    printf("recomp_discovery: ok\n");
    return 0;
}
