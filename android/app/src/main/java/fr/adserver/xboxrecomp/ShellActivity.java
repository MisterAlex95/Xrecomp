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
 */
public class ShellActivity extends SDLActivity {
    private CompanionScreen companion;
    private CompanionPanel panel;
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
        super.onCreate(savedInstanceState);
        companion = new CompanionScreen(this, "shell", ctx -> {
            panel = new CompanionPanel(ctx, new NativeSink(), ShellActivity::stats);
            return panel;
        }, new KeyForwarder());
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (companion != null)
            companion.start();
        fpsLog.removeCallbacks(fpsTick);
        fpsLog.postDelayed(fpsTick, 10_000);
    }

    @Override
    protected void onStop() {
        fpsLog.removeCallbacks(fpsTick);
        if (companion != null)
            companion.stop();   // the panel's onDetachedFromWindow releases its buttons
        overlayOff();           // and again here, whatever happened to the panel
        panel = null;
        super.onStop();
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
                startActivity(new Intent(ShellActivity.this, LauncherActivity.class)
                        .setAction(LauncherActivity.ACTION_SHOW_LAUNCHER).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
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
                if (panel != null)
                    panel.flash(getString(saved ? R.string.PANEL_SHOT_SAVED : R.string.PANEL_SHOT_NONE, name));
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
                    if (panel != null)
                        panel.flash(getString(R.string.SHARE_LOG_NONE));
                    return;
                }
                android.net.Uri uri = LogProvider.gameUri(this);
                Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                        .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                send.setClipData(android.content.ClipData.newRawUri("game.log", uri));
                try {
                    startActivity(Intent.createChooser(send, getString(R.string.SHARE_LOG)));
                } catch (ActivityNotFoundException e) {
                    if (panel != null)
                        panel.flash(getString(R.string.SHARE_LOG_NO_APP));
                }
            });
        }, "share-log").start();
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
}
