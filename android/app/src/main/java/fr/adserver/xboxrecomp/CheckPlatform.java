package fr.adserver.xboxrecomp;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.Intent;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * The library check on a real device (CheckDriver.Platform): PrepareService for the Prepares, ShellActivity in a fresh
 * main process for the boots (the launcher's own steps: the launched game, the shared caches, the self-heal stamp),
 * ActivityManager for the game's process and how it ended, logcat for its log, and the embedded CPython for the grade
 * ({@code python -m tools.compat grade}: tools.compat's rules and schema, one implementation for desktop and device).
 */
class CheckPlatform implements CheckDriver.Platform {
    static final String TAG = "xboxrecomp";
    /** A grade takes a second or two; past this the Python is stuck. */
    static final long GRADE_TIMEOUT_S = 120;

    final Context c;
    /** Told when the state changes (CheckService: the notification). */
    final Runnable onChange;

    CheckPlatform(Context c, Runnable onChange) {
        this.c = c.getApplicationContext() != null ? c.getApplicationContext() : c;
        this.onChange = onChange;
    }

    @Override
    public long now() {
        return System.currentTimeMillis();
    }

    @Override
    public File filesDir() {
        return c.getFilesDir();
    }

    @Override
    public boolean needsPrepare(LibraryCheck.Title t) {
        File f = filesDir();
        return LibraryCheck.needsPrepare(f, t.tid, t.xbeSha256, LibraryCheck.installedToolkit(f));
    }

    /** As the launcher's Prepare: a fresh status first (no stale outcome is read), then PrepareService. */
    @Override
    public void startPrepare(LibraryCheck.Title t) throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = t.tid;
        s.startedAt = s.updatedAt = now();
        s.msg = c.getString(R.string.PREPARE_STARTING);
        s.log = PrepareService.logFile(c, t.tid).getAbsolutePath();
        PrepareProtocol.writeStatus(PrepareService.statusFile(c, t.tid), s);
        c.startForegroundService(PrepareService.startIntent(c, t.source, t.tid, t.title));
        Log.i(TAG, "check: prepare started for " + t.tid + " (" + t.source + ")");
    }

    @Override
    public void cancelPrepare(String tid) {
        try {
            c.startService(PrepareService.cancelIntent(c, tid));
        } catch (RuntimeException e) {
            Log.w(TAG, "check: cannot cancel " + tid + "'s Prepare: " + e);
        }
    }

    @Override
    public PrepareProtocol.Status prepareStatus(String tid) {
        return PrepareProtocol.readStatus(PrepareService.statusFile(c, tid));
    }

    private ActivityManager.RunningAppProcessInfo process(String name) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
        if (procs == null)
            return null;
        for (ActivityManager.RunningAppProcessInfo p : procs) {
            if (name.equals(p.processName))
                return p;
        }
        return null;
    }

    @Override
    public boolean prepareAlive() {
        return process(c.getPackageName() + ":prepare") != null;
    }

    @Override
    public boolean canStart() {
        return CheckActivity.isShown();
    }

    /** launchShell's steps for the check's title, then ShellActivity in a new main process. */
    @Override
    public void launchGame(LibraryCheck.Title t) throws Exception {
        long now = now();
        GameStore.rememberLaunch(c, t.gameDir, t.title, t.tid);
        try {
            if (SaveCaches.switchTo(GameTheme.saveDir(c), Library.cacheOwnerFile(filesDir()), t.tid))
                Log.i(TAG, "check: game caches switched to " + t.tid);
        } catch (IOException e) {
            Log.w(TAG, "check: cannot switch the game caches to " + t.tid, e);
        }
        try {   // the exit that follows is this run's (XBA-74), not an older one
            SelfHeal.launched(LibraryCheck.workDir(filesDir(), t.tid), now);
        } catch (IOException e) {
            Log.w(TAG, "check: cannot update " + t.tid + "'s selfheal.json", e);
        }
        c.startActivity(new Intent(c, ShellActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
    }

    @Override
    public int gamePid() {
        ActivityManager.RunningAppProcessInfo p = process(c.getPackageName());
        return p == null ? 0 : p.pid;
    }

    @Override
    public boolean gameInFront() {
        ActivityManager.RunningAppProcessInfo p = process(c.getPackageName());
        return p != null && p.importance <= ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
    }

    @Override
    public boolean gameWindow() {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null)
            return true;
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

    @Override
    public void bringCheckToFront() {
        try {
            c.startActivity(new Intent(c, CheckActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
        } catch (RuntimeException e) {
            Log.w(TAG, "check: cannot bring the check to the front: " + e);
        }
    }

    @Override
    public void endGame() {
        int pid = gamePid();
        if (pid == 0)
            return;
        Log.i(TAG, "check: ending the game (pid " + pid + ")");
        Process.killProcess(pid);   // by PID, as the desktop runner
        long until = SystemClock.uptimeMillis() + 3000;
        while (gamePid() == pid && SystemClock.uptimeMillis() < until)
            SystemClock.sleep(50);
    }

    @Override
    public CheckDriver.Exit exitSince(long since) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null)
            return null;
        List<ApplicationExitInfo> exits;
        try {
            exits = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 16);
        } catch (RuntimeException e) {
            return null;
        }
        for (ApplicationExitInfo x : exits) {   // newest first
            if (!c.getPackageName().equals(x.getProcessName()) || x.getTimestamp() < since)
                continue;
            return new CheckDriver.Exit(exitArg(x.getReason(), x.getStatus()), x.getTimestamp());
        }
        return null;
    }

    /** ApplicationExitInfo's reason and status in tools.compat grade's --exit words. */
    static String exitArg(int reason, int status) {
        switch (reason) {
        case ApplicationExitInfo.REASON_EXIT_SELF:
            return "exit:" + status;
        case ApplicationExitInfo.REASON_SIGNALED:
            return "signal:" + status;
        case ApplicationExitInfo.REASON_CRASH_NATIVE:
            return status > 0 ? "crash:" + status : "crash";
        case ApplicationExitInfo.REASON_CRASH:
            return "java";
        case ApplicationExitInfo.REASON_ANR:
            return "anr";
        case ApplicationExitInfo.REASON_LOW_MEMORY:
            return "lowmem";
        default:
            return "exit:" + status;
        }
    }

    @Override
    public long peakKb(int pid) {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/" + pid + "/status"))) {
            String l;
            while ((l = r.readLine()) != null) {
                if (l.startsWith("VmHWM:"))
                    return Long.parseLong(l.replaceAll("[^0-9]", ""));
            }
        } catch (IOException | NumberFormatException ignored) {
        }
        return 0;
    }

    @Override
    public void dumpLog(int pid, File out) {
        try {
            java.lang.Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "20000",
                                                     "--pid=" + pid)
                    .redirectErrorStream(true).redirectOutput(out).start();
            if (!p.waitFor(15, TimeUnit.SECONDS))
                p.destroy();
        } catch (IOException e) {
            Log.w(TAG, "check: no log for pid " + pid + ": " + e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    // ---- Grading ----------------------------------------------------------------------------------------------------

    /** The device's fields in every line (« Nothing from the game »: the device, the app, the run's own numbers). */
    List<String> deviceFields(CheckDriver.Grade g) {
        List<String> f = new ArrayList<>();
        String model = Build.MODEL == null ? "" : Build.MODEL;
        String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER;
        String device = model.toLowerCase(Locale.ROOT).startsWith(maker.toLowerCase(Locale.ROOT)) ? model
                        : (maker + " " + model).trim();
        f.add("device=" + device);
        f.add("android=" + JSONObject.quote(String.valueOf(Build.VERSION.RELEASE)));
        f.add("api=" + Build.VERSION.SDK_INT);
        if (Build.VERSION.SDK_INT >= 31 && Build.SOC_MODEL != null && !Build.SOC_MODEL.isEmpty())
            f.add("soc=" + Build.SOC_MODEL);
        f.add("app_build=" + c.getString(R.string.xba_version) + " (" + c.getString(R.string.xba_version_code) + ")");
        f.add("seconds=" + (int)Math.round(g.runS > 0 ? g.runS : 0));
        if (g.peakKb > 0)
            f.add("peak_mem_mb=" + (g.peakKb + 512) / 1024);
        return f;
    }

    /** tools.compat grade's arguments for g (after the interpreter and « -m tools.compat grade »). */
    static List<String> gradeArgs(CheckDriver.Grade g, File results, List<String> fields) {
        List<String> a = new ArrayList<>();
        a.add("--out");
        a.add(results.getAbsolutePath());
        a.add("--source");
        a.add(g.title.title.isEmpty() ? g.title.tid : g.title.title);
        a.add("--title-id");
        a.add(g.title.tid);
        a.add("--work");
        a.add(g.work.getAbsolutePath());
        if (g.prepareMs >= 0) {
            a.add("--prepare-s");
            a.add(String.valueOf(g.prepareMs / 1000));
        }
        if (g.prepareFailed != null) {
            a.add("--prepare-failed");
            a.add(g.prepareFailed);
        } else if (g.notBooted != null) {
            a.add("--not-booted");
            a.add(g.notBooted);
        } else {
            a.add("--status");
            a.add(g.status.getAbsolutePath());
            a.add("--log");
            a.add(g.log.getAbsolutePath());
            a.add("--presses");
            a.add(g.presses);
            a.add("--exit");
            a.add(g.exit);
            a.add("--run-s");
            a.add(String.format(Locale.ROOT, "%.1f", g.runS));
            a.add("--sustain");
            a.add(String.valueOf(g.sustain));
        }
        for (String f : fields) {
            a.add("--field");
            a.add(f);
        }
        return a;
    }

    @Override
    public JSONObject grade(CheckDriver.Grade g) throws Exception {
        File results = LibraryCheck.resultsFile(filesDir());
        List<String> fields = deviceFields(g);
        try {
            return python(g, results, fields);
        } catch (IOException | JSONException | RuntimeException e) {
            Log.w(TAG, "check: tools.compat grade failed, the app writes the line itself: " + e);
            return fallback(g, results, fields, e);
        }
    }

    /** The real grade: the embedded CPython, the installed toolkit (installed already: a Prepare ran). */
    private JSONObject python(CheckDriver.Grade g, File results, List<String> fields)
            throws IOException, JSONException {
        File files = filesDir();
        File nat = Toolchain.nativeDir(c);
        Toolchain.Installed in = Toolchain.install(c, files, new Toolchain.Progress() {
            @Override
            public void onProgress(long done, long total, String what) {
            }

            @Override
            public boolean cancelled() {
                return false;
            }
        });
        List<String> cmd = new ArrayList<>();
        cmd.add(new File(nat, Toolchain.PYTHON).getAbsolutePath());
        cmd.add("-m");
        cmd.add("tools.compat");
        cmd.add("grade");
        cmd.addAll(gradeArgs(g, results, fields));
        ProcessBuilder pb = new ProcessBuilder(cmd).directory(in.toolkit).redirectErrorStream(false);
        Map<String, String> e = pb.environment();
        File cache = c.getCacheDir();
        e.put("PYTHONHOME", in.python.getAbsolutePath());
        e.put("PYTHONPATH", in.toolkit.getAbsolutePath());
        e.put("PYTHONPYCACHEPREFIX", new File(cache, "pycache").getAbsolutePath());
        e.put("TMPDIR", new File(cache, "tmp").getAbsolutePath());
        e.put("HOME", files.getAbsolutePath());
        e.put("LD_LIBRARY_PATH", nat.getAbsolutePath());
        e.put("PYTHONUNBUFFERED", "1");
        File err = new File(LibraryCheck.dir(files), "grade.err");
        pb.redirectError(err);
        java.lang.Process p = pb.start();
        String last = "";
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(),
                                                                         StandardCharsets.UTF_8))) {
            String l;
            while ((l = r.readLine()) != null) {
                if (l.startsWith("{"))
                    last = l;
            }
        }
        try {
            if (!p.waitFor(GRADE_TIMEOUT_S, TimeUnit.SECONDS)) {
                p.destroyForcibly();
                throw new IOException("tools.compat grade timed out");
            }
        } catch (InterruptedException ie) {
            Thread.currentThread().interrupt();
            throw new IOException("interrupted");
        }
        if (p.exitValue() != 0 || last.isEmpty())
            throw new IOException("tools.compat grade exited " + p.exitValue() + " (" + err + ")");
        return new JSONObject(last);
    }

    /**
     * When the Python cannot run (the tools could not be installed, for one): the line's fixed part written here, so
     * the title is in the report anyway, marked {@code "graded_by": "app"}, with what the app knows without grading.
     */
    static JSONObject fallback(CheckDriver.Grade g, File results, List<String> fields, Exception why)
            throws IOException, JSONException {
        JSONObject o = new JSONObject();
        o.put("source", g.title.title.isEmpty() ? g.title.tid : g.title.title);
        o.put("title_id", g.title.tid);
        o.put("title", g.title.title);
        o.put("toolkit", "unknown");
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        f.setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
        o.put("time", f.format(new java.util.Date()));
        o.put("host", "android");
        o.put("graded_by", "app");
        if (g.prepareFailed != null) {
            o.put("stage", "none");
            o.put("blocker", "prepare failed: " + g.prepareFailed);
        } else {
            o.put("stage", "prepares");
            o.put("blocker", (g.notBooted != null ? "not booted: " + g.notBooted : "not graded")
                             + " (tools.compat grade: " + why.getMessage() + ")");
        }
        if (g.prepareMs >= 0)
            o.put("prepare_s", g.prepareMs / 1000);
        for (String kv : fields) {
            int eq = kv.indexOf('=');
            String v = kv.substring(eq + 1);
            Object val = v;
            try {
                val = Long.parseLong(v);
            } catch (NumberFormatException ignored) {
                if (v.startsWith("\""))
                    val = new org.json.JSONTokener(v).nextValue();
            }
            o.put(kv.substring(0, eq), val);
        }
        File d = results.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            throw new IOException("cannot create " + d);
        try (FileOutputStream out = new FileOutputStream(results, true)) {
            out.write((o.toString() + "\n").getBytes(StandardCharsets.UTF_8));
        }
        return o;
    }

    // ---- The device ---------------------------------------------------------------------------------------------

    @Override
    public int thermalStatus() {
        PowerManager pm = c.getSystemService(PowerManager.class);
        return pm == null ? 0 : pm.getCurrentThermalStatus();
    }

    @Override
    public int batteryPercent() {
        BatteryManager bm = c.getSystemService(BatteryManager.class);
        return bm == null ? -1 : bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
    }

    @Override
    public boolean charging() {
        BatteryManager bm = c.getSystemService(BatteryManager.class);
        return bm == null || bm.isCharging();
    }

    @Override
    public void changed(LibraryCheck.State s) {
        if (onChange != null)
            onChange.run();
    }
}
