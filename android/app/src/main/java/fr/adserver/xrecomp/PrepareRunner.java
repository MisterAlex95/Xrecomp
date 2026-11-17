package fr.adserver.xrecomp;

import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * Runs one Prepare (docs/android/prepare.md) and reports it: spawns `python -m tools.prepare --android ...` (from a
 * CommandFactory, so the tests substitute a script), copies its whole output to prepare.log, turns its XBA_PROGRESS /
 * XBA_RESULT lines into status.json (atomically) and into the listener (the notification), and decides the outcome.
 *
 * - The child runs in its own process group (setsid), so a cancel reaches the compilers it started: SIGTERM to the
 *   group, then SIGKILL after killGraceMs.
 * - A child killed by a signal it did not ask for (Android's phantom-process killer, the low-memory killer) is run
 *   once more with half the jobs; a second kill fails with code KILLED. tools.prepare's stamps make the second run
 *   start where the first stopped.
 * - Plain Java apart from android.util.Log: PrepareService owns the Android side.
 */
final class PrepareRunner {
    static final String TAG = "xboxrecomp";

    /** Builds the command for a number of jobs; its environment and working directory. */
    interface CommandFactory {
        List<String> command(int jobs);

        Map<String, String> environment();

        File workingDir();
    }

    interface Listener {
        void onStatus(PrepareProtocol.Status s);
    }

    /** Checks the outcome of a run that said ok: null when the pack is usable, else an error code. */
    interface Validator {
        String validate();
    }

    /** Error codes the app itself sets (tools.prepare sets its own in XBA_RESULT). */
    static final String KILLED = "KILLED", CANCELLED_CODE = "CANCELLED", SPAWN_FAILED = "SPAWN_FAILED",
            NO_RESULT = "NO_RESULT", PACK_INVALID = "PACK_INVALID", INSTALL_FAILED = "INSTALL_FAILED",
            NO_TOOLS = "NO_TOOLS", ASSET_HASH = "ASSET_HASH";

    private final File statusFile, logFile;
    private final CommandFactory factory;
    private final Listener listener;
    private final Validator validator;
    private final int jobs;
    private final PrepareProtocol.Status status = new PrepareProtocol.Status();

    long killGraceMs = 5000;
    /** tools.prepare's own log (<work>/prepare.log): its tail is appended to ours at the end, for « Share log ». */
    volatile File toolLog;
    static final int TOOL_LOG_TAIL = 2 << 20;
    /** Minimum time between two status.json writes for progress inside one step (step changes always write). */
    long statusIntervalMs = 250;

    private volatile boolean cancelRequested;
    private volatile Process process;
    private volatile int pgid;
    /** The group leader's start time (startTime) when pgid was read, or -1. */
    private volatile long pgidStart = -1;
    private Thread killer;
    private long lastWrite;
    private PrintStream log;

    PrepareRunner(File statusDir, String titleId, int jobs, CommandFactory factory, Validator validator,
                  Listener listener) {
        this.statusFile = new File(statusDir, "status.json");
        this.logFile = new File(statusDir, "prepare.log");
        this.factory = factory;
        this.validator = validator;
        this.listener = listener;
        this.jobs = Math.max(1, jobs);
        status.titleId = titleId == null ? "" : titleId;
        status.log = logFile.getAbsolutePath();
    }

    File statusFile() {
        return statusFile;
    }

    /** A background Prepare (XBA-129): said in status.json, which the launcher reads (before begin()). */
    void setBackground(boolean b) {
        status.background = b;
    }

    /** The clang -O level (XBA-128), in status.json and for the command (before begin()). */
    void setOpt(int opt) {
        status.opt = opt;
    }

    int opt() {
        return status.opt;
    }

    /** A Prepare started again after its process died (XBA-128): prepare.log is kept and appended to. */
    void setResumes(int n) {
        status.resumes = n;
    }

    boolean background() {
        return status.background;
    }

    File logFile() {
        return logFile;
    }

    boolean cancelRequested() {
        return cancelRequested;
    }

    /** Opens prepare.log (truncated) and writes the first status (running, the install phase). */
    void begin() throws IOException {
        File dir = statusFile.getParentFile();
        if (dir != null && !(dir.mkdirs() || dir.isDirectory()))
            throw new IOException("cannot create " + dir);
        killOrphans();
        log = new PrintStream(new FileOutputStream(logFile, status.resumes > 0), true, "UTF-8");
        if (status.resumes > 0)
            logLine("=== resumed after the Prepare's process died (" + status.resumes + "): the stamps skip what was done");
        status.state = PrepareProtocol.RUNNING;
        status.startedAt = System.currentTimeMillis();
        status.step = 0;
        status.id = PrepareProtocol.STEP_INSTALL;
        status.jobs = jobs;
        logLine("xboxrecomp prepare, title " + status.titleId + ", started "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date(status.startedAt)));
        publish(true);
    }

    /** The install phase's progress (Toolchain.install), as step 0. */
    void installProgress(int pct, String msg) {
        status.step = 0;
        status.id = PrepareProtocol.STEP_INSTALL;
        status.pct = pct;
        status.eta = -1;
        status.msg = msg == null ? "" : msg;
        publish(false);
    }

    void logLine(String s) {
        if (log != null)
            log.println(s);
    }

    /** The process group of the running tools.prepare, kept beside status.json while it runs (XBA-128). */
    File pgidFile() {
        return new File(statusFile.getParentFile(), "pgid");
    }

    /**
     * A Prepare whose process died (killed by the system: no onDestroy, no cancel) leaves tools.prepare and its
     * compilers in their own process group, which setsid took out of the app's: they would compile beside the new run.
     * The group's id was written when it started; it is ended before anything else runs.
     *
     * The id alone is not enough: the file outlives the group (the app died after the run ended, a reboot), and the
     * number may by now be another process's group, one of ours (a game). The file also holds the leader's start
     * time and the boot's id, and the group is ended only when it is still that one (groupIsThatOne). A file without
     * them (an older build) is not trusted.
     */
    void killOrphans() {
        File f = pgidFile();
        if (!f.isFile())
            return;
        int id = 0;
        long start = -1;
        String boot = "";
        try {
            String[] w = PrepareProtocol.readText(f, 256).trim().split("\\s+");
            id = Integer.parseInt(w[0]);
            if (w.length >= 2)
                start = Long.parseLong(w[1]);
            if (w.length >= 3)
                boot = w[2];
        } catch (IOException | NumberFormatException ignored) {
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
        if (id <= 1)
            return;
        if (start < 0 || !boot.equals(bootId()) || !groupIsThatOne(id, start)) {
            Log.i(TAG, "prepare: process group " + id + " left by an earlier Prepare is gone: nothing to end");
            return;
        }
        Log.w(TAG, "prepare: process group " + id + " left by a Prepare whose process died: ended");
        try {
            android.system.Os.kill(-id, 9);
        } catch (Throwable ignored) {   // gone already, or no libcore in a JVM test
        }
        runKill("-KILL", "--", "-" + id);
    }

    /** Ends without running the command (the install failed or was cancelled). */
    PrepareProtocol.Status finish(String state, String code, String msg) {
        status.state = state;
        status.code = code == null ? "" : code;
        status.msg = msg == null ? "" : msg;
        status.eta = -1;
        logLine("=== " + state + (status.code.isEmpty() ? "" : " " + status.code) + ": " + status.msg);
        //noinspection ResultOfMethodCallIgnored
        pgidFile().delete();
        appendToolLog();
        publish(true);
        if (log != null)
            log.close();
        return status.copy();
    }

    private void appendToolLog() {
        File f = toolLog;
        if (log == null || f == null || !f.isFile())
            return;
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            long len = r.length(), from = Math.max(0, len - TOOL_LOG_TAIL);
            log.println("=== " + f + (from > 0 ? " (last " + TOOL_LOG_TAIL + " of " + len + " bytes)" : ""));
            r.seek(from);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = r.read(buf)) > 0)
                log.write(buf, 0, n);
            log.println();
        } catch (IOException e) {
            log.println("=== cannot read " + f + ": " + e);
        }
    }

    /** Runs the command (and the retry after a kill); blocking. begin() first. Returns the final status. */
    PrepareProtocol.Status run() {
        int attemptJobs = jobs;
        for (int attempt = 1; ; attempt++) {
            status.attempt = attempt;
            status.jobs = attemptJobs;
            Outcome o = runOnce(attemptJobs, attempt);
            if (cancelRequested || o.kind == Outcome.CANCELLED)
                return finish(PrepareProtocol.CANCELLED, CANCELLED_CODE, "");
            if (o.kind == Outcome.OK) {
                String bad = validator == null ? null : validator.validate();
                if (bad != null)
                    return finish(PrepareProtocol.FAILED, bad, "the pack made is not usable (" + bad + ")");
                status.step = status.n;
                status.pct = 100;
                status.id = PrepareProtocol.STEPS[PrepareProtocol.STEPS.length - 1];
                return finish(PrepareProtocol.DONE, "", o.msg);
            }
            if (o.kind == Outcome.KILLED_KIND) {
                if (attempt == 1) {
                    int next = Math.max(1, attemptJobs / 2);
                    logLine("=== killed by signal " + o.signal + ": once more with " + next + " jobs");
                    Log.w(TAG, "prepare: killed by signal " + o.signal + ", retrying with " + next + " jobs");
                    attemptJobs = next;
                    continue;
                }
                return finish(PrepareProtocol.FAILED, KILLED, "killed by the system (signal " + o.signal
                                                              + ") twice; " + o.msg);
            }
            return finish(PrepareProtocol.FAILED, o.code, o.msg);
        }
    }

    /** Asks the run to stop: SIGTERM to the child's group now, SIGKILL after killGraceMs (from the run thread). */
    void cancel() {
        cancelRequested = true;
        Process p = process;
        if (p != null)
            startKiller(p);
    }

    // ---- One attempt --------------------------------------------------------------------------------------------

    private static final class Outcome {
        static final int OK = 0, ERROR = 1, CANCELLED = 2, KILLED_KIND = 3;
        int kind = ERROR, signal;
        String code = "", msg = "";
    }

    private Outcome runOnce(int attemptJobs, int attempt) {
        Outcome out = new Outcome();
        List<String> cmd = factory.command(attemptJobs);
        List<String> full = wrap(cmd);
        logLine("=== attempt " + attempt + ", " + attemptJobs + " jobs: " + String.join(" ", cmd));
        ProcessBuilder pb = new ProcessBuilder(full).redirectErrorStream(true);
        File cwd = factory.workingDir();
        if (cwd != null)
            pb.directory(cwd);
        Map<String, String> env = factory.environment();
        if (env != null)
            pb.environment().putAll(env);
        Process p;
        try {
            p = pb.start();
        } catch (IOException e) {
            out.code = SPAWN_FAILED;
            out.msg = String.valueOf(e.getMessage());
            logLine("=== cannot start: " + e);
            return out;
        }
        pgid = 0;
        pgidStart = -1;
        synchronized (this) {
            killer = null;
        }
        process = p;
        if (cancelRequested)
            startKiller(p);
        PrepareProtocol.Result result = null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(),
                                                                         StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (pgid == 0 && line.startsWith("XBA_PGID ")) {
                    try {
                        int id = Integer.parseInt(line.substring(9).trim());
                        pgidStart = startTime(id);
                        pgid = id;
                        try (FileOutputStream o = new FileOutputStream(pgidFile())) {   // for killOrphans
                            o.write((id + " " + pgidStart + " " + bootId()).trim()
                                    .getBytes(StandardCharsets.US_ASCII));
                        } catch (IOException e) {
                            Log.w(TAG, "prepare: cannot write " + pgidFile() + ": " + e);
                        }
                    } catch (NumberFormatException ignored) {
                    }
                    if (cancelRequested)
                        signal(Signal.TERM);
                    continue;
                }
                logLine(line);
                PrepareProtocol.Progress pr = PrepareProtocol.parseProgress(line);
                if (pr != null) {
                    onProgress(pr);
                    continue;
                }
                PrepareProtocol.Result res = PrepareProtocol.parseResult(line);
                if (res != null)
                    result = res;
            }
        } catch (IOException e) {
            logLine("=== output: " + e);
        }
        int code;
        try {
            code = p.waitFor();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            signal(Signal.KILL);
            code = -1;
        }
        process = null;
        synchronized (this) {
            if (killer != null)
                killer.interrupt();
        }
        logLine("=== exit " + code);

        if (cancelRequested || code == 130) {
            out.kind = Outcome.CANCELLED;
            return out;
        }
        if (result != null && result.ok && code == 0) {
            out.kind = Outcome.OK;
            out.msg = result.pack;
            return out;
        }
        int sig = signalOf(code);
        if (sig != 0 && (result == null || !result.ok)) {
            out.kind = Outcome.KILLED_KIND;
            out.signal = sig;
            out.msg = status.msg;
            return out;
        }
        if (result != null && !result.ok) {
            if (CANCELLED_CODE.equals(result.code)) {   // tools.prepare got SIGTERM / SIGINT from someone else
                out.kind = Outcome.CANCELLED;
                return out;
            }
            if (KILLED.equals(result.code)) {   // tools.prepare saw one of its compilers killed
                out.kind = Outcome.KILLED_KIND;
                out.signal = 9;
                out.msg = result.msg;
                return out;
            }
            out.code = result.code;
            out.msg = result.msg;
            return out;
        }
        if (code == 0) {   // exit 0 without a result line: trust the pack check
            out.kind = Outcome.OK;
            return out;
        }
        out.code = NO_RESULT;
        out.msg = "tools.prepare exited with " + code + " without a result; see the log";
        return out;
    }

    /**
     * The signal that ended a child, from its exit value: OpenJDK reports 128 + signal (and so do shells), Android's
     * older libcore the bare signal. SIGKILL / SIGTERM / SIGSEGV / SIGABRT / SIGBUS only: an exit 1 is an error.
     */
    static int signalOf(int code) {
        int s = code > 128 ? code - 128 : code;
        switch (s) {
        case 9:
        case 15:
        case 11:
        case 6:
        case 7:
            return code > 128 || code == 9 || code == 15 ? s : 0;
        default:
            return 0;
        }
    }

    private void onProgress(PrepareProtocol.Progress pr) {
        boolean stepChanged = pr.step != status.step || !pr.id.equals(status.id);
        status.step = pr.step;
        status.n = pr.n;
        status.id = pr.id;
        status.pct = pr.pct;
        status.eta = pr.eta;
        status.msg = pr.msg;
        publish(stepChanged);
    }

    private void publish(boolean force) {
        long now = System.currentTimeMillis();
        status.updatedAt = now;
        if (force || now - lastWrite >= statusIntervalMs) {
            lastWrite = now;
            try {
                PrepareProtocol.writeStatus(statusFile, status);
            } catch (IOException e) {
                Log.w(TAG, "prepare: cannot write " + statusFile + ": " + e);
            }
        }
        if (listener != null)
            listener.onStatus(status.copy());
    }

    // ---- Process group ------------------------------------------------------------------------------------------

    static String findBinary(String... candidates) {
        for (String c : candidates) {
            if (new File(c).canExecute())
                return c;
        }
        return null;
    }

    /**
     * sh -c 'echo XBA_PGID $$; exec "$0" "$@"' cmd..., under setsid when there is one: the shell prints its pid,
     * which setsid made the group's id, then becomes the command (same pid).
     */
    static List<String> wrap(List<String> cmd) {
        List<String> out = new ArrayList<>();
        String sh = findBinary("/system/bin/sh", "/bin/sh");
        if (sh == null)
            return new ArrayList<>(cmd);
        String setsid = findBinary("/system/bin/setsid", "/usr/bin/setsid", "/bin/setsid");
        if (setsid != null)
            out.add(setsid);
        out.add(sh);
        out.add("-c");
        out.add("echo XBA_PGID $$; exec \"$0\" \"$@\"");
        out.addAll(cmd);
        return out;
    }

    private enum Signal { TERM, KILL }

    private void signal(Signal s) {
        int id = pgid;
        long start = pgidStart;
        Process p = process;
        // A signal that comes after the group has ended (a cancel racing the exit) must not reach a group that has
        // since taken its number; a start time unknown (no /proc) keeps the plain group signal of a live run.
        if (id > 0 && (start < 0 || groupIsThatOne(id, start))) {
            // The group: kill(2) with a negative pid (android.system.Os), and the kill command as well (toybox on
            // a device, util-linux / procps on Linux): Os is a stub in JVM tests, and a second signal to a group
            // that is gone is harmless.
            boolean sent = false;
            try {
                android.system.Os.kill(-id, s == Signal.TERM ? 15 : 9);   // SIGTERM, SIGKILL (Linux)
                sent = true;
            } catch (Throwable ignored) {   // ErrnoException (group gone), or no libcore in a JVM test
            }
            String sig = s == Signal.TERM ? "TERM" : "KILL";
            if (runKill("-" + sig, "--", "-" + id) || runKill("-" + sig, "-" + id) || sent)
                return;
        }
        if (p != null) {
            if (s == Signal.TERM)
                p.destroy();
            else
                p.destroyForcibly();
        }
    }

    /** /proc, a field for the tests. */
    static File proc = new File("/proc");

    /** A process's /proc/<pid>/stat fields after its name: [0] state, [1] ppid, [2] pgrp, [19] start time. */
    private static String[] statFields(int pid) {
        try {
            String st = PrepareProtocol.readText(new File(proc, pid + "/stat"), 4096);
            int close = st.lastIndexOf(')');
            if (close < 0)
                return null;
            String[] f = st.substring(close + 1).trim().split("\\s+");
            return f.length >= 20 ? f : null;
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /** A process's start time (clock ticks after boot), or -1 when it is not there or /proc cannot be read. */
    static long startTime(int pid) {
        String[] f = statFields(pid);
        try {
            return f == null ? -1 : Long.parseLong(f[19]);
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    /** This boot's id (changes at every boot), or "" when it cannot be read. */
    static String bootId() {
        try {
            return PrepareProtocol.readText(new File(proc, "sys/kernel/random/boot_id"), 256).trim();
        } catch (IOException | RuntimeException e) {
            return "";
        }
    }

    /**
     * Whether process group id is still the one whose leader started at start: the leader is there with that start
     * time, or it has exited and a live process is still in group id (Linux gives no new process a pid number that a
     * group still has as its id, so those are the leader's own descendants). False when the leader's number is
     * another process's now, or when nothing is left in the group.
     */
    static boolean groupIsThatOne(int id, long start) {
        if (id <= 1 || start < 0)
            return false;
        if (new File(proc, String.valueOf(id)).exists())
            return start == startTime(id);
        File[] all = proc.listFiles();
        if (all == null)
            return false;
        String want = String.valueOf(id);
        for (File d : all) {
            String n = d.getName();
            if (n.isEmpty() || !Character.isDigit(n.charAt(0)))
                continue;
            String[] f;
            try {
                f = statFields(Integer.parseInt(n));
            } catch (NumberFormatException e) {
                continue;
            }
            if (f != null && want.equals(f[2]) && !"Z".equals(f[0]))
                return true;
        }
        return false;
    }

    private static boolean runKill(String... args) {
        String kill = findBinary("/system/bin/kill", "/bin/kill", "/usr/bin/kill");
        if (kill == null)
            return false;
        List<String> c = new ArrayList<>();
        c.add(kill);
        java.util.Collections.addAll(c, args);
        try {
            Process k = new ProcessBuilder(c).redirectErrorStream(true).start();
            try (OutputStream ignored = k.getOutputStream()) {
                // nothing to send
            }
            k.getInputStream().close();
            if (!k.waitFor(3, TimeUnit.SECONDS)) {
                k.destroyForcibly();
                return false;
            }
            return k.exitValue() == 0;
        } catch (IOException e) {
            return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    /** After a cancel: SIGTERM now, SIGKILL to the group if the child is still there after killGraceMs. */
    private synchronized void startKiller(Process p) {
        if (killer != null || process != p)
            return;
        Thread t = new Thread(() -> {
            signal(Signal.TERM);
            try {
                if (!p.waitFor(killGraceMs, TimeUnit.MILLISECONDS)) {
                    logLine("=== still running " + killGraceMs + " ms after SIGTERM: SIGKILL");
                    signal(Signal.KILL);
                }
            } catch (InterruptedException ignored) {
            }
        }, "xba-prepare-kill");
        t.setDaemon(true);
        killer = t;
        t.start();
    }
}
