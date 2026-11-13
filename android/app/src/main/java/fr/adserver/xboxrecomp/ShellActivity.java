package fr.adserver.xboxrecomp;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The app shell every title uses: SDL loads libSDL2.so, then libmain.so, and calls its SDL_main.
 * Started by LauncherActivity (the launcher and game picker, XBA-41 / XBA-54) once a game is set up.
 *
 * On a device with a second screen (AYN Thor) the in-game CompanionPanel goes there (CompanionScreen; shown in
 * onStart, dismissed in onStop; docs/android/app-shell.md « Dual screen »).
 *
 * The in-game menu (GameMenu, docs/android/app-shell.md « In-game menu ») is a View layer over SDL's surface: every
 * key, joystick and touch event passes through it first (dispatchKeyEvent, dispatchGenericMotionEvent,
 * dispatchTouchEvent), and what it takes never reaches SDL.
 */
public class ShellActivity extends SDLActivity {
    private CompanionScreen companion;
    private CompanionPanel panel;
    private GameMenu menu;
    /** The overlay pad both the companion panel and the on-screen pad drive (XBA-42). */
    private final OverlayMixer mixer = new OverlayMixer(new NativeOverlay());
    private TouchControls touch;
    /** « shell: fps N » every 10 s while started, panel or not (Device check: fps with the panel on and off). */
    private final android.os.Handler fpsLog = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            int[] st = stats();
            if (st != null && st[0] > 0)
                Log.i(LauncherActivity.TAG, String.format(Locale.ROOT, "shell: fps %.1f, %d draws, companion %s",
                                                          st[0] / 100f, st[1],
                                                          companion != null && companion.isShowing() ? "shown" : "off"));
            fpsLog.postDelayed(this, 10_000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        /* The chosen game folder, as RECOMP_GAME_DIR, before SDLActivity starts anything: SDL_main runs later on
         * SDLThread, and recomp_app_start reads it in recomp_storage_resolve. The process environment is libc's, so
         * native getenv sees it. Set here, it wins over a RECOMP_GAME_DIR line in recomp.cfg. */
        String dir = GameStore.chosenDir(this);
        /* A game of another console (XBA-147) is a file, handed over as it is: the native side reads its console and
         * title key and gives its pack to that console's runtime (pack_loader.c). Said here for the device check. */
        if (dir != null && new File(dir).isFile()) {
            Pack.Route r = Pack.route(getFilesDir(), new File(dir));
            if (r != null && r.console != Console.XBOX)
                Log.i(LauncherActivity.TAG, "shell: " + r.console.id + " game " + r.key + ", pack " + r.check);
        }
        /* A disc image read in place (XBA-126): the folder is its companion (XBEs and disc.json); the runtime gets
         * the image itself, its path or fds opened here from the persisted documents. Opened again at every
         * process start, so a quick reboot or a hand-over to another image (both a new process) finds it too.
         * Unreachable (SD card out, permission gone): "fd:-1", which the runtime reports as no game data. */
        DiscImages.Disc disc = dir == null ? null : DiscImages.read(new java.io.File(dir));
        if (disc != null) {
            String spec = DiscImages.runtimeSpec(disc, DiscImages.contentOpener(this));
            Log.i(LauncherActivity.TAG, "shell: disc image " + DiscImages.label(disc) + " (" + disc.kind + ") -> "
                                        + (spec != null ? spec : "unreachable"));
            dir = spec != null ? spec : "fd:-1";
        }
        if (dir != null) {
            try {
                Os.setenv("RECOMP_GAME_DIR", dir, true);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set RECOMP_GAME_DIR", e);
            }
        }
        applyTitleVideo();
        applyLanguage();
        boolean checked = LibraryCheck.applyRunEnv(this);   // a library check's run: status file and pad script (XBA-130)
        applyBuiltinPad();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries)
            return;   // SDLActivity shows its error dialog and finishes
        SystemBars.hide(this);   // full screen: no status bar, no gesture pill (XBA-99)
        applyGameTheme();
        menu = new GameMenu(this, new MenuHost());
        touch = new TouchControls(this, mixer, () -> PadDevices.any(this));
        touch.checkRun(checked);   // never the on-screen pad during a library check
        touch.attach();   // under the menu
        menu.attach();
        companion = new CompanionScreen(this, "shell", ctx -> {
            panel = new CompanionPanel(ctx, new NativeSink(), ShellActivity::stats);
            return panel;
        }, new KeyForwarder());
    }

    /**
     * The game theme's colours for the in-game menu and the bottom screen's panel (docs/android/app-shell.md « Game
     * theme »), before they are built: from the launcher's cache, read once at game start; tokens only, no bitmaps.
     * None cached, or the player's « Off »: the stock colours.
     */
    private void applyGameTheme() {
        String tid = GameLanguage.currentTitleId(this);
        GameTheme g = null;
        try {
            g = GameTheme.load(this, tid);
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: theme unreadable, stock colours", e);
        }
        Theme.apply(g != null ? g.colors : Theme.Colors.STOCK);
        Log.i(LauncherActivity.TAG, g != null ? "shell: " + g.logLine().substring("launcher: ".length())
                                              : "shell: theme " + tid + ": stock");
    }

    /**
     * RECOMP_PAD_BUILTIN (XBA-123, src/input/input_ports.h): the handheld's own pad (the gamepad that is not external,
     * PadDevices) is port 1 unless the Controls screen pins another there. Unset on a phone.
     */
    private void applyBuiltinPad() {
        String key = PadPorts.builtinKey(PadDevices.connected(this));
        try {
            if (key != null)
                Os.setenv(PadPorts.BUILTIN_ENV, key, true);
            else
                Os.unsetenv(PadPorts.BUILTIN_ENV);
        } catch (ErrnoException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot set " + PadPorts.BUILTIN_ENV, e);
        }
        Log.i(LauncherActivity.TAG, "shell: controllers " + PadDevices.connected(this)
                                    + (key != null ? ", built-in " + key : ""));
    }

    /**
     * RECOMP_LANGUAGE for the runtime (XBA-77, {@link GameLanguage}): the title's own choice, else none when recomp.cfg
     * has a line (the runtime reads it there), else the Android language when the Xbox has it. Unset otherwise, also
     * clearing a value a previous game left in this process.
     */
    private void applyLanguage() {
        File ext = GameStore.externalFilesDir(this);
        String cfg = ext == null ? null : RecompConfig.load(ext).get(GameLanguage.ENV);
        String tid = GameLanguage.currentTitleId(this);
        String lang = GameLanguage.resolve(GameLanguage.titleChoice(this, tid), cfg, Locale.getDefault());
        try {
            if (lang != null)
                Os.setenv(GameLanguage.ENV, lang, true);
            else
                Os.unsetenv(GameLanguage.ENV);
        } catch (ErrnoException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot set " + GameLanguage.ENV, e);
        }
        Log.i(LauncherActivity.TAG, "shell: game language " + (lang != null ? lang : cfg != null ? cfg + " (recomp.cfg)"
                                                              : "en (default)") + ", title " + tid);
    }

    /**
     * RECOMP_WIDESCREEN and RECOMP_480P for the runtime (XBA-134, {@link GameVideo}): « 1 » / « 0 » when the title has
     * its own choice, else unset (recomp.cfg, then off, decide), also clearing what a previous game left.
     */
    private void applyTitleVideo() {
        String tid = GameLanguage.currentTitleId(this);
        StringBuilder line = new StringBuilder("shell: title video");
        for (GameVideo.Setting s : GameVideo.Setting.values()) {
            String v = GameVideo.resolve(GameVideo.titleChoice(this, tid, s));
            try {
                if (v != null)
                    Os.setenv(s.env, v, true);
                else
                    Os.unsetenv(s.env);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set " + s.env, e);
            }
            line.append(' ').append(s.env).append('=').append(v != null ? v : "(Settings)");
        }
        Log.i(LauncherActivity.TAG, line.append(", title ").append(tid).toString());
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (companion != null)
            companion.start();
        if (touch != null)
            touch.start();
        fpsLog.removeCallbacks(fpsTick);
        fpsLog.postDelayed(fpsTick, 10_000);
        if (menu != null)
            menu.onStart();
        /* The companion Presentation (shown in onStart above) takes the focus for a moment on some devices and the
         * bars came back with it: hide them again once it is up. */
        getWindow().getDecorView().postDelayed(() -> SystemBars.hide(this), 300);
    }

    @Override
    protected void onResume() {
        super.onResume();
        SystemBars.hide(this);
        if (menu != null)
            menu.onResume();
    }

    @Override
    protected void onStop() {
        fpsLog.removeCallbacks(fpsTick);
        if (companion != null)
            companion.stop();   // the panel's onDetachedFromWindow releases its buttons
        if (touch != null)
            touch.stop();       // hidden, fingers released
        overlayOff();           // and again here, whatever happened to the panel
        panel = null;
        super.onStop();
        if (menu != null)
            menu.onStop();      // after SDL's pause: a Back to launcher hands the hold to the background
    }

    @Override
    protected void onDestroy() {
        if (menu != null)
            menu.release();
        super.onDestroy();
    }

    // ---- The in-game menu's ends --------------------------------------------------------------------------------

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (touch != null)
            touch.onKey(e);   // a controller used hides the Auto on-screen pad
        if (menu != null && menu.dispatchKey(e))
            return true;
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (touch != null)
            touch.onMotion(e);
        if (menu != null && menu.dispatchMotion(e))
            return true;
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (touch != null && e.getActionMasked() == MotionEvent.ACTION_DOWN)
            touch.onTouch();
        if (menu != null && menu.dispatchTouch(e))
            return true;
        return super.dispatchTouchEvent(e);
    }

    private final class MenuHost implements GameMenu.Host {
        @Override
        public void hold(boolean on) {
            try {
                MenuNative.nativeHold(on);
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native hold", e);
            }
        }

        @Override
        public void sendKeyToGame(KeyEvent e) {
            // Straight to SDL's handler: the focus may be on the drawer's rows at this point.
            SDLActivity.handleKeyEvent(mSurface, e.getKeyCode(), e, null);
        }

        @Override
        public void sendMotionToGame(MotionEvent e) {
            // SDL's surface handles joystick motion (SDLControllerManager): the focus may be on the drawer's rows.
            if (mSurface != null)
                mSurface.dispatchGenericMotionEvent(e);
        }

        @Override
        public void cancelGameTouch(MotionEvent e) {
            MotionEvent c = MotionEvent.obtain(e);
            c.setAction(MotionEvent.ACTION_CANCEL);
            ShellActivity.super.dispatchTouchEvent(c);
            c.recycle();
        }

        @Override
        public void releaseOverlay() {
            if (touch != null)
                touch.view.releaseAll();   // the on-screen pad: nothing held, still shown
            if (panel == null)
                return;   // no panel: its part of the overlay is off already (onStop, onDetachedFromWindow)
            panel.releaseAll();
            mixer.panel(0, 0);   // still on (a stand-in pad), nothing held
        }

        @Override
        public void restoreGameFocus() {
            if (mSurface != null)
                mSurface.requestFocus();
        }

        @Override
        public boolean fromGamepad(KeyEvent e) {
            return SDLControllerManager.isDeviceSDLJoystick(e.getDeviceId());
        }

        @Override
        public void setVolume(int percent) {
            try {
                MenuNative.nativeSetVolume(percent);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }

        @Override
        public int[] stats() {
            return ShellActivity.stats();
        }

        @Override
        public boolean hasSecondScreen() {
            return DualScreen.available(ShellActivity.this, CompanionScreen.ownDisplayId(ShellActivity.this));
        }

        @Override
        public void bottomScreenChanged() {
            if (companion != null)
                companion.refresh();
        }

        @Override
        public void screenshot() {
            ShellActivity.this.screenshot();
        }

        @Override
        public void shareLog() {
            ShellActivity.this.shareLog();
        }

        @Override
        public void backToLauncher() {
            openLauncher();
        }

        @Override
        public void quit() {
            overlayOff();
            try {
                MenuNative.nativeQuit();
            } catch (UnsatisfiedLinkError e) {
                finish();
            }
        }
    }

    /** The launcher on top; this task (the game) stays, and the launcher's Play brings it back. */
    private void openLauncher() {
        startActivity(new Intent(this, LauncherActivity.class)
                .setAction(LauncherActivity.ACTION_SHOW_LAUNCHER).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    /**
     * Plan B (RECOMP_COMPANION_FOCUS=forward): the focusable panel takes the focus when touched, and hands its key
     * and motion events back (KeyForwarder); the game must not pause for that.
     */
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus && companion != null && companion.holdsFocus()) {
            Log.i(LauncherActivity.TAG, "shell: focus went to the companion panel, ignored (focus=forward)");
            return;
        }
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            SystemBars.hide(this);   // after SDL's own handling, which uses the old flags
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    // ---- The companion panel's ends -------------------------------------------------------------------------------

    private static int[] stats() {
        try {
            return CompanionNative.nativeStats();
        } catch (UnsatisfiedLinkError e) {
            return null;
        }
    }

    private void overlayOff() {
        mixer.off();
    }

    /** The overlay pad (input_overlay.h) through CompanionNative. */
    private static final class NativeOverlay implements OverlayMixer.Sink {
        @Override
        public void pad(int buttons, int analogMask, int lt, int rt, int lx, int ly, int rx, int ry) {
            try {
                CompanionNative.nativeOverlayPad(buttons, analogMask, lt, rt, lx, ly, rx, ry);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }

        @Override
        public void off() {
            try {
                CompanionNative.nativeOverlayOff();
            } catch (UnsatisfiedLinkError ignored) {
            }
        }
    }

    private final class NativeSink implements CompanionPanel.Sink {
        @Override
        public void overlay(int buttons, int analogMask) {
            mixer.panel(buttons, analogMask);
            if (touch != null)
                touch.panelShowing(true);   // never the on-screen pad while the panel is up
        }

        @Override
        public void release() {
            mixer.panelOff();
            if (touch != null)
                touch.panelShowing(false);
        }

        @Override
        public void action(int id) {
            switch (id) {
            case CompanionPanel.LAUNCHER:
                Log.i(LauncherActivity.TAG, "shell: companion: launcher");
                if (menu != null && menu.isOpen())
                    menu.close();
                openLauncher();
                break;
            case CompanionPanel.MENU:
                Log.i(LauncherActivity.TAG, "shell: companion: menu");
                if (menu != null)
                    menu.toggle("panel");
                break;
            case CompanionPanel.QUIT:
                Log.i(LauncherActivity.TAG, "shell: companion: quit (held)");
                overlayOff();
                try {
                    CompanionNative.nativeQuit();
                } catch (UnsatisfiedLinkError e) {
                    finish();
                }
                break;
            case CompanionPanel.SCREENSHOT:
                screenshot();
                break;
            case CompanionPanel.SHARE_LOG:
                shareLog();
                break;
            default:
                break;   // PERF: the panel's own details
            }
        }
    }

    /** <external files>/screenshots/<date>.bmp, written off the UI thread; the panel says how it went. */
    private void screenshot() {
        File ext = getExternalFilesDir(null);
        File dir = new File(ext == null ? getFilesDir() : ext, "screenshots");
        String name = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + ".bmp";
        File out = new File(dir, name);
        new Thread(() -> {
            boolean ok;
            try {
                ok = (dir.isDirectory() || dir.mkdirs()) && CompanionNative.nativeScreenshot(out.getPath());
            } catch (UnsatisfiedLinkError e) {
                ok = false;
            }
            boolean saved = ok;
            Log.i(LauncherActivity.TAG, "shell: companion: screenshot " + (saved ? out : "failed"));
            runOnUiThread(() -> {
                String msg = getString(saved ? R.string.PANEL_SHOT_SAVED : R.string.PANEL_SHOT_NONE, name);
                if (panel != null)
                    panel.flash(msg);
                if (menu != null)
                    menu.flash(msg, saved);
            });
        }, "screenshot").start();
    }

    /** This process's logcat (the runtime's stdout and stderr go there) into a file, shared through LogProvider. */
    private void shareLog() {
        new Thread(() -> {
            File f = LogProvider.gameLogFile(this);
            boolean ok = LogProvider.dumpOwnLogcat(f);
            runOnUiThread(() -> {
                if (!ok) {
                    flashBoth(getString(R.string.SHARE_LOG_NONE));
                    return;
                }
                android.net.Uri uri = LogProvider.gameUri(this);
                Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                send.setClipData(android.content.ClipData.newRawUri("game.log", uri));
                try {
                    startActivity(Intent.createChooser(send, getString(R.string.SHARE_LOG)));
                } catch (ActivityNotFoundException e) {
                    flashBoth(getString(R.string.SHARE_LOG_NO_APP));
                }
            });
        }, "share-log").start();
    }

    private void flashBoth(String msg) {
        if (panel != null)
            panel.flash(msg);
        if (menu != null)
            menu.flash(msg, false);
    }

    /** Plan B: the panel's window hands its events to SDL as if they had come to this window. */
    private final class KeyForwarder implements CompanionScreen.KeyTarget {
        @Override
        public boolean forwardKey(KeyEvent e) {
            return ShellActivity.this.dispatchKeyEvent(e);
        }

        @Override
        public boolean forwardMotion(MotionEvent e) {
            // Joystick motion is not a pointer event: the window gives it to its focused view, SDL's surface.
            return ShellActivity.this.dispatchGenericMotionEvent(e);
        }
    }

    /**
     * Called by shell_main.c (SDLThread, through JNI) when default.xbe is not in the game folder (NO_GAME_DATA): the
     * picker opens naming that folder, and this activity finishes (SDL_main then gets SDL_QUIT and returns).
     */
    @SuppressWarnings("unused")
    public void openGamePicker(String gameDir) {
        runOnUiThread(() -> {
            Intent i = new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_NO_GAME_DATA, gameDir)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
            finish();
        });
    }

    /**
     * Called by shell_main.c (SDLThread, through JNI) when the title pack could not be loaded (PACK_INVALID,
     * PACK_DLOPEN; pack_loader.h): the launcher opens, drops the pack and offers Prepare again, and this activity
     * finishes.
     */
    @SuppressWarnings("unused")
    public void openLauncherBadPack(String reason) {
        runOnUiThread(() -> {
            Intent i = new Intent(this, LauncherActivity.class)
                    .putExtra(LauncherActivity.EXTRA_BAD_PACK, reason == null ? "" : reason)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
            finish();
        });
    }

    /** How long titleExited waits for the UI thread to have asked for the launcher. */
    static final long TITLE_EXIT_HANDOFF_MS = 2000;

    /**
     * Called by shell_main.c (the guest thread, through JNI) when the title left by itself: HalReturnToFirmware to
     * the dashboard, an image this build cannot start, a shutdown (src/kernel/kernel_reboot.c xbox_title_exit). The
     * launcher opens saying why (LauncherActivity.EXTRA_TITLE_EXITED) and this activity finishes. Returns once that
     * is asked (at most TITLE_EXIT_HANDOFF_MS): the native side then ends this process itself (recomp_quit).
     */
    @SuppressWarnings("unused")
    public void titleExited(String why) {
        String w = why == null ? "" : why;
        Log.i(LauncherActivity.TAG, "shell: the game exited by itself (" + w + "), back to the launcher");
        CountDownLatch asked = new CountDownLatch(1);
        Runnable open = () -> {
            try {
                startActivity(titleExitedIntent(this, w));
                finish();
                overridePendingTransition(0, 0);
            } finally {
                asked.countDown();
            }
        };
        if (Looper.myLooper() == Looper.getMainLooper()) {
            open.run();
            return;
        }
        runOnUiThread(open);
        try {
            if (!asked.await(TITLE_EXIT_HANDOFF_MS, TimeUnit.MILLISECONDS))
                Log.w(LauncherActivity.TAG, "shell: the launcher was not asked within " + TITLE_EXIT_HANDOFF_MS + " ms");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** The launcher, on Home with the title's reason for leaving. */
    static Intent titleExitedIntent(Context c, String why) {
        return new Intent(c, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_TITLE_EXITED, why == null ? "" : why)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
    }

    /**
     * Called by shell_main.c (the guest thread, through JNI) when the title quick-rebooted into itself
     * (kernel_reboot.c): the launcher process ends this one and starts this activity again in a new process. The
     * native side waits to be ended.
     */
    @SuppressWarnings("unused")
    public void restartGame(String unused) {
        runOnUiThread(() -> {
            Log.i(LauncherActivity.TAG, "shell: quick reboot, asking the launcher to restart the game");
            startActivity(new Intent(this, LauncherActivity.class)
                    .setAction(LauncherActivity.ACTION_RESTART_GAME)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            overridePendingTransition(0, 0);
        });
    }
}
