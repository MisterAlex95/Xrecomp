package fr.adserver.xrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * The N64 shader warm-up (docs/android/app-shell.md « Shader warm-up »): the Vulkan RDP's pipelines built ahead of
 * time for the (driver, internal resolution) an N64 game will start with, so its first start does not spend seconds
 * on a black screen compiling them. ShaderWarmupService runs n64_rdp_vk_warmup in a process of its own (":warmup",
 * background priority) which this class starts, and ends:
 * <ul>
 *   <li>when: at the end of an N64 Prepare ({@link Reason#PREPARE}); when the player changes the Vulkan driver or the
 *       internal resolution, of a game or in Settings ({@link Reason#SETTING}); after an app update, for the last N64
 *       game played ({@link Reason#UPDATE});</li>
 *   <li>never next to a game: at 8x it takes the game's 2 GB of buffers (nor next to a Prepare's compilers). A
 *       request while a game runs is kept (warmup.pending) and made when the launcher shows again without one; ShellActivity ends a warm-up before
 *       its game starts ({@link #cancel}; the cache files are written whole or not at all);</li>
 *   <li>once per (driver, factor, app install): the service writes vk_cache/&lt;driver&gt;/warmup-&lt;f&gt;x, and a
 *       request finding it skips; a driver whose last start failed is skipped (the game's start reports it).</li>
 * </ul>
 * The decisions are pure ({@link #decide}, {@link #factor}, {@link #isWarm}) for the tests.
 */
final class ShaderWarmup {
    private ShaderWarmup() {}

    static final String PROCESS_SUFFIX = ":warmup";
    static final String PENDING = "warmup.pending", VERSION = "warmup.version";
    static final String EXTRA_TITLE_ID = "tid";

    enum Reason { PREPARE, SETTING, UPDATE }

    enum Decision { RUN, PENDING, SKIP_WARM, SKIP_DRIVER_FAILED, SKIP_NOT_N64 }

    /** What to warm: a game's (or Settings', tid null) driver (null: the system's) and internal resolution. */
    static final class Target {
        final String tid;
        final VkDrivers.Driver driver;
        final int factor;

        Target(String tid, VkDrivers.Driver driver, int factor) {
            this.tid = tid;
            this.driver = driver;
            this.factor = factor;
        }

        String driverLabel() {
            return driver == null ? VkDrivers.SYSTEM : driver.id;
        }

        @Override
        public String toString() {
            return (tid == null ? "(Settings)" : tid) + " " + driverLabel() + " " + factor + "x";
        }
    }

    // ---- Pure ------------------------------------------------------------------------------------------------------

    /** RECOMP_UPSCALE's value as the runtime reads it (n64_rdp_upscale_parse): 1, 2, 4 or 8; unset or bad: 1. */
    static int factor(String v) {
        if (v == null)
            return 1;
        try {
            int f = Integer.parseInt(v.trim());
            return f >= 8 ? 8 : f >= 4 ? 4 : f >= 2 ? 2 : 1;
        } catch (NumberFormatException e) {
            return 1;
        }
    }

    /** The record's line: « &lt;app install stamp&gt; &lt;rc&gt; &lt;start-up pipelines&gt; &lt;compiled&gt; ». */
    static String recordLine(long version, int[] res) {
        return version + " " + res[0] + " " + res[2] + " " + res[4] + "\n";
    }

    /**
     * The record says this (driver, factor) was warmed by this app version: done (rc 0), or the Vulkan RDP cannot run
     * on that driver (rc 1: the game's start falls back, warming again would not help).
     */
    static boolean isWarm(String record, long version) {
        if (record == null)
            return false;
        String[] w = record.trim().split("\\s+");
        if (w.length < 2)
            return false;
        try {
            return Long.parseLong(w[0]) == version && (w[1].equals("0") || w[1].equals("1"));
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /** The decision for a request (driverFailed: its last start failed or crashed; warm: isWarm). */
    static Decision decide(Reason r, boolean n64, boolean gameRunning, boolean driverFailed, boolean warm) {
        if (!n64)
            return Decision.SKIP_NOT_N64;
        if (driverFailed)
            return Decision.SKIP_DRIVER_FAILED;
        if (warm)
            return Decision.SKIP_WARM;
        if (gameRunning)
            return Decision.PENDING;
        return Decision.RUN;
    }

    /** After an app update (stored version differs; none stored = first run with this feature: also an update). */
    static boolean updated(String storedVersion, long version) {
        if (storedVersion == null)
            return true;
        try {
            return Long.parseLong(storedVersion.trim()) != version;
        } catch (NumberFormatException e) {
            return true;
        }
    }

    // ---- Paths -----------------------------------------------------------------------------------------------------

    static File root(File cacheDir) {
        return new File(cacheDir, VkDrivers.CACHE_DIR);
    }

    /** The driver's cache directory (VkDrivers.cacheEnv: what the game gets as RECOMP_VK_CACHE_DIR). */
    static File driverDir(File cacheDir, Target t) {
        return new File(VkDrivers.cacheEnv(t.driver, cacheDir)[1]);
    }

    static File recordFile(File cacheDir, Target t) {
        return new File(driverDir(cacheDir, t), "warmup-" + t.factor + "x");
    }

    // ---- Android ---------------------------------------------------------------------------------------------------

    /** The game's (or Settings') driver and internal resolution, as ShellActivity would give them to the runtime. */
    static Target target(Context c, String tid) {
        VkDrivers.Driver d = VkDrivers.of(c).resolve(tid);
        File ext = GameStore.externalFilesDir(c);
        RecompConfig cfg = ext == null ? null : RecompConfig.load(ext);
        String game = tid == null ? null : ConsoleVideo.gameChoice(c, tid, ConsoleVideo.Setting.UPSCALE);
        String v = ConsoleVideo.resolve(game, cfg == null ? null : cfg.get(ConsoleVideo.ENV_UPSCALE));
        return new Target(tid, d, factor(v));
    }

    /**
     * This install of the app: when the APK was last installed or updated (PackageInfo.lastUpdateTime; the
     * versionCode stays 1 for local builds). A new APK may carry new shaders: everything is warmed again.
     */
    static long versionCode(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            return 0;
        }
    }

    static String read(File f) {
        try {
            return f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8) : null;
        } catch (IOException e) {
            return null;
        }
    }

    static void write(File f, String text) {
        try {
            //noinspection ResultOfMethodCallIgnored
            f.getParentFile().mkdirs();
            File tmp = new File(f.getPath() + ".tmp");
            Files.write(tmp.toPath(), text.getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(f))
                Log.w(LauncherActivity.TAG, "warmup: cannot write " + f);
        } catch (IOException e) {
            Log.w(LauncherActivity.TAG, "warmup: cannot write " + f, e);
        }
    }

    static boolean driverFailed(Context c, Target t) {
        if (t.driver == null)
            return false;
        VkDrivers drivers = VkDrivers.of(c);
        String st = drivers.lastStartStatus(t.driver.id);
        return drivers.failedLastStart(t.driver.id) || (st != null && !st.equals("ok"));
    }

    /**
     * A request: decided, then the warm-up started (a running one for another target ended first), kept for later
     * (a game runs), or skipped; logged. Any process; returns the decision.
     */
    static Decision request(Context c, String tid, Reason r) {
        boolean n64 = tid == null || Console.ofKey(tid) == Console.N64;
        Target t = n64 ? target(c, tid) : null;
        boolean busy = gameRunning(c) || (r != Reason.PREPARE && processPid(c, ":prepare") != 0);
        Decision d = decide(r, n64, busy, n64 && driverFailed(c, t),
                            n64 && isWarm(read(recordFile(c.getCacheDir(), t)), versionCode(c)));
        Log.i(LauncherActivity.TAG, "warmup: " + r.name().toLowerCase(java.util.Locale.ROOT) + " " + (t != null ? t
                                    : tid) + ": " + d.name().toLowerCase(java.util.Locale.ROOT));
        switch (d) {
        case RUN:
            cancel(c);   // latest wins: a fresh process (a custom driver, once loaded, stays in its process)
            try {
                c.startService(new Intent(c, ShaderWarmupService.class).putExtra(EXTRA_TITLE_ID, tid));
            } catch (IllegalStateException | SecurityException e) {   // background start refused: later
                Log.w(LauncherActivity.TAG, "warmup: cannot start now, kept for later (" + e.getMessage() + ")");
                keepPending(c, tid);
            }
            break;
        case PENDING:
            keepPending(c, tid);
            break;
        default:
            break;
        }
        return d;
    }

    static void keepPending(Context c, String tid) {
        write(new File(root(c.getCacheDir()), PENDING), tid == null ? "\n" : tid + "\n");
    }

    /** The launcher shows and no game runs: a request kept while one ran is made now. */
    static void runPending(Context c) {
        File f = new File(root(c.getCacheDir()), PENDING);
        String p = read(f);
        if (p == null || gameRunning(c))
            return;
        //noinspection ResultOfMethodCallIgnored
        f.delete();
        String tid = p.trim();
        request(c, tid.isEmpty() ? null : tid, Reason.SETTING);
    }

    /** At the launcher's start: after an app update, the last N64 game played warmed again (its pipelines changed). */
    static void afterUpdate(Context c) {
        File f = new File(root(c.getCacheDir()), VERSION);
        long v = versionCode(c);
        if (!updated(read(f), v))
            return;
        write(f, v + "\n");
        String tid = GameStore.launchedTitleId(c);
        if (tid != null && Console.ofKey(tid) == Console.N64)
            request(c, tid, Reason.UPDATE);
    }

    /** The ":warmup" process, 0 when there is none. */
    static int warmupPid(Context c) {
        return processPid(c, PROCESS_SUFFIX);
    }

    /** This app's process of that suffix (":prepare"...), 0 when there is none. */
    static int processPid(Context c, String suffix) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return 0;
        String name = c.getPackageName() + suffix;
        for (ActivityManager.RunningAppProcessInfo p : procs)
            if (name.equals(p.processName))
                return p.pid;
        return 0;
    }

    /** Ends a running warm-up (a game is starting, or another warm-up replaces it); waits up to 1 s for it to go. */
    static void cancel(Context c) {
        int pid = warmupPid(c);
        if (pid == 0 || pid == Process.myPid())
            return;
        Log.i(LauncherActivity.TAG, "warmup: ending the running warm-up (pid " + pid + ")");
        Process.killProcess(pid);
        long until = SystemClock.uptimeMillis() + 1000;
        while (warmupPid(c) != 0 && SystemClock.uptimeMillis() < until)
            SystemClock.sleep(20);
    }

    /** A game runs: the main process is there with a ShellActivity task (LauncherActivity.gameRunning's test). */
    static boolean gameRunning(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null)
            return false;
        boolean main = false;
        List<ActivityManager.RunningAppProcessInfo> procs = am.getRunningAppProcesses();
        if (procs != null)
            for (ActivityManager.RunningAppProcessInfo p : procs)
                main |= p.processName.equals(c.getPackageName());
        if (!main)
            return false;
        for (ActivityManager.AppTask t : am.getAppTasks()) {
            try {
                ActivityManager.RecentTaskInfo info = t.getTaskInfo();
                if (info.numActivities > 0 && info.baseActivity != null
                    && info.baseActivity.getClassName().equals(ShellActivity.class.getName()))
                    return true;
            } catch (IllegalArgumentException ignored) {   // the task went away meanwhile
            }
        }
        return false;
    }
}
