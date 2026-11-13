package fr.adserver.xboxrecomp;

import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * The library check's steps (XBA-130): a state machine over {@link LibraryCheck.State}, advanced by {@link #tick}
 * (CheckService calls it twice a second on its own thread), with every effect on the device behind {@link Platform}
 * so that the JVM tests drive it with fakes and a fake clock.
 *
 * <p>Per title: PENDING → (a Prepare when {@link Platform#needsPrepare}) PREPARING → READY → BOOTING → GRADED. The
 * state is written after each change: started again after a crash of the app or a reboot, the check goes on with the
 * title it was at. A boot it was in the middle of is graded from what that run left (blocker « the check was
 * interrupted ... », so a title that takes the device down cannot loop), a Prepare it was waiting for is waited for
 * or started again (tools.prepare's stamps skip what was done).
 *
 * <p>Pauses: the game left the screen during a boot (the player went away, the screen turned off; the run is thrown
 * away and done again on return: a game in the background is held by the runtime, its numbers would mean nothing),
 * the check screen is not shown when a Prepare or a boot has to start (Android lets an app start those only from the
 * screen), the device reports a severe thermal status (a running Prepare goes on with fewer compile jobs, XBA-128;
 * nothing new starts until it cooled down),
 * or the battery runs low while not charging.
 */
final class CheckDriver {
    static final String TAG = "xboxrecomp";
    /** The game window may take this long to appear before « not on screen » counts. */
    static final long START_GRACE_MS = 15_000;
    /** The game off the screen this long during a boot: interrupted. */
    static final long AWAY_MS = 3_000;
    /** No game process this long: it ended (a quick reboot leaves a gap of a second or two). */
    static final long GONE_MS = 4_000;
    /** The process never came up after the launch. */
    static final long NO_START_MS = 30_000;
    /** After the run's seconds, the game is ended; past this much more it is ended again, harder. */
    static final long END_GRACE_MS = 30_000;
    /** Peak memory sampled this often. */
    static final long SAMPLE_MS = 5_000;
    /** Thermal statuses (PowerManager.THERMAL_STATUS_*): severe pauses, moderate or less resumes (DeviceGuard). */
    static final int THERMAL_SEVERE = DeviceGuard.THERMAL_SEVERE, THERMAL_MODERATE = DeviceGuard.THERMAL_MODERATE;
    /** Not charging and below this: pause; resume when charging or at RESUME_BATTERY (DeviceGuard). */
    static final int LOW_BATTERY = DeviceGuard.LOW_BATTERY, RESUME_BATTERY = DeviceGuard.RESUME_BATTERY;
    /** Interrupted Prepares (killed, cancelled by the system) started again before it is reported. */
    static final int MAX_PREPARE_ATTEMPTS = 3;
    /** Boots interrupted by the player leaving before the title is graded as it is. */
    static final int MAX_BOOT_ATTEMPTS = 3;

    /** How a game process ended, as the system tells it (ApplicationExitInfo), in tools.compat grade's words. */
    static final class Exit {
        /** --exit: exit:N, signal:N, crash[:N], java, anr, lowmem, window. */
        final String arg;
        final long time;

        Exit(String arg, long time) {
            this.arg = arg;
            this.time = time;
        }
    }

    /** What tools.compat grade gets for one title (CheckPlatform turns it into the command line). */
    static final class Grade {
        LibraryCheck.Title title;
        File work, status, log;
        String presses = "", exit = "none";
        double runS;
        int sustain = LibraryCheck.SUSTAIN;
        /** Set for a Prepare that failed (« CODE: msg ») or a title not booted (why). */
        String prepareFailed, notBooted;
        long prepareMs = -1, peakKb;
    }

    interface Platform {
        long now();

        File filesDir();

        /** The title has no valid, current pack (LibraryCheck.needsPrepare). */
        boolean needsPrepare(LibraryCheck.Title t);

        /** Starts its Prepare (PrepareService); throws when Android does not let the app start it now. */
        void startPrepare(LibraryCheck.Title t) throws Exception;

        void cancelPrepare(String tid);

        /** status.json of the title's Prepare, or null. */
        PrepareProtocol.Status prepareStatus(String tid);

        /** A Prepare process is alive (the status of a dead one says « running » for a while). */
        boolean prepareAlive();

        /** The check's screen is in front: Android lets the app start a game or a Prepare. */
        boolean canStart();

        /** Starts the game (ShellActivity in a fresh main process) for t; current.json is written. */
        void launchGame(LibraryCheck.Title t) throws Exception;

        /** The game's (main) process, 0 when there is none. */
        int gamePid();

        /** The game's window is on screen. */
        boolean gameInFront();

        /** The game's window exists (its task), on screen or not. */
        boolean gameWindow();

        /** The check's screen to the front (while the game is still visible: Android allows it then). */
        void bringCheckToFront();

        /** Ends the game's process by PID and waits for it. */
        void endGame();

        /** How the game's process ended after since, or null when the system has no record. */
        Exit exitSince(long since);

        /** The process's peak resident memory (VmHWM), kB; 0 when unknown. */
        long peakKb(int pid);

        /** The process's log lines (logcat of that PID) into out. */
        void dumpLog(int pid, File out);

        /** Grades and appends the results line (python -m tools.compat grade); returns the line. */
        JSONObject grade(Grade g) throws Exception;

        /** PowerManager's thermal status (0 none ... 6 shutdown). */
        int thermalStatus();

        int batteryPercent();

        boolean charging();

        /** The state changed (the screen and the notification). */
        void changed(LibraryCheck.State s);
    }

    final Platform p;
    LibraryCheck.State s;
    /** The pid of the boot's game process, kept for the log after the process is gone. */
    private int lastPid;

    CheckDriver(Platform p, LibraryCheck.State s) {
        this.p = p;
        this.s = s;
    }

    File files() {
        return p.filesDir();
    }

    // ---- Commands -------------------------------------------------------------------------------------------------

    /** A paused check goes on (the player's Continue, or the check screen back in front). */
    synchronized void resume() {
        if (!LibraryCheck.PAUSED.equals(s.state))
            return;
        if (LibraryCheck.HOT.equals(s.reason) && DeviceGuard.hot(p.thermalStatus()))
            return;
        Log.i(TAG, "check: resumed (" + s.reason + ")");
        s.state = LibraryCheck.RUNNING;
        s.reason = "";
        save();
    }

    /** The player's Pause: what runs finishes its step, nothing new starts. */
    synchronized void pause() {
        if (LibraryCheck.RUNNING.equals(s.state))
            pauseFor(LibraryCheck.USER);
    }

    /** Stop: the running Prepare is cancelled, the game ended; the results so far stay. */
    synchronized void stop() {
        if (!s.active())
            return;
        LibraryCheck.Title t = s.current();
        if (t != null && LibraryCheck.PREPARING.equals(t.phase)) {
            p.cancelPrepare(t.tid);
            t.phase = LibraryCheck.PENDING;
        }
        if (t != null && LibraryCheck.BOOTING.equals(t.phase)) {
            p.endGame();
            t.phase = LibraryCheck.READY;
        }
        s.run = null;
        LibraryCheck.clearCurrent(files());
        s.state = LibraryCheck.STOPPED;
        Log.i(TAG, "check: stopped at " + s.graded() + " of " + s.titles.size());
        save();
    }

    /**
     * The check was started again in a new process (the app crashed, the device rebooted): a boot it was in the
     * middle of is graded as it stands, and the check goes on.
     */
    synchronized void recover() {
        LibraryCheck.Title t = s.current();
        if (t != null && LibraryCheck.BOOTING.equals(t.phase) && s.run != null) {
            Log.i(TAG, "check: " + t.tid + "'s boot was cut by a restart of the check: graded as it stands");
            if (p.gamePid() != 0)
                p.endGame();
            finishBoot(t, "lost", false);
        }
    }

    // ---- The step ---------------------------------------------------------------------------------------------------

    /**
     * One step. Paused, nothing new starts, but a Prepare or a boot under way goes on to its end (a pause during a
     * boot waits for the title's grade).
     */
    synchronized void tick() {
        if (LibraryCheck.PAUSED.equals(s.state))
            autoResume();
        if (!s.active())
            return;
        boolean paused = LibraryCheck.PAUSED.equals(s.state);
        LibraryCheck.Title t = s.current();
        if (t == null) {
            if (paused)
                return;
            s.state = LibraryCheck.DONE;
            LibraryCheck.clearCurrent(files());
            Log.i(TAG, "check: done, " + s.titles.size() + " titles " + s.counts());
            save();
            return;
        }
        switch (t.phase) {
        case LibraryCheck.PENDING:
            if (!paused)
                pending(t);
            break;
        case LibraryCheck.PREPARING:
            preparing(t);
            break;
        case LibraryCheck.READY:
            if (!paused)
                ready(t);
            break;
        case LibraryCheck.BOOTING:
            booting(t);
            break;
        default:   // GRADED
            next();
            break;
        }
    }

    private void autoResume() {
        switch (s.reason) {
        case LibraryCheck.HOT:
            if (DeviceGuard.cooled(p.thermalStatus()))
                resume();
            break;
        case LibraryCheck.BATTERY:
            if (DeviceGuard.batteryBack(p.batteryPercent(), p.charging()))
                resume();
            break;
        case LibraryCheck.AWAY:
            if (p.canStart())
                resume();
            break;
        default:   // the player's pause waits for the player
            break;
        }
    }

    /** True (and paused) when the device should not start anything now. */
    private boolean held() {
        if (DeviceGuard.hot(p.thermalStatus())) {
            pauseFor(LibraryCheck.HOT);
            return true;
        }
        if (DeviceGuard.batteryLow(p.batteryPercent(), p.charging())) {
            pauseFor(LibraryCheck.BATTERY);
            return true;
        }
        return false;
    }

    private void pending(LibraryCheck.Title t) {
        if (held())
            return;
        if (!p.needsPrepare(t)) {
            t.phase = LibraryCheck.READY;
            save();
            return;
        }
        if (!p.canStart()) {
            pauseFor(LibraryCheck.AWAY);
            return;
        }
        try {
            p.startPrepare(t);
        } catch (Exception e) {
            Log.w(TAG, "check: cannot start " + t.tid + "'s Prepare: " + e);
            pauseFor(LibraryCheck.AWAY);
            return;
        }
        t.prepareAttempts++;
        t.prepareStartedAt = p.now();
        t.phase = LibraryCheck.PREPARING;
        Log.i(TAG, "check: preparing " + t.tid + " (attempt " + t.prepareAttempts + ")");
        save();
    }

    private void addPrepareTime(LibraryCheck.Title t) {
        if (t.prepareStartedAt > 0)
            t.prepareMs = Math.max(0, t.prepareMs) + Math.max(0, p.now() - t.prepareStartedAt);
        t.prepareStartedAt = 0;
    }

    private void preparing(LibraryCheck.Title t) {
        PrepareProtocol.Status st = p.prepareStatus(t.tid);
        boolean live = st != null && (st.running() || st.queued()) && p.prepareAlive();
        if (live) {
            // XBA-128: hot, the Prepare drops its own compile jobs (PrepareMonitor, DeviceGuard.jobsFor) and goes on
            // slowly; the check pauses, so nothing new starts after it until the device cooled down.
            if (DeviceGuard.hot(p.thermalStatus()) && !LibraryCheck.PAUSED.equals(s.state)) {
                Log.i(TAG, "check: severe thermal status, " + t.tid + "'s Prepare goes on with fewer jobs; paused");
                pauseFor(LibraryCheck.HOT);
            }
            return;
        }
        if (st != null && PrepareProtocol.DONE.equals(st.state) && !p.needsPrepare(t)) {
            addPrepareTime(t);
            t.phase = LibraryCheck.READY;
            Log.i(TAG, "check: " + t.tid + " prepared in " + t.prepareMs / 1000 + " s");
            save();
            return;
        }
        if (st != null && PrepareProtocol.FAILED.equals(st.state)) {
            addPrepareTime(t);
            Grade g = grade(t);
            g.prepareFailed = st.code + (st.msg.isEmpty() ? "" : ": " + st.msg);
            graded(t, g);
            return;
        }
        // Cancelled, interrupted (a « running » status without a process), or a « done » whose pack does not pass.
        if (st != null && (st.running() || st.queued()) && p.now() - t.prepareStartedAt < GONE_MS)
            return;   // the service is starting
        addPrepareTime(t);
        if (t.prepareAttempts >= MAX_PREPARE_ATTEMPTS) {
            Grade g = grade(t);
            g.prepareFailed = (st == null ? "NO_STATUS" : st.code.isEmpty() ? st.state.toUpperCase(java.util.Locale.ROOT)
                                                                              : st.code)
                              + ": interrupted " + t.prepareAttempts + " times";
            graded(t, g);
            return;
        }
        Log.i(TAG, "check: " + t.tid + "'s Prepare ended " + (st == null ? "without a status" : st.state)
                   + ", starting it again");
        t.phase = LibraryCheck.PENDING;
        save();
    }

    private void ready(LibraryCheck.Title t) {
        if (!t.bootable()) {
            Grade g = grade(t);
            g.notBooted = "the game's files are not in the app (import it to boot it)";
            graded(t, g);
            return;
        }
        if (held())
            return;
        if (!p.canStart()) {
            pauseFor(LibraryCheck.AWAY);
            return;
        }
        if (p.gamePid() != 0)
            p.endGame();
        File run = LibraryCheck.runDir(files(), t.tid);
        GameStore.deleteTree(run);
        //noinspection ResultOfMethodCallIgnored
        run.mkdirs();
        try {
            touch(new File(run, "pad-live.txt"));
            LibraryCheck.writeCurrent(files(), t.tid, run, p.now() + (s.seconds * 1000L) + 120_000);
            p.launchGame(t);
        } catch (Exception e) {
            Log.w(TAG, "check: cannot start " + t.tid + ": " + e);
            LibraryCheck.clearCurrent(files());
            pauseFor(LibraryCheck.AWAY);
            return;
        }
        s.run = new LibraryCheck.Run();
        s.run.launchedAt = p.now();
        t.bootAttempts++;
        t.phase = LibraryCheck.BOOTING;
        lastPid = 0;
        Log.i(TAG, "check: booting " + t.tid + " for " + s.seconds + " s (attempt " + t.bootAttempts + ")");
        save();
    }

    private void booting(LibraryCheck.Title t) {
        LibraryCheck.Run r = s.run;
        if (r == null) {   // check.json from a version without it: start the boot again
            t.phase = LibraryCheck.READY;
            save();
            return;
        }
        long now = p.now();
        long el = now - r.launchedAt;
        File run = LibraryCheck.runDir(files(), t.tid);
        int pid = p.gamePid();
        if (pid != 0) {
            if (pid != r.pid) {
                r.pid = pid;
                lastPid = pid;
            }
            r.sawGame = true;
            r.goneSince = 0;
            if (now - r.lastSampleAt >= SAMPLE_MS) {
                r.lastSampleAt = now;
                r.peakKb = Math.max(r.peakKb, p.peakKb(pid));
            }
        }
        pad(r, run, now);
        if (el >= s.seconds * 1000L) {
            Log.i(TAG, "check: " + t.tid + "'s " + s.seconds + " s are up");
            p.bringCheckToFront();
            if (pid != 0)
                r.peakKb = Math.max(r.peakKb, p.peakKb(pid));
            p.endGame();
            finishBoot(t, "none", false);
            return;
        }
        if (pid == 0) {
            if (!r.sawGame) {
                if (el >= NO_START_MS) {
                    Exit x = p.exitSince(r.launchedAt);
                    finishBoot(t, x != null ? x.arg : "nostart", true);
                }
                return;
            }
            if (r.goneSince == 0)
                r.goneSince = now;
            if (now - r.goneSince >= GONE_MS) {
                Exit x = p.exitSince(r.launchedAt);
                Log.i(TAG, "check: " + t.tid + "'s process ended by itself" + (x == null ? "" : " (" + x.arg + ")"));
                finishBoot(t, x != null ? x.arg : "exit:0", true);
            }
            return;
        }
        if (el < START_GRACE_MS)
            return;
        if (!p.gameWindow()) {   // the shell finished its window (no game data, a pack it refused): an end
            if (r.noWindowSince == 0)
                r.noWindowSince = now;
            if (now - r.noWindowSince >= GONE_MS) {
                Log.i(TAG, "check: " + t.tid + "'s window closed");
                p.endGame();
                finishBoot(t, "window", true);
            }
            return;
        }
        r.noWindowSince = 0;
        if (p.gameInFront()) {
            r.awaySince = 0;
            return;
        }
        if (r.awaySince == 0)
            r.awaySince = now;
        if (now - r.awaySince < AWAY_MS)
            return;
        // The player left (Home, the screen off): the runtime holds a game in the background; this run means nothing.
        Log.i(TAG, "check: " + t.tid + " left the screen, its boot is done again later");
        p.endGame();
        LibraryCheck.clearCurrent(files());
        if (t.bootAttempts >= MAX_BOOT_ATTEMPTS) {
            finishBoot(t, "away", false);
            return;
        }
        s.run = null;
        t.phase = LibraryCheck.READY;
        pauseFor(LibraryCheck.AWAY);
    }

    /** The pad script, on the status clock (the runtime's status starts at its first init, after the launch). */
    private void pad(LibraryCheck.Run r, File run, long now) {
        if (r.statusOrigin == 0) {
            long ms = lastStatusMs(new File(run, "status.jsonl"));
            if (ms < 0)
                return;
            r.statusOrigin = now - ms;
        }
        long sms = now - r.statusOrigin;
        int n = r.presses.size();
        if (sms < LibraryCheck.PRESS_FIRST_MS + n * LibraryCheck.PRESS_EVERY_MS)
            return;
        try (OutputStream o = new FileOutputStream(new File(run, "pad-live.txt"), true)) {
            o.write(((n < LibraryCheck.PRESS_STARTS ? "start" : "a") + "\n").getBytes(StandardCharsets.US_ASCII));
            r.presses.add(sms);
        } catch (IOException e) {
            Log.w(TAG, "check: pad file: " + e);
        }
    }

    /** The ms of the status file's last complete line, -1 when it has none yet. */
    static long lastStatusMs(File f) {
        long len = f.length();
        if (len <= 0)
            return -1;
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            int n = (int)Math.min(len, 4096);
            byte[] b = new byte[n];
            r.seek(len - n);
            r.readFully(b);
            String text = new String(b, StandardCharsets.UTF_8);
            int end = text.lastIndexOf('\n');
            if (end < 0)
                return -1;
            int start = text.lastIndexOf('\n', end - 1) + 1;
            return new JSONObject(text.substring(start, end)).optLong("ms", -1);
        } catch (IOException | org.json.JSONException e) {
            return -1;
        }
    }

    private void finishBoot(LibraryCheck.Title t, String exit, boolean byItself) {
        LibraryCheck.Run r = s.run;
        File run = LibraryCheck.runDir(files(), t.tid);
        LibraryCheck.clearCurrent(files());
        File log = new File(run, "run.log");
        int pid = r != null && r.pid != 0 ? r.pid : lastPid;
        if (pid != 0)
            p.dumpLog(pid, log);
        Grade g = grade(t);
        g.status = new File(run, "status.jsonl");
        g.log = log;
        g.exit = exit;
        if (r != null) {
            StringBuilder b = new StringBuilder();
            for (long x : r.presses)
                b.append(b.length() == 0 ? "" : ",").append(x);
            g.presses = b.toString();
            g.runS = Math.max(0, p.now() - r.launchedAt) / 1000.0;
            g.peakKb = r.peakKb;
        }
        graded(t, g);
    }

    private Grade grade(LibraryCheck.Title t) {
        Grade g = new Grade();
        g.title = t;
        g.work = LibraryCheck.workDir(files(), t.tid);
        g.sustain = s.sustain;
        g.prepareMs = t.prepareMs;
        return g;
    }

    private void graded(LibraryCheck.Title t, Grade g) {
        JSONObject line;
        try {
            line = p.grade(g);
        } catch (Exception e) {
            Log.w(TAG, "check: grading " + t.tid + " failed", e);
            line = null;
        }
        t.stage = line == null ? "" : line.optString("stage", "");
        t.blocker = line == null ? "grading failed" : line.isNull("blocker") ? "" : line.optString("blocker", "");
        t.phase = LibraryCheck.GRADED;
        s.run = null;
        Log.i(TAG, "check: " + t.tid + ": " + t.stage + (t.blocker.isEmpty() ? "" : " (" + t.blocker + ")"));
        next();
    }

    private void next() {
        s.index++;
        save();
    }

    private void pauseFor(String why) {
        if (!LibraryCheck.PAUSED.equals(s.state) || !why.equals(s.reason))
            Log.i(TAG, "check: paused (" + why + ")");
        s.state = LibraryCheck.PAUSED;
        s.reason = why;
        save();
    }

    private void save() {
        s.updatedAt = p.now();
        try {
            LibraryCheck.write(files(), s);
        } catch (IOException e) {
            Log.w(TAG, "check: cannot write check.json: " + e);
        }
        p.changed(s);
    }

    private static void touch(File f) throws IOException {
        if (!f.isFile())
            new FileOutputStream(f).close();
    }

    /** The titles not graded yet (the screen's count). */
    static int remaining(List<LibraryCheck.Title> l) {
        int n = 0;
        for (LibraryCheck.Title t : l)
            n += LibraryCheck.GRADED.equals(t.phase) ? 0 : 1;
        return n;
    }
}
