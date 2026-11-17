package fr.adserver.xrecomp;

/**
 * The companion panel's native side (android/app/src/main/cpp/companion_jni.c, in libmain.so: the game's process
 * only, loaded by SDLActivity before ShellActivity uses it).
 */
final class CompanionNative {
    private CompanionNative() {}

    /** The touch overlay pad on port 0 (input_overlay.h): wButtons bits, analog mask (bit i = button i at 255). */
    static native void nativeOverlay(int buttons, int analogMask);

    /**
     * The whole overlay pad on port 0 (XBA-42, OverlayMixer): wButtons bits, analog mask for A B X Y Black White (bit
     * i at 255), triggers 0..255, sticks -32767..32767 with Y up.
     */
    static native void nativeOverlayPad(int buttons, int analogMask, int lt, int rt, int lx, int ly, int rx, int ry);

    /** "1\tname" lines for the ports with a host controller now (XBA-123, xbox_InputPortName). */
    static native String nativePorts();

    /** Overlay off: nothing held, no stand-in pad. */
    static native void nativeOverlayOff();

    /** { fps x 100, draws of the last frame } (xbox_FramebufferWindowGetStats; zeros before the window runs). */
    static native int[] nativeStats();

    /** The last frame as a 24-bit BMP (xbox_FramebufferDumpBmp); false when there is no frame or it failed. */
    static native boolean nativeScreenshot(String path);
}
