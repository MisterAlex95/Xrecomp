package fr.adserver.xboxrecomp;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.os.Bundle;
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
        if (dir != null) {
            try {
                Os.setenv("RECOMP_GAME_DIR", dir, true);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set RECOMP_GAME_DIR", e);
            }
        }
        applyLanguage();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries)
            return;   // SDLActivity shows its error dialog and finishes
        SystemBars.hide(this);   // full screen: no status bar, no gesture pill (XBA-99)
        applyGameTheme();
        menu = new GameMenu(this, new MenuHost());
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

    @Override
    protected void onStart() {
        super.onStart();
        if (companion != null)
            companion.start();
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
        if (menu != null && menu.dispatchKey(e))
            return true;
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (menu != null && menu.dispatchMotion(e))
            return true;
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
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
            if (panel == null)
                return;   // no panel: the overlay is off already (onStop, CompanionPanel.onDetachedFromWindow)
            panel.releaseAll();
            try {
                CompanionNative.nativeOverlay(0, 0);   // still on (a stand-in pad), nothing held
            } catch (UnsatisfiedLinkError ignored) {
            }
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

    private static void overlayOff() {
        try {
            CompanionNative.nativeOverlayOff();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private final class NativeSink implements CompanionPanel.Sink {
        @Override
        public void overlay(int buttons, int analogMask) {
            try {
                CompanionNative.nativeOverlay(buttons, analogMask);
            } catch (UnsatisfiedLinkError ignored) {
            }
        }

        @Override
        public void release() {
            overlayOff();
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
