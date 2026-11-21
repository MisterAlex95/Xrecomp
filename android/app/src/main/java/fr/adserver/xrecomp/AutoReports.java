package fr.adserver.xrecomp;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;

/**
 * « Send reports automatically » at work (XBA-336), behind Telemetry's gate:
 * - Release health: one Sentry session per game run. The launcher learns how the last run ended from the main
 *   process's newest ApplicationExitInfo (the game runs in the package's main process), counted once: the exit's
 *   timestamp is kept in files/telemetry/counted, and a run with no exit record yet is not over (it waits for the
 *   next look). The run waits in files/telemetry/runs/ as sid, start, end and status only; the session envelope is
 *   built when it leaves, with the install id of that moment (none on disk after off or « Reset »).
 * - Crash and Prepare failure reports: the same envelope as « Send report » (CrashReport.envelope, scrubbed), built
 *   when the launcher sees the failure (the log is fresh then) into files/telemetry/outbox/.
 * - ReportJobService (JobScheduler, no WorkManager) posts both through CrashReport.sendAuto, on an unmetered network
 *   unless « Also on mobile data » is on. Never through CrashReport.send (its files/reports/ queue leaves ungated).
 * Turning the option off deletes all of it and cancels the job (Telemetry.setAuto). When it is off nothing is kept
 * but the timestamp of the last counted run (to know a new crash) and the version a bubble was offered for.
 */
final class AutoReports {
    private static final String TAG = "xba";
    static final int JOB_ID = 0x58420336;
    /** What waits at most, per folder, and for how long. */
    static final int KEEP_MAX = 10;
    static final long KEEP_MS = 7L * 24 * 3600_000L;
    static final int PREPARE_SEEN = 32;
    static final String EXITED = "exited", CRASHED = "crashed", ABNORMAL = "abnormal";

    private AutoReports() {}

    // ---- Where the game's runs end ----------------------------------------------------------------------------

    /** One end of the game's process: its pid, when, ApplicationExitInfo's reason in words (LogProvider.reasonName). */
    static final class Exit {
        final int pid, status;
        final long time;
        final String reason;

        Exit(int pid, long time, String reason, int status) {
            this.pid = pid;
            this.time = time;
            this.reason = reason;
            this.status = status;
        }
    }

    /** The main process's ends, newest first; a field so the tests use a fake. */
    interface Exits {
        List<Exit> list(Context c);
    }

    static final Exits SYSTEM_EXITS = AutoReports::systemExits;
    static Exits exits = SYSTEM_EXITS;

    /** Writes the last game's log (LogProvider.dumpLastGame); a field so the tests need no logcat. */
    interface Dumper {
        boolean dump(Context c, File f);
    }

    static final Dumper LOGCAT = LogProvider::dumpLastGame;
    static Dumper dumper = LOGCAT;

    private static List<Exit> systemExits(Context c) {
        List<Exit> out = new ArrayList<>();
        android.app.ActivityManager am = c.getSystemService(android.app.ActivityManager.class);
        if (am == null)
            return out;
        try {
            for (android.app.ApplicationExitInfo x : am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 16))
                if (c.getPackageName().equals(x.getProcessName()))
                    out.add(new Exit(x.getPid(), x.getTimestamp(), LogProvider.reasonName(x.getReason()),
                                     x.getStatus()));
        } catch (RuntimeException e) {
            // no record
        }
        return out;
    }

    /**
     * A run's session status from how its process ended: a crash (native or Java) is crashed; the game leaving by
     * itself (status 0), the player stopping it, or the launcher ending it (endedByLauncher: the pid it killed) is
     * exited; everything else (not responding, killed for memory, a signal from elsewhere, a failing exit) abnormal.
     */
    static String status(String reason, int exitStatus, boolean endedByLauncher) {
        if (reason == null)
            return ABNORMAL;
        switch (reason) {
        case "native crash":
        case "Java exception":
            return CRASHED;
        case "exited":
            return exitStatus == 0 ? EXITED : ABNORMAL;
        case "stopped by the user":
            return EXITED;
        case "killed by a signal":
            return endedByLauncher ? EXITED : ABNORMAL;
        default:
            return ABNORMAL;
        }
    }

    /**
     * The launcher ends the game's process itself (endRunningGame, before another game or a quick reboot): that end
     * is not abnormal, and its launch (start) is noted, since the next launch replaces GameStore.lastPlayed.
     */
    static void launcherEnded(Context c, int pid, long start) {
        write(f(c, "ended-pid"), pid + "\t" + start);
    }

    /** What collect learnt: the run that ended (its status), or null when no new run ended. */
    static final class Run {
        final String status, reason;
        final long start, end;

        Run(String status, String reason, long start, long end) {
            this.status = status;
            this.reason = reason;
            this.start = start;
            this.end = end;
        }

        /** A crash the player could report: what the « Good to know » bubble is about. */
        boolean failed() {
            return CRASHED.equals(status) || "not responding".equals(reason);
        }
    }

    /**
     * Looks once at how the last game run ended (the launcher calls it on resume, off the UI thread): null while the
     * game runs, while its end has no record yet, or when that end was already counted. A new end is counted (its
     * timestamp kept); with the option on, its session waits in runs/ and a crash's report in outbox/, and the job is
     * scheduled. The first look ever only starts counting (a run from before is not news).
     */
    static synchronized Run collect(Context c, boolean gameRunning) {
        String m = read(f(c, "counted"));
        if (m == null) {
            write(f(c, "counted"), String.valueOf(System.currentTimeMillis()));
            return null;
        }
        long counted = parse(m), last = GameStore.lastPlayed(c), launchCounted = parse(read(f(c, "launch")));
        String[] ended = String.valueOf(read(f(c, "ended-pid"))).split("\t");
        long endedPid = ended.length == 2 ? parse(ended[0]) : -1, endedStart = ended.length == 2 ? parse(ended[1]) : 0;
        List<Exit> all = exits.list(c);
        // One run per launch (its start: GameStore.lastPlayed, or what the launcher noted when it ended that
        // process to start another game); a launch whose process was restarted (a quick reboot) ends at its last end.
        java.util.TreeMap<Long, Exit> runs = new java.util.TreeMap<>();
        for (Exit x : all) {
            if (x.time <= counted)
                continue;
            long start = x.pid == endedPid && endedStart > 0 ? endedStart : last > 0 && x.time >= last ? last : 0;
            if (start <= 0 || start > x.time || start <= launchCounted)
                continue;   // a process whose launch is unknown (a library check's) or already counted
            Exit prev = runs.get(start);
            if (prev == null || x.time > prev.time)
                runs.put(start, x);
        }
        if (gameRunning)
            runs.remove(last);   // the last launch is still playing (its process restarted)
        if (runs.isEmpty())
            return null;
        long newest = counted;
        Run worst = null;
        boolean on = enabled(c);
        for (java.util.Map.Entry<Long, Exit> en : runs.entrySet()) {
            Exit e = en.getValue();
            long start = en.getKey();
            newest = Math.max(newest, e.time);
            Run run = new Run(status(e.reason, e.status, e.pid == endedPid), e.reason, start, e.time);
            Log.i(TAG, "reports: a game's run ended " + run.status + " (" + e.reason + ")");
            if (worst == null || run.failed() || !worst.failed())
                worst = run;
            try {   // its performance (GamePerf: the option on, not for a crash); never in the way of the rest
                if (on)
                    GamePerf.runEnded(c, e.pid, run.status, e.reason, start, e.time);
                else
                    GamePerf.discard(c, e.pid);
            } catch (RuntimeException ex) {
                Log.w(TAG, "reports: game performance not sent: " + ex);
            }
            if (!on)
                continue;
            write(new File(dir(c, "runs"), e.time + ".run"),
                  UUID.randomUUID().toString().replace("-", "") + "\t" + start + "\t" + e.time + "\t" + run.status);
            if (run.failed() && e == all.get(0)) {   // the log of the newest end only (LogProvider.dumpLastGame)
                File log = new File(Telemetry.dir(c), "last-game.tmp");
                if (dumper.dump(c, log))
                    outbox(c, e.time + "-game", CrashReport.game(c, GameStore.sessionTid(c), null, log));
                log.delete();
            }
        }
        write(f(c, "counted"), String.valueOf(newest));
        write(f(c, "launch"), String.valueOf(runs.lastKey()));   // a launch is one run, whatever ends after
        if (on) {
            prune(c);
            schedule(c);
        }
        return worst;
    }

    /**
     * A failed Prepare the launcher shows (its screen rebuilds on every poll: seen once per title and status time).
     * True when it is news; with the option on (and the failure after it was turned on) its report goes to outbox/.
     */
    static synchronized boolean prepareFailed(Context c, String tid, String title, PrepareProtocol.Status s,
                                              File log) {
        if (tid == null || s == null)
            return false;
        String id = tid + "@" + s.updatedAt;
        List<String> seen = new ArrayList<>(Arrays.asList(String.valueOf(read(f(c, "prepare-seen"))).split("\n")));
        if (seen.contains(id))
            return false;
        seen.removeIf(x -> x.startsWith(tid + "@") || x.isEmpty() || x.equals("null"));
        seen.add(id);   // one per title, the last PREPARE_SEEN titles
        write(f(c, "prepare-seen"), String.join("\n", seen.subList(Math.max(0, seen.size() - PREPARE_SEEN),
                                                                   seen.size())));
        if (enabled(c) && s.updatedAt >= parse(read(f(c, "since")))) {
            outbox(c, s.updatedAt + "-prepare", CrashReport.prepare(c, tid, title, s.code, s.msg, log));
            prune(c);
            schedule(c);
        }
        return true;
    }

    private static void outbox(Context c, String name, CrashReport r) {
        String id = UUID.randomUUID().toString().replace("-", "");
        try {
            File d = dir(c, "outbox");
            if (d.mkdirs() || d.isDirectory())
                Files.write(new File(d, name + ".envelope").toPath(), r.envelope(id, System.currentTimeMillis()));
        } catch (IOException e) {
            Log.w(TAG, "reports: cannot keep the report: " + e);
        }
    }

    // ---- The option ----------------------------------------------------------------------------------------------

    /** This app can send at all: a DSN, and not a game's own app (TitleApk drops INTERNET from those). */
    static boolean possible(Context c) {
        return CrashReport.available(c) && TitleApp.ownTitleId(c) == null && ConsoleApp.ownKey(c) == null;
    }

    /** The option is on here. */
    static boolean enabled(Context c) {
        return possible(c) && Telemetry.auto(c);
    }

    /** Called by Telemetry.setAuto: on, only what happens from now counts; off, everything waiting goes. */
    static void optionChanged(Context c, boolean on) {
        if (on) {
            String now = String.valueOf(System.currentTimeMillis());
            write(f(c, "counted"), now);
            write(f(c, "since"), now);
            return;
        }
        wipe(dir(c, "runs"));
        wipe(dir(c, "outbox"));
        f(c, "since").delete();
        cancel(c);
    }

    /** « Also on mobile data »: off by default (Wi-Fi and other unmetered networks only). */
    static boolean mobileData(Context c) {
        return f(c, "mobile").isFile();
    }

    static void setMobileData(Context c, boolean on) {
        if (on)
            write(f(c, "mobile"), "1");
        else
            f(c, "mobile").delete();
        if (enabled(c) && pending(c) > 0)
            schedule(c);
    }

    // ---- « Good to know » ---------------------------------------------------------------------------------------

    /** The bubble offering the option: after a crash or a failed Prepare, the option off, once per app version. */
    static boolean offerDue(Context c) {
        return possible(c) && !Telemetry.auto(c)
               && !c.getString(R.string.xba_version).equals(read(f(c, "offered")));
    }

    static void offered(Context c) {
        write(f(c, "offered"), c.getString(R.string.xba_version));
    }

    // ---- Sending (ReportJobService) -----------------------------------------------------------------------------

    /** What waits: sessions and reports. */
    static int pending(Context c) {
        return files(dir(c, "runs"), ".run").length + files(dir(c, "outbox"), ".envelope").length;
    }

    /** The job: a network that fits the option, nothing persisted across a reboot (the launcher schedules again). */
    static void schedule(Context c) {
        JobScheduler js = c.getSystemService(JobScheduler.class);
        if (js == null)
            return;
        try {
            js.schedule(new JobInfo.Builder(JOB_ID, new ComponentName(c, ReportJobService.class))
                                .setRequiredNetworkType(mobileData(c) ? JobInfo.NETWORK_TYPE_ANY
                                                                      : JobInfo.NETWORK_TYPE_UNMETERED)
                                .setBackoffCriteria(60_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL).build());
        } catch (RuntimeException e) {
            Log.w(TAG, "reports: cannot schedule the job: " + e);
        }
    }

    static void cancel(Context c) {
        JobScheduler js = c.getSystemService(JobScheduler.class);
        if (js != null)
            js.cancel(JOB_ID);
    }

    /**
     * Posts what waits, through the gate: the option off, everything goes unsent; the gate closed (refused version,
     * back-off), it waits for the next look. Sessions are built now, with the install id of now. True when something
     * could not leave though the gate is open (a network failure: the job tries again later).
     */
    static synchronized boolean flush(Context c) {
        if (!enabled(c)) {
            optionChanged(c, false);
            return false;
        }
        prune(c);
        if (!Telemetry.allowed(c))
            return false;
        boolean failed = false;
        String release = CrashReport.release(c), env = CrashReport.environment(c);
        for (File r : files(dir(c, "runs"), ".run")) {
            String[] p = String.valueOf(read(r)).split("\t");
            if (p.length < 4) {
                r.delete();
                continue;
            }
            long start = parse(p[1]), end = parse(p[2]);
            byte[] body = CrashReport.sessionEnvelope(p[0], Telemetry.installId(c), true, p[3], start, end,
                                                      CRASHED.equals(p[3]) ? 1 : 0, release, env);
            if (CrashReport.sendAuto(c, body))
                r.delete();
            else
                failed = true;
        }
        for (File o : files(dir(c, "outbox"), ".envelope")) {
            byte[] body;
            try {
                body = Files.readAllBytes(o.toPath());
            } catch (IOException e) {
                o.delete();
                continue;
            }
            if (CrashReport.sendAuto(c, body))
                o.delete();
            else
                failed = true;
        }
        Log.i(TAG, "reports: automatic items sent" + (failed ? ", some wait" : ""));
        return failed && Telemetry.allowed(c);
    }

    /** At most KEEP_MAX per folder (the newest), none older than KEEP_MS. */
    private static void prune(Context c) {
        long old = System.currentTimeMillis() - KEEP_MS;
        for (File[] all : Arrays.asList(files(dir(c, "runs"), ".run"), files(dir(c, "outbox"), ".envelope")))
            for (int i = 0; i < all.length; i++)
                if (i < all.length - KEEP_MAX || all[i].lastModified() < old)
                    all[i].delete();
    }

    // ---- Files ----------------------------------------------------------------------------------------------

    private static File f(Context c, String name) {
        return new File(Telemetry.dir(c), name);
    }

    private static File dir(Context c, String name) {
        return new File(Telemetry.dir(c), name);
    }

    /** The files of d ending in ext, oldest (by name: a timestamp first) first. */
    private static File[] files(File d, String ext) {
        File[] f = d.listFiles((x, n) -> n.endsWith(ext));
        if (f == null)
            return new File[0];
        Arrays.sort(f, (a, b) -> a.getName().compareTo(b.getName()));
        return f;
    }

    private static void wipe(File d) {
        File[] f = d.listFiles();
        if (f != null)
            for (File x : f)
                x.delete();
        d.delete();
    }

    private static long parse(String s) {
        try {
            return s == null ? 0 : Long.parseLong(s.trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String read(File f) {
        try {
            return f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    private static void write(File f, String s) {
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return;
        File tmp = new File(f.getPath() + ".tmp");
        try {
            Files.write(tmp.toPath(), s.getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(f))
                tmp.delete();
        } catch (IOException e) {
            tmp.delete();
        }
    }
}
