package fr.adserver.xboxrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The on-device Prepare (docs/android/prepare.md): turns the player's game into a title pack, in the background,
 * in its own process (":prepare"), as a foreground service of type specialUse (dataSync is capped at 6 hours a day on
 * Android 15; a Prepare can take longer on a slow device).
 *
 * ACTION_START (game dir, title ID): installs the tools from the APK's assets if needed (Toolchain.install, the
 * « install » phase), then runs `libxbapython.so -m tools.prepare <game> --android ...` through PrepareRunner, with
 * a partial wake lock. Progress goes to the notification (bar, step, time left, Cancel) and to
 * <files>/prepare/<TID>/status.json, which the launcher polls; the whole output to prepare.log next to it.
 * ACTION_CANCEL stops it (process group SIGTERM, then SIGKILL). One Prepare at a time; a second start while one runs
 * is ignored. Started again after an interruption, it runs the same command: tools.prepare's stamps skip what was
 * done.
 */
public class PrepareService extends Service {
    static final String TAG = "xboxrecomp";
    static final String ACTION_START = "fr.adserver.xboxrecomp.PREPARE_START";
    static final String ACTION_CANCEL = "fr.adserver.xboxrecomp.PREPARE_CANCEL";
    static final String EXTRA_GAME_DIR = "game_dir", EXTRA_TITLE_ID = "title_id", EXTRA_TITLE = "title";
    static final String CHANNEL = "prepare";
    static final int NOTIF_ID = 41, NOTIF_DONE_ID = 42;
    /** A Prepare holds the CPU at most this long (the wake lock's timeout). */
    static final long WAKE_LOCK_MS = 6L * 3600 * 1000;
    /** clang -O level: 2, tools.prepare's default (docs/android/prepare.md « Measurements »: 6 % slower than -O1). */
    static final int OPT = 2;

    /** Builds the command for a run; tests substitute a script. */
    interface Environment {
        /** Installs what the command needs (on the worker thread) and returns its factory. */
        PrepareRunner.CommandFactory prepare(Context c, String gameDir, String titleId, PrepareRunner runner)
                throws IOException;
    }

    static volatile Environment environmentOverride;

    /** The running job of this process, or null. */
    private static volatile PrepareRunner current;
    private static volatile Thread worker;

    private PowerManager.WakeLock wakeLock;
    private NotificationManager nm;
    private String titleName = "";
    private long lastNotify;
    private int lastNotifyPct = -1;
    private String lastNotifyId = "";

    static File filesDir(Context c) {
        return c.getFilesDir();
    }

    static File statusDir(Context c, String titleId) {
        return new File(new File(filesDir(c), "prepare"), titleId);
    }

    static File statusFile(Context c, String titleId) {
        return new File(statusDir(c, titleId), "status.json");
    }

    static File logFile(Context c, String titleId) {
        return new File(statusDir(c, titleId), "prepare.log");
    }

    /** True while a Prepare runs in this process (tests; the launcher, in another process, reads status.json). */
    static boolean busy() {
        Thread w = worker;
        return w != null && w.isAlive();
    }

    static Thread workerThread() {
        return worker;
    }

    static Intent startIntent(Context c, String gameDir, String titleId, String title) {
        return new Intent(c, PrepareService.class).setAction(ACTION_START).putExtra(EXTRA_GAME_DIR, gameDir)
                .putExtra(EXTRA_TITLE_ID, titleId).putExtra(EXTRA_TITLE, title);
    }

    static Intent cancelIntent(Context c, String titleId) {
        return new Intent(c, PrepareService.class).setAction(ACTION_CANCEL).putExtra(EXTRA_TITLE_ID, titleId);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.PREPARE_CHANNEL),
                                                             NotificationManager.IMPORTANCE_LOW);
            ch.setDescription(getString(R.string.PREPARE_CHANNEL_DESC));
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_CANCEL.equals(action)) {
            onCancel(intent.getStringExtra(EXTRA_TITLE_ID));
            return START_NOT_STICKY;
        }
        if (!ACTION_START.equals(action)) {
            if (!busy())
                stopSelf(startId);
            return START_NOT_STICKY;
        }
        String gameDir = intent.getStringExtra(EXTRA_GAME_DIR);
        String titleId = intent.getStringExtra(EXTRA_TITLE_ID);
        String title = intent.getStringExtra(EXTRA_TITLE);
        // startForegroundService() needs startForeground() even when nothing new starts.
        goForeground(notification(null));
        if (busy()) {
            Log.i(TAG, "prepare: already running, start ignored");
            return START_NOT_STICKY;
        }
        if (gameDir == null || titleId == null || titleId.isEmpty()) {
            Log.w(TAG, "prepare: start without a game or title ID");
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        titleName = title == null ? "" : title;
        startJob(gameDir, titleId);
        return START_NOT_STICKY;
    }

    private void goForeground(Notification n) {
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIF_ID, n);
    }

    private void onCancel(String titleId) {
        PrepareRunner r = current;
        if (r != null && busy()) {
            Log.i(TAG, "prepare: cancel requested");
            r.cancel();
            return;
        }
        // Nothing runs here (the process was restarted): a status left « running » by a dead process is cancelled.
        if (titleId != null) {
            File f = statusFile(this, titleId);
            PrepareProtocol.Status s = PrepareProtocol.readStatus(f);
            if (s != null && s.running()) {
                s.state = PrepareProtocol.CANCELLED;
                s.code = PrepareRunner.CANCELLED_CODE;
                s.updatedAt = System.currentTimeMillis();
                try {
                    PrepareProtocol.writeStatus(f, s);
                } catch (IOException e) {
                    Log.w(TAG, "prepare: cannot write " + f + ": " + e);
                }
            }
        }
        stopSelf();
    }

    private void startJob(String gameDir, String titleId) {
        int jobs = Toolchain.jobs(this);
        File filesDir = filesDir(this);
        PrepareRunner.CommandFactory[] factory = new PrepareRunner.CommandFactory[1];
        // The real factory exists once the tools are installed (in the worker); this one forwards to it.
        PrepareRunner runner = new PrepareRunner(statusDir(this, titleId), titleId, jobs,
                new PrepareRunner.CommandFactory() {
                    @Override
                    public List<String> command(int j) {
                        return factory[0].command(j);
                    }

                    @Override
                    public Map<String, String> environment() {
                        return factory[0].environment();
                    }

                    @Override
                    public File workingDir() {
                        return factory[0].workingDir();
                    }
                },
                () -> Pack.check(filesDir, titleId) == Pack.Check.VALID ? null : PrepareRunner.PACK_INVALID,
                this::onStatus);
        current = runner;
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "xboxrecomp:prepare");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire(WAKE_LOCK_MS);
        }
        Thread t = new Thread(() -> runJob(runner, factory, gameDir, titleId), "xba-prepare");
        worker = t;
        t.start();
        Log.i(TAG, "prepare: started for " + titleId + " (" + gameDir + "), " + jobs + " jobs");
    }

    /** The worker: install, then the runner. */
    private void runJob(PrepareRunner runner, PrepareRunner.CommandFactory[] factory, String gameDir,
                        String titleId) {
        PrepareProtocol.Status end;
        try {
            runner.begin();
            Environment env = environmentOverride != null ? environmentOverride : PrepareService::realEnvironment;
            try {
                factory[0] = env.prepare(this, gameDir, titleId, runner);
            } catch (Toolchain.CancelledException e) {
                factory[0] = null;
            }
            if (factory[0] == null || runner.cancelRequested())
                end = runner.finish(PrepareProtocol.CANCELLED, PrepareRunner.CANCELLED_CODE, "");
            else
                end = runner.run();
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "prepare: failed before tools.prepare ran", e);
            String code = e instanceof Toolchain.MissingAssetException ? PrepareRunner.NO_TOOLS
                          : e instanceof Toolchain.HashMismatchException ? PrepareRunner.ASSET_HASH
                          : PrepareRunner.INSTALL_FAILED;
            end = runner.finish(PrepareProtocol.FAILED, code, String.valueOf(e.getMessage()));
        }
        Log.i(TAG, "prepare: " + end.state + (end.code.isEmpty() ? "" : " " + end.code)
                   + (end.msg.isEmpty() ? "" : ": " + end.msg));
        PrepareProtocol.Status fin = end;
        new android.os.Handler(getMainLooper()).post(() -> onJobEnd(fin));
    }

    /** The real tools: the zips installed, then the command of docs/android/prepare.md. */
    static PrepareRunner.CommandFactory realEnvironment(Context c, String gameDir, String titleId,
                                                        PrepareRunner runner) throws IOException {
        File files = filesDir(c);
        File nat = Toolchain.nativeDir(c);
        Toolchain.Installed in = Toolchain.install(c, files, new Toolchain.Progress() {
            @Override
            public void onProgress(long done, long total, String what) {
                int pct = total > 0 ? (int)Math.min(100, done * 100 / total) : 100;
                runner.installProgress(pct, what);
            }

            @Override
            public boolean cancelled() {
                return runner.cancelRequested();
            }
        });
        runner.logLine("tools: " + in.python + ", " + in.sysroot + ", " + in.toolkit
                       + (in.toolkitVersion.isEmpty() ? "" : " (toolkit " + in.toolkitVersion + ")")
                       + (in.extracted > 0 ? ", " + in.extracted + " installed now" : ", already installed"));
        File cache = c.getCacheDir();
        File pycache = new File(cache, "pycache"), tmp = new File(cache, "tmp");
        //noinspection ResultOfMethodCallIgnored
        pycache.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        tmp.mkdirs();
        File work = new File(new File(files, "work"), titleId);
        runner.toolLog = new File(work, "prepare.log");
        File packs = Pack.packsDir(files);
        //noinspection ResultOfMethodCallIgnored
        work.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        packs.mkdirs();
        return new PrepareRunner.CommandFactory() {
            @Override
            public List<String> command(int jobs) {
                List<String> a = new ArrayList<>();
                a.add(new File(nat, Toolchain.PYTHON).getAbsolutePath());
                a.add("-m");
                a.add("tools.prepare");
                a.add(gameDir);
                a.add("--android");
                a.add("--work");
                a.add(work.getAbsolutePath());
                a.add("--pack-dir");
                a.add(packs.getAbsolutePath());
                a.add("--cc");
                a.add(new File(nat, Toolchain.CC).getAbsolutePath());
                a.add("--cc-prefix-arg");
                a.add("clang");
                a.add("--sysroot");
                a.add(in.sysroot.getAbsolutePath());
                a.add("--shell-lib");
                a.add(new File(nat, Toolchain.SHELL_LIB).getAbsolutePath());
                a.add("--jobs");
                a.add(String.valueOf(jobs));
                a.add("--opt");
                a.add(String.valueOf(OPT));
                a.add("--shell-abi");
                a.add(String.valueOf(Pack.SHELL_ABI));
                return a;
            }

            @Override
            public Map<String, String> environment() {
                Map<String, String> e = new HashMap<>();
                e.put("PYTHONHOME", in.python.getAbsolutePath());
                e.put("PYTHONPYCACHEPREFIX", pycache.getAbsolutePath());
                e.put("TMPDIR", tmp.getAbsolutePath());
                e.put("HOME", files.getAbsolutePath());
                e.put("LIBCAPSTONE_PATH", nat.getAbsolutePath());
                e.put("LD_LIBRARY_PATH", nat.getAbsolutePath());
                e.put("PYTHONUNBUFFERED", "1");
                e.put("PYTHONPATH", in.toolkit.getAbsolutePath());   // tools.prepare from the toolkit root
                e.put("XBA_PYTHON", new File(nat, Toolchain.PYTHON).getAbsolutePath());
                return e;
            }

            @Override
            public File workingDir() {
                return in.toolkit;
            }
        };
    }

    private void onJobEnd(PrepareProtocol.Status s) {
        current = null;
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        wakeLock = null;
        stopForeground(STOP_FOREGROUND_REMOVE);
        if (nm != null && !PrepareProtocol.CANCELLED.equals(s.state))
            nm.notify(NOTIF_DONE_ID, endNotification(s));
        stopSelf();
    }

    @Override
    public void onDestroy() {
        PrepareRunner r = current;
        if (r != null && busy())
            r.cancel();   // the system stops us: do not leave compilers behind
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        super.onDestroy();
    }

    // ---- Notification -------------------------------------------------------------------------------------------

    private void onStatus(PrepareProtocol.Status s) {
        long now = SystemClock.uptimeMillis();
        // Notification updates are rate-limited by the system: at most ~1 per second, and on a step change.
        if (!s.id.equals(lastNotifyId) || (now - lastNotify >= 1000 && s.pct != lastNotifyPct)) {
            lastNotify = now;
            lastNotifyPct = s.pct;
            lastNotifyId = s.id;
            if (nm != null && s.running())
                nm.notify(NOTIF_ID, notification(s));
        }
    }

    private PendingIntent openLauncher() {
        Intent i = new Intent(this, LauncherActivity.class).setAction(LauncherActivity.ACTION_SHOW_LAUNCHER)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    static String stepName(Context c, String id) {
        switch (id == null ? "" : id) {
        case "install":
            return c.getString(R.string.STEP_INSTALL);
        case "identify":
            return c.getString(R.string.STEP_IDENTIFY);
        case "analyse":
            return c.getString(R.string.STEP_ANALYSE);
        case "lift":
            return c.getString(R.string.STEP_LIFT);
        case "compile":
            return c.getString(R.string.STEP_COMPILE);
        case "pack":
            return c.getString(R.string.STEP_PACK);
        default:
            return id == null ? "" : id;
        }
    }

    private Notification notification(PrepareProtocol.Status s) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(titleName.isEmpty() ? getString(R.string.PREPARE_NOTIF_TITLE_PLAIN)
                                                     : getString(R.string.PREPARE_NOTIF_TITLE, titleName))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(openLauncher());
        if (Build.VERSION.SDK_INT >= 31)
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        if (s == null) {
            b.setContentText(getString(R.string.PREPARE_STARTING)).setProgress(0, 0, true);
        } else {
            String step = stepName(this, s.id);
            String text = s.step > 0 ? getString(R.string.PREPARE_NOTIF_STEP, s.step, s.n, step) : step;
            if (s.eta >= 0)
                text += " · " + getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(s.eta));
            b.setContentText(text);
            if (!s.msg.isEmpty())
                b.setSubText(s.msg.length() > 60 ? s.msg.substring(0, 60) + "…" : s.msg);
            int overall = s.step > 0 ? Math.round(PrepareProtocol.overall(s.step, s.n, s.pct) * 100) : s.pct;
            b.setProgress(100, overall, false);
        }
        String tid = s != null ? s.titleId : "";
        PendingIntent cancel = PendingIntent.getService(this, 1, cancelIntent(this, tid),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        b.addAction(new Notification.Action.Builder(null, getString(R.string.CANCEL), cancel).build());
        return b.build();
    }

    private Notification endNotification(PrepareProtocol.Status s) {
        boolean ok = PrepareProtocol.DONE.equals(s.state);
        String title = titleName.isEmpty() ? getString(R.string.app_name) : titleName;
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(ok ? android.R.drawable.stat_sys_download_done : android.R.drawable.stat_notify_error)
                .setContentTitle(title)
                .setContentText(ok ? getString(R.string.PREPARE_NOTIF_DONE)
                                   : getString(R.string.PREPARE_NOTIF_FAILED, s.code))
                .setAutoCancel(true)
                .setContentIntent(openLauncher())
                .build();
    }
}
