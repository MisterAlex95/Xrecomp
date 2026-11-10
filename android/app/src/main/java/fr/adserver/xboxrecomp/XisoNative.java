package fr.adserver.xboxrecomp;

/** JNI side of the ISO import (libxba_import.so, android/app/src/main/cpp/xiso_jni.c, around src/xiso). */
final class XisoNative {
    static {
        System.loadLibrary("xba_import");
    }

    interface Progress {
        /** Called on the extracting thread. Returns true to cancel. path is relative, "" on the final call. */
        boolean onProgress(long done, long total, String path);
    }

    private XisoNative() {}

    /** Returns { status (0 or an XISO_E* code), total bytes of the files, 1 if default.xbe is in the root }. */
    static native long[] probe(int fd, String[] titleOut);

    /** Extracts into outDir (through outDir.partial; an existing outDir is replaced). 0 or an XISO_E* code. */
    static native int extract(int fd, String outDir, Progress progress);

    static native String strerror(int err);

    static native int canceledCode();

    static native int noSpaceCode();
}
