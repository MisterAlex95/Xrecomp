package fr.adserver.xrecomp;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The Prepare's wall-clock time per stage (XBA-341), sent as one performance transaction « prepare » when it ends,
 * only while « Send reports automatically » is on (Telemetry's gate; off: nothing leaves).
 *
 * The stages are the ones the progress protocol already names (PrepareProtocol): « install » (the app's own phase,
 * the tools' assets), « launch » (from starting tools.prepare to its first progress line: Python's start-up), then
 * each XBA_PROGRESS id as it changes: identify, analyse, lift, compile, pack. An id seen again (attempt 2 after a
 * kill starts at identify) opens a new span with the same op. A span is « prepare.&lt;id&gt; ».
 *
 * What the transaction carries: the stages, the result (ok or the error code, never the message: it holds the title
 * and paths), the console and library key (as the crash reports), the device model, ABI, Android level, cores, big
 * cores, and the Prepare's kind (opt level, jobs, attempt, resumes, background). No title, path or file name.
 */
final class PrepareTimings {
    static final String TAG = "xboxrecomp";
    static final String STAGE_INSTALL = PrepareProtocol.STEP_INSTALL, STAGE_LAUNCH = "launch";

    /** The clock, a field for the tests. */
    interface Clock {
        long now();
    }

    private final Clock clock;
    private final List<CrashReport.Span> spans = new ArrayList<>();
    private String open;
    private long openAt, startedAt, endedAt;

    PrepareTimings() {
        this(System::currentTimeMillis);
    }

    PrepareTimings(Clock clock) {
        this.clock = clock;
    }

    /** The Prepare starts: the install stage opens. */
    synchronized void begin() {
        spans.clear();
        startedAt = clock.now();
        endedAt = 0;
        open = STAGE_INSTALL;
        openAt = startedAt;
    }

    /** A stage opens (and the open one closes); the same stage again changes nothing. */
    synchronized void stage(String id) {
        String op = clean(id);
        if (op.isEmpty() || op.equals(open) || startedAt == 0 || endedAt != 0)
            return;
        long now = clock.now();
        close(now);
        open = op;
        openAt = now;
    }

    /** One line of tools.prepare's output: an XBA_PROGRESS line moves the stage. */
    void line(String line) {
        PrepareProtocol.Progress p = PrepareProtocol.parseProgress(line);
        if (p != null)
            stage(p.id);
    }

    /** The Prepare ends: the open stage closes. */
    synchronized void end() {
        if (startedAt == 0 || endedAt != 0)
            return;
        endedAt = Math.max(startedAt, clock.now());
        close(endedAt);
        open = null;
    }

    private void close(long now) {
        if (open == null)
            return;
        spans.add(new CrashReport.Span("prepare." + open, openAt, Math.max(openAt, now)));
    }

    synchronized List<CrashReport.Span> spans() {
        return new ArrayList<>(spans);
    }

    synchronized long startedAt() {
        return startedAt;
    }

    synchronized long endedAt() {
        return endedAt;
    }

    /** A stage id as an op: lower case letters, digits and « _ » only, 32 at most. */
    static String clean(String id) {
        if (id == null)
            return "";
        StringBuilder b = new StringBuilder();
        for (char ch : id.toLowerCase(Locale.ROOT).toCharArray()) {
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')
                b.append(ch);
            if (b.length() >= 32)
                break;
        }
        return b.toString();
    }

    /** The transaction status of an ended Prepare (PrepareProtocol states). */
    static String txStatus(String state) {
        if (PrepareProtocol.DONE.equals(state))
            return "ok";
        if (PrepareProtocol.CANCELLED.equals(state))
            return "cancelled";
        return "internal_error";
    }

    // ---- The transaction ----------------------------------------------------------------------------------------

    /** /sys/devices/system/cpu, a field for the tests. */
    static File cpuRoot = new File("/sys/devices/system/cpu");

    /**
     * The cores above the slowest cluster (their cpu_capacity, else cpuinfo_max_freq, higher than the lowest one's);
     * all of them when every core is the same; -1 when it cannot be read.
     */
    static int bigCores() {
        File[] all = cpuRoot.listFiles();
        if (all == null)
            return -1;
        List<Long> v = new ArrayList<>();
        for (String what : new String[] { "cpu_capacity", "cpufreq/cpuinfo_max_freq" }) {
            v.clear();
            for (File d : all) {
                if (!d.getName().matches("cpu\\d+"))
                    continue;
                try {
                    v.add(Long.parseLong(PrepareProtocol.readText(new File(d, what), 64).trim()));
                } catch (IOException | NumberFormatException e) {
                    v.clear();
                    break;
                }
            }
            if (!v.isEmpty())
                break;
        }
        if (v.isEmpty())
            return -1;
        long min = Long.MAX_VALUE;
        for (long x : v)
            min = Math.min(min, x);
        int n = 0;
        for (long x : v)
            if (x > min)
                n++;
        return n == 0 ? v.size() : n;
    }

    /** The tags: see the class comment. */
    static Map<String, String> tags(PrepareProtocol.Status end, int cores, int bigCores) {
        Map<String, String> t = new LinkedHashMap<>();
        t.put("console", CrashReport.consoleOf(end.titleId));
        t.put("game_key", end.titleId);
        t.put("device", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL);
        t.put("abi", android.os.Build.SUPPORTED_ABIS.length > 0 ? android.os.Build.SUPPORTED_ABIS[0] : "");
        t.put("android", String.valueOf(android.os.Build.VERSION.SDK_INT));
        t.put("cores", String.valueOf(cores));
        if (bigCores > 0)
            t.put("big_cores", String.valueOf(bigCores));
        t.put("result", PrepareProtocol.DONE.equals(end.state) ? "ok"
                        : end.code == null || end.code.isEmpty() ? end.state : end.code);
        t.put("prepare_kind", end.background ? "background" : "foreground");
        if (end.opt > 0)
            t.put("opt", "O" + end.opt);
        t.put("jobs", String.valueOf(end.jobs));
        t.put("attempt", String.valueOf(end.attempt));
        t.put("resumes", String.valueOf(end.resumes));
        return t;
    }

    /** The envelope of an ended Prepare, or null when it never began. */
    static byte[] envelope(Context c, PrepareTimings t, PrepareProtocol.Status end, int cores, int bigCores) {
        long start = t.startedAt();
        if (start == 0)
            return null;
        long stop = t.endedAt() != 0 ? t.endedAt() : Math.max(start, System.currentTimeMillis());
        return CrashReport.transactionEnvelope("prepare", "prepare", txStatus(end.state), start, stop, t.spans(),
                                               tags(end, cores, bigCores), CrashReport.release(c),
                                               CrashReport.environment(c));
    }

    /** Builds and sends the transaction of an ended Prepare (blocking: the network). True when it was accepted. */
    static boolean send(Context c, PrepareTimings t, PrepareProtocol.Status end) {
        if (!Telemetry.allowed(c))
            return false;
        byte[] body = envelope(c, t, end, Runtime.getRuntime().availableProcessors(), bigCores());
        return body != null && CrashReport.sendAuto(c, body);
    }

    /** send() on a short daemon thread: the end notification and the queue's next Prepare do not wait for it. */
    static void sendInBackground(Context c, PrepareTimings t, PrepareProtocol.Status end) {
        Context app = c.getApplicationContext() != null ? c.getApplicationContext() : c;
        Thread th = new Thread(() -> {
            try {
                send(app, t, end);
            } catch (Throwable e) {   // a report must never break the Prepare
                Log.w(TAG, "prepare: timings not sent: " + e);
            }
        }, "xba-prepare-timings");
        th.setDaemon(true);
        th.start();
    }
}
