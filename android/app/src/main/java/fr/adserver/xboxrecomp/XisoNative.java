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

    /**
     * Copies one file of the image (pathInImage: '/' or '\\' separators, case-insensitive) to outFile, without
     * extracting the rest: the game library takes default.xbe out of an ISO this way. outFile must not exist (O_EXCL)
     * and is removed on failure. Returns 0, ENOENT when the image has no such file, EINVAL when it is larger than
     * maxBytes, or another XISO_E* code.
     */
    static native int extractFile(int fd, String pathInImage, String outFile, long maxBytes);

    // ---- XBA-126: an image given as its parts in order (one fd for a plain or compressed file) ----

    /**
     * probe() over the parts of one image (a split .1/.2, a .cso, ...): { status, total bytes of the files, 1 if
     * default.xbe is in the root, kind (KIND_*, | KIND_SPLIT), bytes stored (the parts' sizes) }.
     */
    static native long[] probeParts(int[] fds, String[] titleOut);

    /** extract() over the parts of one image. 0 or an XISO_E* code. */
    static native int extractParts(int[] fds, String outDir, Progress progress);

    /** extractFile() over the parts of one image. */
    static native int extractFileParts(int[] fds, String pathInImage, String outFile, long maxBytes);

    /**
     * Copies every *.xbe of the image (any directory, at most 16, each at most maxBytes) into outDir, keeping their
     * directories, and nothing else: the companion folder of an image read in place. Returns the count, or an
     * XISO_E* code (negative).
     */
    static native int extractXbes(int[] fds, String outDir, long maxBytes);

    /** src/xiso/xiso.h XISO_KIND_*. */
    static final int KIND_PLAIN = 1, KIND_CSO = 2, KIND_ZSO = 3, KIND_CCI = 4, KIND_SPLIT = 0x100;

    /** XISO_* codes (src/xiso/xiso.h), for the callers that tell them apart. */
    static final int ENOTXISO = -2, ECORRUPT = -3, EINVAL = -8, ENOENT = -9;

    static native String strerror(int err);

    static native int canceledCode();

    static native int noSpaceCode();
}
