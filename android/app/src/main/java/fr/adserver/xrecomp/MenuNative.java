package fr.adserver.xrecomp;

/**
 * The in-game menu's native side (android/app/src/main/cpp/menu_jni.c, in libmain.so: the game's process only, loaded
 * by SDLActivity before ShellActivity uses it).
 */
final class MenuNative {
    private MenuNative() {}

    /**
     * Holds (true) or lets go of (false) the game: the lifecycle's MENU reason (recomp_lifecycle.h), the same hold as
     * the background: the flip parks, the title's threads stop behind it, the audio device pauses.
     */
    static native void nativeHold(boolean on);

    /** True while the MENU reason holds the game. */
    static native boolean nativeHeld();

    /** The output volume, 0..100 (apu_volume.h); 0 mutes. */
    static native void nativeSetVolume(int percent);

    /** Lets the game go, syncs the saves to disk, then recomp_quit(0). Does not return. */
    static native void nativeQuit();

    /**
     * XBA-166: the PlayStation game being played gets another disc (ps1_runtime_change_disc: the lid opens, a second
     * later the disc is in and the lid closes). path: a file path or « fd:N » (a detached descriptor, left open).
     * False when no PS1 game runs.
     */
    static native boolean nativePs1ChangeDisc(String path);
}
