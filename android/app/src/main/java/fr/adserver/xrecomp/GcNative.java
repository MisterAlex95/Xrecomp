package fr.adserver.xrecomp;

/**
 * JNI side of a GameCube Prepare's disc (XBA-264; libxba_import.so, android/app/src/main/cpp/gc_jni.c): the sparse GCM
 * tools.prepare reads (the header, the apploader, the FST, the main DOL and the REL modules, the rest left as holes),
 * written by the disc readers the game itself uses (ISO / GCM, CISO, GCZ, RVZ, WIA), from a path or a document's
 * descriptor. RVZ's Zstandard / LZMA groups are not in the JDK, and a library folder's document has no path
 * tools.prepare could open.
 */
final class GcNative {
    private static volatile Boolean loaded;

    private GcNative() {}

    private static boolean load() {
        Boolean l = loaded;
        if (l == null) {
            try {
                System.loadLibrary("xba_import");
                l = Boolean.TRUE;
            } catch (UnsatisfiedLinkError | SecurityException e) {
                l = Boolean.FALSE;   // a JVM test, or an APK built without it
            }
            loaded = l;
        }
        return l;
    }

    /** Writes a sparse GCM; a test substitutes its own (no native library in a JVM test). */
    interface Extractor {
        /**
         * The disc at path (a file path or « fd:N », left open) as a sparse GCM at out: « OK &lt;key&gt; &lt;rels&gt;
         * &lt;dol bytes&gt; », « ERR &lt;why&gt; », or null when the native reader is not there.
         */
        String extract(String path, String out);
    }

    static volatile Extractor extractor = GcNative::extract;

    static String extract(String path, String out) {
        if (path == null || out == null || !load())
            return null;
        try {
            return extractNative(path, out);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    private static native String extractNative(String path, String out);
}
