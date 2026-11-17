package fr.adserver.xrecomp;

/**
 * The N64 shader warm-up's native side (android/app/src/main/cpp/warmup_jni.c, in libmain.so), for
 * ShaderWarmupService in its own ":warmup" process: libSDL2.so and libmain.so are loaded there for it, no game runs.
 */
final class WarmupNative {
    private WarmupNative() {}

    /** Loads libmain.so (and libSDL2.so it needs) once; false when it cannot be loaded. */
    static synchronized boolean load() {
        if (loaded)
            return true;
        try {
            System.loadLibrary("SDL2");
            System.loadLibrary("main");
            loaded = true;
        } catch (UnsatisfiedLinkError | SecurityException e) {
            android.util.Log.w(LauncherActivity.TAG, "warmup: libmain.so cannot be loaded", e);
        }
        return loaded;
    }

    private static boolean loaded;

    /**
     * n64_rdp_vk_warmup(factor) with this process' environment (RECOMP_VK_DRIVER_*, RECOMP_VK_CACHE_DIR): { rc, factor,
     * start-up pipelines, extra pipelines, compiled 5 ms or more, cache KB read, ms }; rc 0 done, 1 the Vulkan RDP
     * cannot run on this driver, 2 no cache directory or memory, 3 no Vulkan RDP in this build. Blocks for seconds.
     */
    static native int[] nativeWarmup(int factor);

    /**
     * The GameCube warm-up (XBA-264, GcShaderWarmup): gx_warmup_headless(lists, cache), the TEV states of the shader
     * lists (missing files skipped) compiled on a headless EGL context and their program binaries written to cache:
     * { rc, states, already cached, compiled, failed, ms }; rc 0 done, 1 the driver has no program binaries, 2 no GLES 3
     * context, 3 the cache cannot be written, 4 bad arguments. Blocks (a second or so per hundred states).
     */
    static native int[] nativeGcWarmup(String[] lists, String cache);
}
