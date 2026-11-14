package fr.adserver.xrecomp;

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
 * ACTION_CANCEL stops it (process group SIGTERM, then SIGKILL). One Prepare at a time: a start for another title
 * while one runs joins the queue (<files>/prepare/queue.json, PrepareProtocol.Queue; that title's status.json says
 * « queued »), a start for the running title or one already waiting is ignored; ACTION_CANCEL for a waiting title
 * takes it out of the queue. When a Prepare ends, whatever its outcome, the next waiting one starts (its own wake
 * lock). The queue lives in this process: if the process dies, the next start marks the titles it held cancelled
 * (the launcher shows « queued » without a ":prepare" process as interrupted meanwhile). Started again after an
 * interruption, a Prepare runs the same command: tools.prepare's stamps skip what was done.
 */
public class PrepareService extends Service {
    static final String TAG = "xboxrecomp";
    static final String ACTION_START = "fr.adserver.xrecomp.PREPARE_START";
    static final String ACTION_CANCEL = "fr.adserver.xrecomp.PREPARE_CANCEL";
    static final String EXTRA_GAME_DIR = "game_dir", EXTRA_TITLE_ID = "title_id", EXTRA_TITLE = "title";
    /** A background Prepare (a self-healing round after a good run, XBA-129): see PrepareProtocol.Status.background. */
    static final String EXTRA_BACKGROUND = "background";
    /** clang -O level for this Prepare (XBA-128); absent: OPT, or 1 with the quick first build on. */
    static final String EXTRA_OPT = "opt";
    static final String CHANNEL = "prepare";
    static final int NOTIF_ID = 41, NOTIF_DONE_ID = 42;
    /** A Prepare holds the CPU at most this long (the wake lock's timeout). */
    static final long WAKE_LOCK_MS = 6L * 3600 * 1000;
    /** clang -O level: 2, tools.prepare's default (docs/android/prepare.md « Measurements »: 6 % slower than -O1). */
    static final int OPT = 2;
    /**
     * XBA-128, a measured option, off: with {@code <files>/prepare/quick-first} present, a Prepare builds at -O1 and,
     * once its pack is published, queues a background -O2 rebuild (Play is not blocked by it).
     */
    static final String QUICK_FIRST = "quick-first";
    /** Starts again after the Prepare's process died (START_REDELIVER_INTENT) before it is reported as killed. */
    static final int MAX_RESUMES = 3;
    /** Tests: the device's thermal status, battery and game, else the real ones. */
    static volatile PrepareMonitor.Probe probeOverride;

    /** Builds the command for a run; tests substitute a script. */
    interface Environment {
        /** Installs what the command needs (on the worker thread) and returns its factory. */
        PrepareRunner.CommandFactory prepare(Context c, String gameDir, String titleId, PrepareRunner runner)
                throws IOException;
    }

    static volatile Environment environmentOverride;

    /** The running job of this process, or null. */
    private static volatile PrepareRunner current;
    private static volatile String currentTid;
    private static volatile Thread worker;
    /** The waiting jobs (main thread only); null until this process read the queue file left by an earlier one. */
    private static PrepareProtocol.Queue queue;

    private PowerManager.WakeLock wakeLock;
    private PrepareMonitor monitor;
    private String lastGameDir = "";
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

    /** Tests: drops this process's queue (as a new process would start without one). */
    static void forgetQueue() {
        queue = null;
    }

    /** The waiting titles, in order (tests; the launcher reads queue.json). */
    static java.util.List<String> queuedTitleIds() {
        java.util.List<String> l = new ArrayList<>();
        if (queue != null) {
            for (PrepareProtocol.Job j : queue.jobs)
                l.add(j.titleId);
        }
        return l;
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
        boolean background = intent.getBooleanExtra(EXTRA_BACKGROUND, false);
        int opt = intent.getIntExtra(EXTRA_OPT, 0);
        if (opt <= 0)
            opt = !background && quickFirst(this) ? 1 : OPT;
        // startForegroundService() needs startForeground() even when nothing new starts.
        try {
            goForeground(notification(null));
        } catch (RuntimeException e) {   // a redelivered start the system does not let run in the foreground now
            Log.w(TAG, "prepare: cannot go to the foreground: " + e);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        int resumes = 0;
        if ((flags & START_FLAG_REDELIVERY) != 0 && titleId != null) {
            // XBA-128: the Prepare's process died (the system killed it) and Android started it again with its intent.
            PrepareProtocol.Status prev = PrepareProtocol.readStatus(statusFile(this, titleId));
            boolean waiting = prev != null && prev.queued();   // it was queued: a plain start
            if (prev != null && !prev.running() && !waiting) {
                Log.i(TAG, "prepare: " + titleId + " redelivered but " + prev.state + ": nothing to resume");
                dropStart(startId);
                return START_NOT_STICKY;
            }
            resumes = waiting ? 0 : (prev == null ? 0 : prev.resumes) + 1;
            if (resumes > MAX_RESUMES) {
                Log.w(TAG, "prepare: " + titleId + "'s process died " + resumes + " times: given up");
                markKilled(titleId, resumes);
                dropStart(startId);
                return START_NOT_STICKY;
            }
            if (resumes > 0)
                Log.i(TAG, "prepare: resuming " + titleId + " after its process died (" + resumes + ")");
        }
        boolean busy = busy();
        if (gameDir == null || titleId == null || titleId.isEmpty()) {
            Log.w(TAG, "prepare: start without a game or title ID");
            if (!busy) {
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf(startId);
            }
            return START_NOT_STICKY;
        }
        loadQueue();
        if (busy) {
            PrepareProtocol.Job j = new PrepareProtocol.Job(titleId, gameDir, title);
            j.background = background;
            j.opt = opt;
            enqueue(j);
            return START_REDELIVER_INTENT;
        }
        titleName = title == null ? "" : title;
        startJob(gameDir, titleId, background, opt, resumes);
        // XBA-128: if this process dies (low memory, the phantom process killer), Android starts the service again
        // with this intent; the stamps skip what was done (MAX_RESUMES times).
        return START_REDELIVER_INTENT;
    }

    /**
     * The queue of this process. The first time, a queue file left by a dead process is emptied and the titles it
     * held are marked cancelled (their Prepare never started; the launcher offers it again).
     */
    private void loadQueue() {
        if (queue != null)
            return;
        queue = new PrepareProtocol.Queue();
        File f = PrepareProtocol.queueFile(filesDir(this));
        PrepareProtocol.Queue stale = PrepareProtocol.readQueue(f);
        for (PrepareProtocol.Job j : stale.jobs)
            markCancelled(j.titleId, PrepareProtocol.QUEUED);
        if (f.exists())
            saveQueue();
    }

    private void saveQueue() {
        File f = PrepareProtocol.queueFile(filesDir(this));
        try {
            PrepareProtocol.writeQueue(f, queue);
        } catch (IOException e) {
            Log.w(TAG, "prepare: cannot write " + f + ": " + e);
        }
    }

    /**
     * A start while a Prepare runs: queued, unless it is the running title (ignored). A title already waiting keeps
     * its place; its status is written « queued » again (the launcher writes a fresh status before each start).
     */
    private void enqueue(PrepareProtocol.Job j) {
        lastNotifyId = "";   // goForeground(notification(null)) reset the running job's notification: refresh it
        if (j.titleId.equals(currentTid)) {
            Log.i(TAG, "prepare: " + j.titleId + " is already running, start ignored");
            return;
        }
        if (queue.add(j))
            saveQueue();
        else
            Log.i(TAG, "prepare: " + j.titleId + " is already queued");
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.QUEUED;
        s.titleId = j.titleId;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        s.log = logFile(this, j.titleId).getAbsolutePath();
        s.background = j.background;
        File f = statusFile(this, j.titleId);
        try {
            PrepareProtocol.writeStatus(f, s);
        } catch (IOException e) {
            Log.w(TAG, "prepare: cannot write " + f + ": " + e);
        }
        Log.i(TAG, "prepare: " + j.titleId + " queued (" + queue.jobs.size() + " waiting)");
    }

    /** Sets titleId's status.json to cancelled when its state is one of from (a Prepare that will not run). */
    private void markCancelled(String titleId, String... from) {
        File f = statusFile(this, titleId);
        PrepareProtocol.Status s = PrepareProtocol.readStatus(f);
        if (s == null || !java.util.Arrays.asList(from).contains(s.state))
            return;
        s.state = PrepareProtocol.CANCELLED;
        s.code = PrepareRunner.CANCELLED_CODE;
        s.updatedAt = System.currentTimeMillis();
        try {
            PrepareProtocol.writeStatus(f, s);
        } catch (IOException e) {
            Log.w(TAG, "prepare: cannot write " + f + ": " + e);
        }
    }

    private void goForeground(Notification n) {
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIF_ID, n);
    }

    private void onCancel(String titleId) {
        loadQueue();
        if (titleId != null && queue.remove(titleId)) {
            saveQueue();
            markCancelled(titleId, PrepareProtocol.QUEUED);
            Log.i(TAG, "prepare: " + titleId + " taken out of the queue");
            if (!busy())
                stopSelf();
            return;
        }
        PrepareRunner r = current;
        if (r != null && busy()) {
            if (titleId != null && !titleId.isEmpty() && !titleId.equals(currentTid)) {
                Log.i(TAG, "prepare: cancel for " + titleId + ", which neither runs nor waits: ignored");
                return;
            }
            Log.i(TAG, "prepare: cancel requested");
            r.cancel();
            return;
        }
        // Nothing runs here (the process was restarted): a status left « running » or « queued » by a dead process is
        // cancelled.
        if (titleId != null)
            markCancelled(titleId, PrepareProtocol.RUNNING, PrepareProtocol.QUEUED);
        stopSelf();
    }

    /** A start that runs nothing: out of the foreground and stopped, unless a Prepare runs. */
    private void dropStart(int startId) {
        if (busy())
            return;
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf(startId);
    }

    /** XBA-128: the quick first build (-O1, then a background -O2) is on (a file, for measuring it on a device). */
    static boolean quickFirst(Context c) {
        return new File(new File(filesDir(c), "prepare"), QUICK_FIRST).isFile();
    }

    /** A Prepare whose process died too often: failed, KILLED. */
    private void markKilled(String titleId, int resumes) {
        File f = statusFile(this, titleId);
        PrepareProtocol.Status s = PrepareProtocol.readStatus(f);
        if (s == null)
            return;
        s.state = PrepareProtocol.FAILED;
        s.code = PrepareRunner.KILLED;
        s.msg = "the Prepare's process was killed " + resumes + " times";
        s.resumes = resumes;
        s.updatedAt = System.currentTimeMillis();
        try {
            PrepareProtocol.writeStatus(f, s);
        } catch (IOException e) {
            Log.w(TAG, "prepare: cannot write " + f + ": " + e);
        }
    }

    static File workDir(Context c, String titleId) {
        return new File(new File(filesDir(c), "work"), titleId);
    }

    private void startJob(String gameDir, String titleId, boolean background, int opt, int resumes) {
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
        runner.setBackground(background);
        runner.setOpt(opt);
        runner.setResumes(resumes);
        PrepareMonitor.Probe probe = probeOverride != null ? probeOverride : PrepareMonitor.deviceProbe(this);
        monitor = new PrepareMonitor(workDir(this, titleId), jobs, background, probe);
        monitor.start();
        current = runner;
        currentTid = titleId;
        lastGameDir = gameDir;
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "xboxrecomp:prepare");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire(WAKE_LOCK_MS);
        }
        Thread t = new Thread(() -> runJob(runner, factory, gameDir, titleId), "xba-prepare");
        worker = t;
        t.start();
        Log.i(TAG, "prepare: started for " + titleId + " (" + gameDir + "), " + jobs + " jobs, -O" + opt
                   + (background ? ", in the background" : "") + (resumes > 0 ? ", resumed " + resumes : ""));
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
        if (runner.background()) {   // XBA-129: the launcher ends the other rounds (it relaunches the game)
            try {
                SelfHeal.roundEnded(new File(new File(filesDir(this), "work"), titleId),
                                    PrepareProtocol.DONE.equals(end.state));
            } catch (IOException e) {
                Log.w(TAG, "prepare: cannot update " + titleId + "'s selfheal.json: " + e);
            }
        }
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
        boolean rom = Console.ofKey(titleId) != Console.XBOX;
        if (!rom) {
            try {   // what the runs find from now on is new to this Prepare's analysis (XBA-74)
                SelfHeal.prepareStarted(work);
            } catch (IOException e) {
                runner.logLine("selfheal.json not updated: " + e.getMessage());
            }
        }
        // A GBA game (XBA-158): its ROM, a path, or a library folder's document that tools.prepare cannot open: then
        // copied into the work folder first (32 MB at most), where tools.prepare keeps its own copy anyway. A PS1 disc
        // (XBA-168): a path as it is; a document: the sectors that lead to its boot EXE, copied (Ps1Disc).
        final String source = !rom ? gameDir
                              : Console.ofKey(titleId) == Console.PS1 ? ps1Source(c, gameDir, titleId, work, runner)
                              : romSource(c, gameDir, work, runner);
        return new PrepareRunner.CommandFactory() {
            @Override
            public List<String> command(int jobs) {
                List<String> a = new ArrayList<>();
                a.add(new File(nat, Toolchain.PYTHON).getAbsolutePath());
                a.add("-m");
                a.add("tools.prepare");
                a.add(source);
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
                a.add(String.valueOf(runner.opt() > 0 ? runner.opt() : OPT));
                a.add("--device-log");   // XBA-128: PrepareMonitor's samples, read before each compile
                a.add(PrepareMonitor.logFile(work).getAbsolutePath());
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
                if (!rom && !GameStore.enhancements(c, titleId))   // Settings › Enhancements off (profile-free WP6)
                    e.put("XBA_NO_TITLE_LAYER", "1");
                return e;
            }

            @Override
            public File workingDir() {
                return in.toolkit;
            }
        };
    }

    /** The largest ROM copied for a GBA Prepare (gba_runtime.c GBA_ROM_MAX). */
    static final long ROM_MAX = 32L << 20;

    /**
     * The file tools.prepare reads for a GBA game: its path as it is, or a content URI (a library folder seen through
     * the Storage Access Framework) copied to &lt;work&gt;/rom.gba.
     */
    static String romSource(Context c, String spec, File work, PrepareRunner runner) throws IOException {
        if (!spec.startsWith("content:"))
            return spec;
        File out = new File(work, "rom.gba"), tmp = new File(work, "rom.gba.tmp");
        long n = 0;
        try (java.io.InputStream in = c.getContentResolver().openInputStream(android.net.Uri.parse(spec));
             java.io.FileOutputStream o = new java.io.FileOutputStream(tmp)) {
            if (in == null)
                throw new IOException("cannot open " + spec);
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) > 0) {
                n += r;
                if (n > ROM_MAX)
                    throw new IOException(spec + " is over 32 MB: not a GBA ROM");
                o.write(buf, 0, r);
            }
            o.getFD().sync();
        } catch (SecurityException e) {
            throw new IOException("no access to " + spec + ": " + e.getMessage());
        }
        if (!tmp.renameTo(out))
            throw new IOException("cannot write " + out);
        if (runner != null)
            runner.logLine("rom: " + spec + " copied to " + out + " (" + n + " bytes)");
        return out.getAbsolutePath();
    }

    /**
     * The file tools.prepare reads for a PS1 disc (XBA-168): its path as it is (a .cue finds its tracks beside it); a
     * document of a library folder seen through the Storage Access Framework (a single-file .bin / .iso, or a .cue
     * whose track documents the library index lists): the sectors that lead to the boot EXE, copied from its data
     * track (the .cue's first FILE) into &lt;work&gt;/disc.iso (Ps1Disc: a sparse image of a few hundred KB on disk).
     * A .chd cannot be prepared (tools.prepare reads .cue / .bin / .iso).
     */
    static String ps1Source(Context c, String spec, String titleId, File work, PrepareRunner runner)
            throws IOException {
        if (!spec.startsWith("content:"))
            return spec;
        android.content.ContentResolver cr = c.getContentResolver();
        String name = documentName(c, spec);
        String low = (name != null ? name : android.net.Uri.parse(spec).getLastPathSegment() + "")
                .toLowerCase(java.util.Locale.ROOT);
        if (low.endsWith(".chd"))
            throw new IOException(name + ": a .chd image cannot be prepared (.cue / .bin / .iso only); it still plays");
        String track = spec;
        if (low.endsWith(".cue")) {
            String text;
            try (java.io.InputStream in = cr.openInputStream(android.net.Uri.parse(spec))) {
                if (in == null)
                    throw new IOException("cannot open " + spec);
                byte[] buf = new byte[Ps1Cue.MAX_CUE];
                int n = 0, r;
                while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0)
                    n += r;
                text = new String(buf, 0, n, java.nio.charset.StandardCharsets.ISO_8859_1);
            } catch (SecurityException e) {
                throw new IOException("no access to " + spec + ": " + e.getMessage());
            }
            List<String> files = Console.cueFiles(text);
            if (files.isEmpty())
                throw new IOException(name + " names no track (no FILE line)");
            String want = Ps1Cue.baseName(files.get(0));
            track = null;
            for (String part : cueParts(filesDir(c), spec)) {
                String pn = documentName(c, part);
                if (pn != null && pn.toLowerCase(java.util.Locale.ROOT).equals(want)) {
                    track = part;
                    break;
                }
            }
            if (track == null)
                throw new IOException(name + " names " + files.get(0) + ", not found beside it");
        }
        File out = new File(work, "disc.iso");
        Ps1Disc.Result r;
        try (android.os.ParcelFileDescriptor pfd = cr.openFileDescriptor(android.net.Uri.parse(track), "r");
             java.io.FileInputStream in = new java.io.FileInputStream(pfd.getFileDescriptor())) {
            r = Ps1Disc.extract(Console.source(in.getChannel()), out);
        } catch (SecurityException | NullPointerException e) {
            throw new IOException("no access to " + track + ": " + e.getMessage());
        }
        if (runner != null)
            runner.logLine("disc: " + track + " -> " + out + " (boot " + r.boot + ", " + r.exeSize + " bytes, "
                           + r.sectors + " sectors copied)");
        return out.getAbsolutePath();
    }

    /** A document's display name, or null. */
    static String documentName(Context c, String uri) {
        try (android.database.Cursor cur = c.getContentResolver().query(android.net.Uri.parse(uri),
                new String[] { android.provider.DocumentsContract.Document.COLUMN_DISPLAY_NAME }, null, null, null)) {
            return cur != null && cur.moveToFirst() ? cur.getString(0) : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    /**
     * The track documents of the .cue document cueUri, as the library index lists them (LibraryIndex.Item.parts:
     * « doc: » IDs under the same tree); empty when the index does not know it.
     */
    static List<String> cueParts(File filesDir, String cueUri) {
        List<String> out = new ArrayList<>();
        android.net.Uri u = android.net.Uri.parse(cueUri);
        String doc, tree;
        try {
            doc = android.provider.DocumentsContract.getDocumentId(u);
            tree = android.provider.DocumentsContract.getTreeDocumentId(u);
        } catch (RuntimeException e) {
            return out;
        }
        android.net.Uri treeUri = android.provider.DocumentsContract.buildTreeDocumentUri(u.getAuthority(), tree);
        LibraryIndex index = LibraryIndex.read(Library.indexFile(filesDir));
        for (LibraryIndex.Item it : index.items)
            if (it.loc.equals(LibraryIndex.docLoc(doc))) {
                for (String loc : it.parts)
                    if (loc.startsWith("doc:"))
                        out.add(android.provider.DocumentsContract.buildDocumentUriUsingTree(treeUri, loc.substring(4))
                                .toString());
                return out;
            }
        return out;
    }

    private void onJobEnd(PrepareProtocol.Status s) {
        PrepareRunner done = current;
        current = null;
        currentTid = null;
        if (monitor != null)
            monitor.stop();
        monitor = null;
        // XBA-128, quick first: the -O1 pack is published; the -O2 rebuild follows in the background.
        if (done != null && done.opt() == 1 && PrepareProtocol.DONE.equals(s.state) && quickFirst(this)) {
            loadQueue();
            PrepareProtocol.Job o2 = new PrepareProtocol.Job(s.titleId, lastGameDir, titleName);
            o2.background = true;
            o2.opt = 2;
            if (queue.add(o2)) {
                saveQueue();
                Log.i(TAG, "prepare: " + s.titleId + " built at -O1, the -O2 rebuild queued in the background");
            }
        }
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        wakeLock = null;
        // One end notification per title (tag = title ID): a queue's outcomes do not replace each other.
        if (nm != null && !PrepareProtocol.CANCELLED.equals(s.state))
            nm.notify(s.titleId, NOTIF_DONE_ID, endNotification(s));
        PrepareProtocol.Job next = queue == null ? null : queue.pop();
        if (next != null) {
            saveQueue();
            Log.i(TAG, "prepare: next in the queue: " + next.titleId);
            titleName = next.title;
            lastNotifyId = "";
            goForeground(notification(null));
            startJob(next.gameDir, next.titleId, next.background, next.opt > 0 ? next.opt : OPT, 0);
            return;
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (monitor != null)
            monitor.stop();
        PrepareRunner r = current;
        if (r != null && busy())
            r.cancel();   // the system stops us: do not leave compilers behind
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        // The waiting titles will not run: cancelled, so that a live process does not show them queued for ever.
        if (queue != null && !queue.jobs.isEmpty()) {
            for (PrepareProtocol.Job j : queue.jobs)
                markCancelled(j.titleId, PrepareProtocol.QUEUED);
            queue.jobs.clear();
            saveQueue();
        }
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
