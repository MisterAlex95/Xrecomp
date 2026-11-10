package fr.adserver.xboxrecomp;

/**
 * The companion panel's native side (android/app/src/main/cpp/companion_jni.c, in libmain.so: the game's process
 * only, loaded by SDLActivity before ShellActivity uses it).
 */
final class CompanionNative {
    private CompanionNative() {}

    /** The touch overlay pad on port 0 (input_overlay.h): wButtons bits, analog mask (bit i = button i at 255). */
    static native void nativeOverlay(int buttons, int analogMask);

    /** Overlay off: nothing held, no stand-in pad. */
    static native void nativeOverlayOff();

    /** { fps x 100, draws of the last frame } (xbox_FramebufferWindowGetStats; zeros before the window runs). */
    static native int[] nativeStats();

    /** The last frame as a 24-bit BMP (xbox_FramebufferDumpBmp); false when there is no frame or it failed. */
    static native boolean nativeScreenshot(String path);

    /** Ends the game as Back twice does: recomp_quit(0) (flush, drain logcat, _exit). Does not return. */
    static native void nativeQuit();
}
