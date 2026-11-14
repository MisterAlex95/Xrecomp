package fr.adserver.xrecomp;

/**
 * JNI side of the PlayStation title key of a .chd disc (XBA-159; libxba_import.so, android/app/src/main/cpp/ps1_jni.c):
 * recomp_pack_rom_identify with the PS1 disc reader (ps1_disc.c + the vendored libchdr) as its disc hook. A CHD's hunks
 * are zlib, LZMA, zstd or FLAC compressed, and only the first is in the JDK: the native reader the game process uses is
 * the one robust reader, and the launcher's process already loads libxba_import.so (XisoNative), no SDL.
 */
final class Ps1Native {
    private static volatile Boolean loaded;

    private Ps1Native() {}

    private static boolean load() {
        Boolean l = loaded;
        if (l == null) {
            try {
                System.loadLibrary("xba_import");
                l = Boolean.TRUE;
            } catch (UnsatisfiedLinkError | SecurityException e) {
                l = Boolean.FALSE;   // a JVM test, or an APK built without it: CHD discs stay « other files »
            }
            loaded = l;
        }
        return l;
    }

    /**
     * The PS1 title key of the disc at path (a file path, or « fd:N » for an open descriptor, left open), « SLUS-00594 »
     * or « PSX-&lt;crc&gt; »; null when it is no PlayStation disc or the native library is not there.
     */
    static String titleKey(String path) {
        if (path == null || !load())
            return null;
        try {
            return titleKeyNative(path);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    @SuppressWarnings("JavaJniMissingFunction")
    private static native String titleKeyNative(String path);
}
