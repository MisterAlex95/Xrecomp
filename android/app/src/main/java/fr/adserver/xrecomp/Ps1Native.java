package fr.adserver.xrecomp;

/**
 * JNI side of the PlayStation title key of a .chd disc (XBA-159; libxba_import.so, android/app/src/main/cpp/ps1_jni.c):
 * recomp_pack_rom_identify with the PS1 disc reader (ps1_disc.c + the vendored libchdr) as its disc hook, and the same
 * reader's raw sectors for a .chd's Prepare (XBA-168, {@link #openDisc}). A CHD's hunks
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

    /** A disc image open in the native reader (ps1_disc.c: .cue / .bin / .iso / .chd), as raw 2352-byte sectors. */
    interface Disc extends java.io.Closeable {
        /** Sectors on the disc. */
        int sectors();

        /** The raw sector at absolute LBA lba (MSF 00:02:00 = 0) into out[0..2352); false past the end. */
        boolean read(int lba, byte[] out) throws java.io.IOException;

        @Override
        void close();
    }

    /** Opens a disc image in the native reader; a test substitutes its own (no native library in a JVM test). */
    interface DiscOpener {
        /** The disc at path (a file path or « fd:N », left open), or null when the reader is not there. */
        Disc open(String path) throws java.io.IOException;
    }

    /**
     * XBA-168: a .chd's sectors for a Prepare (Ps1Disc.extract reads the boot EXE through it into the sparse disc.iso
     * tools.prepare reads), through the reader the game itself uses: every codec libchdr has (zlib, LZMA, zstd, FLAC).
     */
    static volatile DiscOpener discOpener = Ps1Native::openDisc;

    /** The native reader's disc at path; null without the library; IOException when it cannot be opened. */
    static Disc openDisc(String path) throws java.io.IOException {
        if (path == null || !load())
            return null;
        final long h;
        try {
            h = discOpenNative(path);
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
        if (h == 0)
            throw new java.io.IOException(path + ": not a disc image the PlayStation reader opens");
        return new Disc() {
            private long handle = h;

            @Override
            public synchronized int sectors() {
                return handle == 0 ? 0 : discSizeNative(handle);
            }

            @Override
            public synchronized boolean read(int lba, byte[] out) throws java.io.IOException {
                if (handle == 0)
                    throw new java.io.IOException("disc closed");
                return discReadNative(handle, lba, out) == 0;
            }

            @Override
            public synchronized void close() {
                if (handle != 0)
                    discCloseNative(handle);
                handle = 0;
            }
        };
    }

    @SuppressWarnings("JavaJniMissingFunction")
    private static native String titleKeyNative(String path);

    @SuppressWarnings("JavaJniMissingFunction")
    private static native long discOpenNative(String path);

    @SuppressWarnings("JavaJniMissingFunction")
    private static native int discSizeNative(long handle);

    @SuppressWarnings("JavaJniMissingFunction")
    private static native int discReadNative(long handle, int lba, byte[] out);

    @SuppressWarnings("JavaJniMissingFunction")
    private static native void discCloseNative(long handle);
}
