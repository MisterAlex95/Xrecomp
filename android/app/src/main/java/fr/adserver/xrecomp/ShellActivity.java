package fr.adserver.xrecomp;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Looper;
import android.provider.DocumentsContract;
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
        String rom = GameStore.launchedRom(this);
        String dir = rom != null ? rom : GameStore.chosenDir(this);
        /* A GBA ROM launched from the library (XBA-157) may be a document of a library folder seen through the Storage
         * Access Framework: opened here, its descriptor handed over as "fd:N" (detached: the game process keeps it for
         * its life; the runtime reads it from offset 0). Unreachable: "fd:-1", which the runtime reports. */
        java.util.List<String> parts = rom != null && rom.startsWith("content:") ? GameStore.launchedRomParts(this)
                                                                                : java.util.Collections.emptyList();
        if (!parts.isEmpty()) {
            /* A PS1 disc's .cue among documents (XBA-168): its tracks opened here and named in a .cue of our own as
             * /proc/self/fd/N (Ps1Cue); unreachable: "fd:-1", which the runtime reports. */
            String cue = Ps1Cue.runtimeCue(rom, parts, u -> getContentResolver().openInputStream(Uri.parse(u)),
                                           this::documentName, DiscImages.contentOpener(this),
                                           new File(getCacheDir(), "ps1"));
            Log.i(LauncherActivity.TAG, "shell: cue sheet " + rom + " (" + parts.size() + " track file(s)) -> "
                                        + (cue != null ? cue : "unreachable"));
            dir = cue != null ? cue : "fd:-1";
        } else if (rom != null && rom.startsWith("content:")) {
            String spec = romDescriptor(rom, DiscImages.contentOpener(this));
            Log.i(LauncherActivity.TAG, "shell: ROM document " + rom + " -> " + (spec != null ? spec : "unreachable"));
            dir = spec != null ? spec : "fd:-1";
        }
        /* A game of another console (XBA-147) is a file, handed over as it is: the native side reads its console and
         * title key and gives it to that console's runtime (pack_loader.c; the GBA needs no pack). Said here for the
         * device check. */
        if (dir != null && new File(dir).isFile()) {
            Pack.Route r = Pack.route(getFilesDir(), new File(dir));
            if (r != null && r.console != Console.XBOX)
                Log.i(LauncherActivity.TAG, "shell: " + r.console.id + " game " + r.key + ", pack " + r.check);
        }
        /* A disc image read in place (XBA-126): the folder is its companion (XBEs and disc.json); the runtime gets
         * the image itself, its path or fds opened here from the persisted documents. Opened again at every
         * process start, so a quick reboot or a hand-over to another image (both a new process) finds it too.
         * Unreachable (SD card out, permission gone): "fd:-1", which the runtime reports as no game data. */
        DiscImages.Disc disc = dir == null || rom != null ? null : DiscImages.read(new java.io.File(dir));
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
        applyN64Pak(rom != null);
        applyVkDriver(rom != null);
        Console console = applyConsolePad(rom != null);
        applyConsoleVideo(console);
        boolean checked = LibraryCheck.applyRunEnv(this);   // a library check's run: status file and pad script (XBA-130)
        applyBuiltinPad();
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries)
            return;   // SDLActivity shows its error dialog and finishes
        SystemBars.hide(this);   // full screen: no status bar, no gesture pill (XBA-99)
        applyGameTheme();
        menu = new GameMenu(this, new MenuHost());
        touch = new TouchControls(this, mixer, () -> PadDevices.any(this));
        touch.layout(TouchPad.layoutFor(console, ps1Analog()));   // the console's own buttons (XBA-158 / 168 / 174)
        touch.checkRun(checked);   // never the on-screen pad during a library check
        touch.attach();   // under the menu
        menu.attach();
        companion = new CompanionScreen(this, "shell", ctx -> {
            panel = new CompanionPanel(ctx, new NativeSink(), ShellActivity::stats);
            return panel;
        }, new KeyForwarder());
    }

    /** A document's display name (a PS1 track's file name, Ps1Cue), or null. */
    private String documentName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri),
                                                   new String[] { DocumentsContract.Document.COLUMN_DISPLAY_NAME },
                                                   null, null, null)) {
            return c != null && c.moveToFirst() ? c.getString(0) : null;
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: no name for " + uri, e);
            return null;
        }
    }

    /**
     * XBA-166: the discs of the PlayStation game being played (title ID « ps1-... »), for the menu's Change disc:
     * Ps1Discs over the files beside the launched disc (a path's folder; a document's folder listed through its tree,
     * else the library index's items of that folder). Empty for any other game.
     */
    private java.util.List<Ps1Discs.Disc> ps1Discs() {
        String rom = GameStore.launchedRom(this), tid = GameStore.launchedTitleId(this);
        if (rom == null || tid == null || !tid.startsWith("ps1-"))
            return java.util.Collections.emptyList();
        java.util.Map<String, String> sib = new java.util.LinkedHashMap<>();
        String current;
        if (!rom.startsWith("content:")) {
            File f = new File(rom);
            current = f.getName();
            File[] ls = f.getParentFile() != null ? f.getParentFile().listFiles() : null;
            if (ls != null)
                for (File x : ls)
                    if (x.isFile())
                        sib.put(x.getName(), x.getAbsolutePath());
            return Ps1Discs.find(current, sib, spec -> readText(new java.io.FileInputStream(spec)));
        }
        Uri u = Uri.parse(rom);
        String doc, tree;
        try {
            doc = DocumentsContract.getDocumentId(u);
            tree = DocumentsContract.getTreeDocumentId(u);
        } catch (RuntimeException e) {
            return java.util.Collections.emptyList();
        }
        Uri treeUri = DocumentsContract.buildTreeDocumentUri(u.getAuthority(), tree);
        String name = documentName(rom);
        current = name != null ? name : doc.substring(doc.lastIndexOf('/') + 1);
        int slash = doc.lastIndexOf('/');
        String parent = slash > 0 ? doc.substring(0, slash) : tree;
        try (Cursor c = getContentResolver().query(DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parent),
                new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                               DocumentsContract.Document.COLUMN_DISPLAY_NAME }, null, null, null)) {
            while (c != null && c.moveToNext())
                sib.put(c.getString(1), DocumentsContract.buildDocumentUriUsingTree(treeUri, c.getString(0)).toString());
        } catch (RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: the disc's folder cannot be listed, the library index instead", e);
        }
        if (sib.isEmpty())
            for (java.util.Map.Entry<String, String> e : Ps1Discs.siblingsFromIndex(
                     LibraryIndex.read(Library.indexFile(getFilesDir())), doc).entrySet())
                sib.put(e.getKey(), DocumentsContract.buildDocumentUriUsingTree(treeUri, e.getValue()).toString());
        return Ps1Discs.find(current, sib, spec -> readText(getContentResolver().openInputStream(Uri.parse(spec))));
    }

    private static String readText(java.io.InputStream in) throws java.io.IOException {
        if (in == null)
            throw new java.io.IOException("cannot open");
        try (java.io.InputStream s = in) {
            byte[] b = new byte[Ps1Discs.M3U_MAX];
            int n = 0, r;
            while (n < b.length && (r = s.read(b, n, b.length - n)) > 0)
                n += r;
            return new String(b, 0, n, java.nio.charset.StandardCharsets.UTF_8);
        }
    }

    /**
     * What the runtime opens for a disc chosen in the menu: a path as it is; a document as « fd:N » (detached), a
     * .cue document as a .cue of our own naming its tracks' descriptors (Ps1Cue, its tracks from the library index);
     * null when it cannot be opened.
     */
    private String ps1DiscSpec(Ps1Discs.Disc d) {
        if (!d.spec.startsWith("content:"))
            return d.spec;
        if (".cue".equals(Ps1Discs.ext(d.name))) {
            java.util.List<String> parts = PrepareService.cueParts(getFilesDir(), d.spec);
            if (parts.isEmpty())
                return null;
            return Ps1Cue.runtimeCue(d.spec, parts, x -> getContentResolver().openInputStream(Uri.parse(x)),
                                     this::documentName, DiscImages.contentOpener(this),
                                     new File(getCacheDir(), "ps1/disc" + d.number));
        }
        return romDescriptor(d.spec, DiscImages.contentOpener(this));
    }

    /** "fd:N" for a ROM document (its descriptor detached, the caller's to keep), or null when it cannot be opened. */
    static String romDescriptor(String uri, DiscImages.Opener opener) {
        try {
            int fd = opener.open(uri);
            return fd >= 0 ? "fd:" + fd : null;
        } catch (java.io.IOException | RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot open " + uri, e);
            return null;
        }
    }

    /**
     * The game theme's colours for the in-game menu and the bottom screen's panel (docs/android/app-shell.md « Game
     * theme »), before they are built: from the launcher's cache, read once at game start; tokens only, no bitmaps.
     * None cached, or the player's « Off »: the stock colours.
     */
    private void applyGameTheme() {
        String tid = GameLanguage.runningTitleId(this);
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
     * RECOMP_N64_PAK for the N64 runtime (XBA-174, {@link N64Pak}): the launched N64 game's own choice (« rumble » or
     * « controller »), else unset (recomp.cfg, then the controller pak, decide), also clearing what a previous game of
     * this process chose.
     */
    private void applyN64Pak(boolean romLaunch) {
        String tid = romLaunch ? GameStore.launchedTitleId(this) : null;
        String v = tid != null && Console.ofKey(tid) == Console.N64 ? N64Pak.choice(this, tid) : null;
        try {
            if (v != null)
                Os.setenv(N64Pak.ENV, v, true);
            else
                Os.unsetenv(N64Pak.ENV);
        } catch (ErrnoException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot set " + N64Pak.ENV, e);
        }
        if (tid != null && Console.ofKey(tid) == Console.N64)
            Log.i(LauncherActivity.TAG, "shell: n64 pak " + (v != null ? v : "default (recomp.cfg, else controller)")
                                        + ", game " + tid);
    }

    /**
     * The custom Vulkan driver for the N64 Vulkan RDP ({@link VkDrivers}): for an N64 game whose driver (its own choice
     * in its Options, else Settings' choice, else the system driver: VkDrivers.resolve) is a still installed one,
     * RECOMP_VK_DRIVER_DIR / _LIB / _ID and RECOMP_VK_HOOK_DIR; otherwise all four unset, also
     * clearing what a previous game of this process had. RECOMP_VK_DRIVER=system in recomp.cfg still wins natively.
     * A game's own app uses its own files/vk_drivers (the driver it took from XRecomp).
     */
    private void applyVkDriver(boolean romLaunch) {
        String tid = romLaunch ? GameStore.launchedTitleId(this) : null;
        boolean n64 = tid != null && Console.ofKey(tid) == Console.N64;
        if (n64 && gameApp())   // XRecomp's choice as it is now (VkDriverShare); unchanged: nothing written
            VkDriverShare.sync(this);
        VkDrivers drivers = VkDrivers.of(this);
        VkDrivers.Driver d = n64 ? drivers.resolve(tid) : null;
        String own = n64 ? drivers.gameChoice(tid) : null;
        for (String[] kv : VkDrivers.runEnv(d, getApplicationInfo().nativeLibraryDir)) {
            try {
                if (kv[1] != null)
                    Os.setenv(kv[0], kv[1], true);
                else
                    Os.unsetenv(kv[0]);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set " + kv[0], e);
            }
        }
        if (n64)
            Log.i(LauncherActivity.TAG, "shell: vulkan driver " + (d == null ? "system"
                                        : d.name + " (" + d.id + ")"
                                          + (drivers.failedLastStart(d.id) ? ", failed at the last start" : ""))
                                        + (own == null ? " (as in Settings)" : " (the game's own choice)")
                                        + ", game " + tid);
    }

    /**
     * The player's mapping for a GBA, PS1 or N64 game (XBA-158 / XBA-168 / XBA-174, {@link ConsoleControls}): the
     * launched game's own as RECOMP_&lt;CONSOLE&gt;_PAD_MAP, where it wins over recomp.cfg's line (the console's
     * mapping); the other consoles' variables unset, so nothing a previous game of this process set stays. Returns
     * the launched game's console (XBOX for an Xbox game).
     */
    private Console applyConsolePad(boolean romLaunch) {
        String tid = romLaunch ? GameStore.launchedTitleId(this) : null;
        Console c = tid != null ? Console.ofKey(tid) : Console.XBOX;
        for (String[] kv : ConsoleControls.runEnv(this, tid)) {
            try {
                if (kv[1] != null)
                    Os.setenv(kv[0], kv[1], true);
                else
                    Os.unsetenv(kv[0]);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set " + kv[0], e);
            }
        }
        if (c != Console.XBOX) {
            String own = ConsoleControls.gameText(this, tid);
            Log.i(LauncherActivity.TAG, "shell: " + c.id + " controls " + (own == null ? "of the console (recomp.cfg)"
                                        : "of the game: " + (own.isEmpty() ? "(default)" : own)) + ", game " + tid);
        }
        return c;
    }

    /**
     * RECOMP_SCALE and RECOMP_FILTER for a GBA, PS1 or N64 game (XBA-181, {@link ConsoleVideo}), RECOMP_UPSCALE too
     * (XBA-184; the game's own choice for a PS1 or N64 game) and RECOMP_LSFG (XBA-186, frame generation): the game's
     * own choice, else Settings' (recomp.cfg), else unset (the runtime's default); with the settings every console
     * shares (vertical sync, audio buffer, rumble, frame generation's multiplier, performance set and shader cache)
     * from recomp.cfg. An Xbox game: scale, filter, internal resolution and RECOMP_LSFG unset (recomp.cfg's applies).
     */
    private void applyConsoleVideo(Console c) {
        String tid = c != Console.XBOX ? GameStore.launchedTitleId(this) : null;
        File ext = GameStore.externalFilesDir(this);
        RecompConfig cfg = ext == null ? null : RecompConfig.load(ext);
        String scale = ConsoleVideo.gameChoice(this, tid, ConsoleVideo.Setting.SCALE);
        String filter = ConsoleVideo.gameChoice(this, tid, ConsoleVideo.Setting.FILTER);
        String upscale = ConsoleVideo.gameChoice(this, tid, ConsoleVideo.Setting.UPSCALE);
        String framegen = ConsoleVideo.gameChoice(this, tid, ConsoleVideo.Setting.FRAMEGEN);   // XBA-186
        java.util.List<String[]> env = ConsoleVideo.runEnv(c, scale, filter, upscale, framegen, cfg);
        StringBuilder line = new StringBuilder("shell: console video");
        for (String[] kv : env) {
            try {
                if (kv[1] != null)
                    Os.setenv(kv[0], kv[1], true);
                else
                    Os.unsetenv(kv[0]);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot set " + kv[0], e);
            }
            line.append(' ').append(kv[0]).append('=').append(kv[1] != null ? kv[1] : "(default)");
        }
        if (c != Console.XBOX)
            Log.i(LauncherActivity.TAG, line.append(", game ").append(tid).toString());
    }

    /** RECOMP_PS1_PAD=analog in recomp.cfg: the PS1 on-screen pad gets the DualShock's two sticks. */
    private boolean ps1Analog() {
        File ext = GameStore.externalFilesDir(this);
        String v = ext == null ? null : RecompConfig.load(ext).get("RECOMP_PS1_PAD");
        return v != null && v.trim().equalsIgnoreCase("analog");
    }

    /**
     * RECOMP_LANGUAGE for the runtime (XBA-77, {@link GameLanguage}): the title's own choice, else none when recomp.cfg
     * has a line (the runtime reads it there), else the Android language when the Xbox has it. Unset otherwise, also
     * clearing a value a previous game left in this process.
     */
    private void applyLanguage() {
        File ext = GameStore.externalFilesDir(this);
        String cfg = ext == null ? null : RecompConfig.load(ext).get(GameLanguage.ENV);
        String tid = GameLanguage.runningTitleId(this);
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
        String tid = GameLanguage.runningTitleId(this);
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
        public Boolean touchShown() {
            return touch == null ? null : touch.shown();
        }

        @Override
        public void setTouchShown(boolean on) {
            if (touch != null)
                touch.force(on);
        }

        @Override
        public ConsolePadMap consoleMap() {
            String tid = GameStore.launchedRom(ShellActivity.this) != null
                         ? GameStore.launchedTitleId(ShellActivity.this) : null;
            if (tid == null || Console.ofKey(tid) == Console.XBOX)
                return null;
            return ConsoleControls.effective(ShellActivity.this, GameStore.externalFilesDir(ShellActivity.this), tid);
        }

        @Override
        public java.util.List<Ps1Discs.Disc> discs() {
            return ps1Discs();
        }

        @Override
        public boolean changeDisc(Ps1Discs.Disc d) {
            String spec = ps1DiscSpec(d);
            if (spec == null)
                return false;
            try {
                return MenuNative.nativePs1ChangeDisc(spec);
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native disc change", e);
                return false;
            }
        }

        /** XBA-182: a GBA or PS1 game runs, and its runtime can save states. */
        @Override
        public boolean statesSupported() {
            String tid = GameStore.launchedRom(ShellActivity.this) != null
                         ? GameStore.launchedTitleId(ShellActivity.this) : null;
            Console c = tid == null ? null : Console.ofKey(tid);
            if (c != Console.GBA && c != Console.PS1)
                return false;
            try {
                return MenuNative.nativeStatesAvailable();
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native save states", e);
                return false;
            }
        }

        @Override
        public long stateTime(int slot) {
            try {
                return MenuNative.nativeStateTime(slot);
            } catch (UnsatisfiedLinkError e) {
                return 0;
            }
        }

        @Override
        public int[] stateThumb(int slot) {
            try {
                return MenuNative.nativeStateThumb(slot);
            } catch (UnsatisfiedLinkError e) {
                return null;
            }
        }

        @Override
        public int saveState(int slot) {
            try {
                return MenuNative.nativeStateSave(slot);
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native state save", e);
                return GameMenu.STATE_NO_GAME;
            }
        }

        @Override
        public int loadState(int slot) {
            try {
                return MenuNative.nativeStateLoad(slot);
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native state load", e);
                return GameMenu.STATE_NO_GAME;
            }
        }

        @Override
        public boolean fastForward() {
            try {
                return MenuNative.nativeFastForwardOn();
            } catch (UnsatisfiedLinkError e) {
                return false;
            }
        }

        @Override
        public void setFastForward(boolean on) {
            try {
                MenuNative.nativeFastForward(on);
            } catch (UnsatisfiedLinkError e) {
                Log.w(LauncherActivity.TAG, "shell: menu: no native fast-forward", e);
            }
        }

        @Override
        public void quit() {
            overlayOff();
            if (gameApp())   // a game's own app closes: Android's home screen, not its launcher
                finishAndRemoveTask();
            try {
                MenuNative.nativeQuit();
            } catch (UnsatisfiedLinkError e) {
                finish();
            }
        }
    }

    /**
     * The launcher on top; this task (the game) stays, and the launcher's Play brings it back. A game's own app has
     * no launcher to show (docs « Straight into the game »): its task goes behind Android's home screen instead, the
     * game held as in the background, and its icon brings it back.
     */
    private void openLauncher() {
        if (gameApp()) {
            Log.i(LauncherActivity.TAG, "shell: game app: to the home screen (the game waits)");
            moveTaskToBack(true);
            return;
        }
        startActivity(new Intent(this, LauncherActivity.class)
                .setAction(LauncherActivity.ACTION_SHOW_LAUNCHER).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    /** This is a game's own app (Create an app: its one-game meta-data). */
    private boolean gameApp() {
        return TitleApp.ownTitleId(this) != null;
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
                if (gameApp())
                    finishAndRemoveTask();
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
        openLauncherAndFinish(titleExitedIntent(this, w));
    }

    /**
     * Called by shell_main.c (the guest thread, through JNI) when the title launched another image of its disc that
     * the pack names but has not prepared yet (XBA-82, lazy; the native side wrote work/TID/image.pending and the
     * kernel the launch data). The launcher opens offering « Prepare IMAGE » (LauncherActivity.EXTRA_PREPARE_IMAGE)
     * and this activity finishes; as titleExited, the native side then ends this process.
     */
    @SuppressWarnings("unused")
    public void prepareImage(String image) {
        String img = image == null ? "" : image;
        Log.i(LauncherActivity.TAG, "shell: the game launches " + img + ", not prepared yet, back to the launcher");
        openLauncherAndFinish(prepareImageIntent(this, img));
    }

    /** The launcher, on Home offering to prepare image (a disc path, as pack.json names it). */
    static Intent prepareImageIntent(Context c, String image) {
        return new Intent(c, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_PREPARE_IMAGE, image == null ? "" : image)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
    }

    /** Starts the launcher with i and finishes; returns once asked (at most TITLE_EXIT_HANDOFF_MS). */
    private void openLauncherAndFinish(Intent i) {
        CountDownLatch asked = new CountDownLatch(1);
        Runnable open = () -> {
            try {
                startActivity(i);
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
