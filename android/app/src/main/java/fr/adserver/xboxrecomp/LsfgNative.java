package fr.adserver.xboxrecomp;

/**
 * JNI side of "Choose Lossless.dll" (libxba_import.so, android/app/src/main/cpp/lsfg_jni.c, around
 * src/framegen/lsfg_cache.c). The launcher UI that calls it comes later; docs/technical/framegen.md.
 *
 * Result arrays: { status, rcdata, dxbc[3.1], dxbc[3.1p], translated[3.1], translated[3.1p], fp16[3.1], fp16[3.1p],
 * fp32[3.1], fp32[3.1p], needed[3.1], needed[3.1p], ready[3.1], ready[3.1p] }. status 0 = ok.
 */
final class LsfgNative {
    static {
        System.loadLibrary("xba_import");
    }

    static final int STATUS = 0, RCDATA = 1, DXBC = 2, TRANSLATED = 4, FP16 = 6, FP32 = 8, NEEDED = 10, READY = 12;

    private LsfgNative() {}

    /**
     * Extracts (and, when built with the translator, translates) the LSFG shaders of the picked DLL into
     * baseDir/key. The fd stays open (the caller closes its ParcelFileDescriptor). dirOut[0] gets the cache
     * directory (RECOMP_LSFG_DIR), errOut[0] a one-line reason.
     */
    static native long[] importDll(int fd, String baseDir, String[] dirOut, String[] errOut);

    static native long[] readManifest(String dir);

    /** baseDir/key of the last import, or null. */
    static native String currentDir(String baseDir);

    /** True when this APK translates DXBC itself (else the cache needs tools/lsfg/lsfg_translate). */
    static native boolean hasTranslator();

    static native String strerror(int err);
}
