/* The lifted-code benchmark's executable (tools/bench, XBA-37/38).
 *
 *   bench <image.bin> [reps] [scale] [only]
 *
 * BENCH_SHARED: the Android layout. Nothing is linked in: libbenchrt.so (the
 * runtime, as libmain.so) is dlopened, then libtitle.so (the lifted C) is
 * dlopened and its recomp_lookup dlsym'd, as the pack loader does
 * (src/platform/recomp_pack.c). The guest registers are then thread-locals of
 * a dlopened library, reached through the dynamic TLS path.
 * Otherwise the runtime and the lifted C are linked into this executable.
 *
 * BENCH_CPU_SLOT (variant cpuslot, BENCH_SHARED only): the lifted C reaches g_cpu through bionic's TLS_SLOT_APP
 * (recomp_types.h RECOMP_CPU_SLOT, XBA-273), tp + 16 on arm64. On aarch64 glibc tp + 16 is where the executable's
 * own TLS block starts (a 16-byte TCB, then the block), so this executable's only thread-local stands in for the
 * slot: the same instructions as on Android, checked below. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
typedef int (*bench_run_t)(recomp_func_t (*)(uint32_t), int (*)(void),
                           const unsigned char *, size_t, int, double, const char *);

#ifdef BENCH_SHARED
#include <dlfcn.h>
#include <libgen.h>
#ifdef BENCH_CPU_SLOT
#if !defined(__aarch64__)
#error "BENCH_CPU_SLOT: arm64 only (bionic's TLS_SLOT_APP is emulated with aarch64 glibc's TLS layout)"
#endif
__thread void *bench_cpu_slot;
#endif
#else
int bench_run(recomp_func_t (*)(uint32_t), int (*)(void),
              const unsigned char *, size_t, int, double, const char *);
recomp_func_t recomp_lookup(uint32_t);
int recomp_dispatch_init(void);
#endif

int main(int argc, char **argv)
{
    unsigned char *image;
    long len;
    FILE *f;
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    double scale = argc > 3 ? atof(argv[3]) : 1.0;
    const char *only = argc > 4 ? argv[4] : NULL;
    if (argc < 2) { fprintf(stderr, "usage: %s image.bin [reps] [scale] [only]\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = malloc((size_t)len);
    if (!image || fread(image, 1, (size_t)len, f) != (size_t)len) { perror("read"); return 2; }
    fclose(f);
#ifdef BENCH_SHARED
    {
        char dir[4096], path[4200];
        void *rt, *title;
        bench_run_t run;
        recomp_func_t (*lookup)(uint32_t);
        int (*init)(void);
#ifdef BENCH_CPU_SLOT
        if ((void *)&bench_cpu_slot != (void *)((void **)__builtin_thread_pointer() + 2)) {
            fprintf(stderr, "bench: the stand-in for TLS_SLOT_APP is not at tp + 16\n");
            return 2;
        }
#endif
        strncpy(dir, argv[0], sizeof dir - 1);
        dir[sizeof dir - 1] = 0;
        snprintf(path, sizeof path, "%s/libbenchrt.so", dirname(dir));
        rt = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!rt) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        strncpy(dir, argv[0], sizeof dir - 1);
        snprintf(path, sizeof path, "%s/libtitle.so", dirname(dir));
        title = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!title) { fprintf(stderr, "%s\n", dlerror()); return 2; }
        run = (bench_run_t)dlsym(rt, "bench_run");
        lookup = (recomp_func_t (*)(uint32_t))dlsym(title, "recomp_lookup");
        init = (int (*)(void))dlsym(title, "recomp_dispatch_init");
        if (!run || !lookup) { fprintf(stderr, "bench: missing bench_run / recomp_lookup\n"); return 2; }
        return run(lookup, init, image, (size_t)len, reps, scale, only);
    }
#else
    return bench_run(recomp_lookup, recomp_dispatch_init, image, (size_t)len, reps, scale, only);
#endif
}
