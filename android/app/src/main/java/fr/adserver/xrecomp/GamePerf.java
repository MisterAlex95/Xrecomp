package fr.adserver.xrecomp;

import android.content.Context;
import android.net.ConnectivityManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.regex.Pattern;

/**
 * A game run's performance as one Sentry transaction « game » (op game.run), docs/android/app-shell.md « Game
 * performance ». The game's frame loop keeps a small JSON summary (host_perf_line.h « The run summary »: numbers and
 * fixed words, built from the counters of the perf line) at XBA_PERF_SUMMARY, files/logs/game-perf.json. Two ways
 * out:
 * - A run that ended (AutoReports.collect, the launcher's resume): only with « Send reports automatically » on and
 *   Telemetry's gate open, only for the newest end, not for a crash or « not responding » (crash reports exist), and
 *   only when the summary's pid is the ended process's. Sent at once on a network the option allows; never queued
 *   (what cannot leave is dropped). The file is deleted either way (not another process's: a game running now).
 * - The Benchmark (the in-game menu): its own window, sent only when the player taps Send (CrashReport.sendOnce).
 *
 * What it carries: tags console, game_key (the library key, as the crash reports), device, android, abi, toolkit (the
 * pack's toolkit digest, 12 characters), end (exited / abnormal), benchmark (0 / 1), state (the Benchmark's state:
 * a hash, or « none »), and the summary's fixed words (gpu_thread, resolve_mode); measurements: the summary's
 * numbers. No title, path, file name or install id.
 */
final class GamePerf {
    static final String TAG = "xba";
    static final String NAME = "game", OP = "game.run";
    /** Words allowed in a tag from the summary, keys of a measurement. */
    private static final Pattern WORD = Pattern.compile("[a-z0-9_-]{1,23}");
    private static final Pattern KEY = Pattern.compile("[a-z][a-z0-9_]{0,31}");

    private GamePerf() {}

    /** The summary's file, in the package's files (the game's process writes it, the launcher's reads it). */
    static File file(Context c) {
        return new File(new File(c.getFilesDir(), "logs"), "game-perf.json");
    }

    /** The game's process starts: an older summary goes, XBA_PERF_SUMMARY tells the frame loop where to write. */
    static void beginRun(Context c) {
        File f = file(c);
        f.delete();
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return;
        try {
            Os.setenv("XBA_PERF_SUMMARY", f.getPath(), true);
        } catch (ErrnoException e) {
            Log.w(TAG, "perf: cannot set XBA_PERF_SUMMARY", e);
        }
    }

    // ---- The summary ----------------------------------------------------------------------------------------------

    /** The fields of a summary that leave (host_perf_line.h). */
    static final class Summary {
        int pid;
        String console = "";
        boolean bench, fin;
        double seconds, speedPct, guestFps, fullHz, shownFps;
        double fpsSpeedMedian, fpsSpeedP5, fpsShownMedian, fpsShownP5;
        int samples;
        double frameMean, frameP99, worst, busy, emu, render, idle;
        long hostFrames, over1;
        final Map<String, Double> stages = new LinkedHashMap<>(), nums = new LinkedHashMap<>();
        final Map<String, String> words = new LinkedHashMap<>();
    }

    /** A summary from its JSON, or null when it is not one (anything but numbers and known words is dropped). */
    static Summary parse(String json) {
        if (json == null)
            return null;
        try {
            JSONObject o = new JSONObject(json);
            if (o.optInt("v", 0) != 1)
                return null;
            Summary s = new Summary();
            s.pid = o.optInt("pid", 0);
            String con = o.optString("console", "");
            s.console = WORD.matcher(con).matches() ? con : "";
            s.bench = o.optInt("bench", 0) == 1;
            s.fin = o.optInt("final", 0) == 1;
            s.seconds = num(o, "seconds");
            s.speedPct = num(o, "speed_pct");
            s.guestFps = num(o, "guest_fps");
            s.fullHz = num(o, "full_hz");
            s.shownFps = num(o, "shown_fps");
            s.fpsSpeedMedian = num(o, "fps_speed_median");
            s.fpsSpeedP5 = num(o, "fps_speed_p5");
            s.fpsShownMedian = num(o, "fps_shown_median");
            s.fpsShownP5 = num(o, "fps_shown_p5");
            s.samples = o.optInt("samples", 0);
            s.frameMean = num(o, "frame_mean_ms");
            s.frameP99 = num(o, "frame_p99_ms");
            s.worst = num(o, "worst_ms");
            s.busy = num(o, "busy_ms");
            s.emu = num(o, "emu_ms");
            s.render = num(o, "render_ms");
            s.idle = num(o, "idle_ms");
            s.hostFrames = o.optLong("host_frames", 0);
            s.over1 = o.optLong("over_1", 0);
            numbers(o.optJSONObject("stages"), s.stages);
            numbers(o.optJSONObject("nums"), s.nums);
            JSONObject w = o.optJSONObject("words");
            if (w != null)
                for (Iterator<String> it = w.keys(); it.hasNext();) {
                    String k = it.next(), v = w.optString(k, "");
                    if (KEY.matcher(k).matches() && WORD.matcher(v).matches())
                        s.words.put(k, v);
                }
            return s;
        } catch (JSONException e) {
            return null;
        }
    }

    private static double num(JSONObject o, String k) {
        double v = o.optDouble(k, 0);
        return Double.isNaN(v) || Double.isInfinite(v) ? 0 : v;
    }

    private static void numbers(JSONObject o, Map<String, Double> out) {
        if (o == null)
            return;
        for (Iterator<String> it = o.keys(); it.hasNext();) {
            String k = it.next();
            double v = o.optDouble(k, Double.NaN);
            if (KEY.matcher(k).matches() && !Double.isNaN(v) && !Double.isInfinite(v))
                out.put(k, v);
        }
    }

    static Summary read(File f) {
        try {
            return f.isFile() ? parse(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8)) : null;
        } catch (IOException e) {
            return null;
        }
    }

    // ---- The transaction ----------------------------------------------------------------------------------------

    private static final String MS = "millisecond", NONE = "none", PCT = "percent", SEC = "second";

    /** The measurements: see the class comment and app-shell.md's table. */
    static Map<String, CrashReport.Measure> measures(Summary s) {
        Map<String, CrashReport.Measure> m = new LinkedHashMap<>();
        m.put("fps_shown_median", new CrashReport.Measure(s.fpsShownMedian, NONE));
        m.put("fps_shown_p5", new CrashReport.Measure(s.fpsShownP5, NONE));
        m.put("fps_speed_median", new CrashReport.Measure(s.fpsSpeedMedian, NONE));
        m.put("fps_speed_p5", new CrashReport.Measure(s.fpsSpeedP5, NONE));
        m.put("fps_shown", new CrashReport.Measure(s.shownFps, NONE));
        m.put("fps_speed", new CrashReport.Measure(s.guestFps, NONE));
        if (s.fullHz > 0)
            m.put("speed_pct", new CrashReport.Measure(s.speedPct, PCT));
        m.put("frame_time_mean", new CrashReport.Measure(s.frameMean, MS));
        m.put("frame_time_p99", new CrashReport.Measure(s.frameP99, MS));
        m.put("worst_frame", new CrashReport.Measure(s.worst, MS));
        if (s.hostFrames > 0)
            m.put("slow_frames_pct", new CrashReport.Measure(100.0 * s.over1 / s.hostFrames, PCT));
        m.put("guest_ms", new CrashReport.Measure(s.emu, MS));
        m.put("render_ms", new CrashReport.Measure(s.render, MS));
        Double present = s.stages.get("present");
        if (present != null)
            m.put("present_ms", new CrashReport.Measure(present, MS));
        m.put("idle_ms", new CrashReport.Measure(s.idle, MS));
        m.put("busy_ms", new CrashReport.Measure(s.busy, MS));
        m.put("run_seconds", new CrashReport.Measure(s.seconds, SEC));
        for (Map.Entry<String, Double> e : s.stages.entrySet())
            m.put("stage_" + e.getKey() + "_ms", new CrashReport.Measure(e.getValue(), MS));
        for (Map.Entry<String, Double> e : s.nums.entrySet()) {
            String k = e.getKey();
            m.put(k, new CrashReport.Measure(e.getValue(), k.endsWith("_ms") ? MS : k.endsWith("_pct") ? PCT : NONE));
        }
        return m;
    }

    /** The pack's toolkit digest, its first 12 characters, or null. */
    static String toolkit(Context c, String key) {
        if (key == null || key.isEmpty())
            return null;
        String h = "xbox".equals(CrashReport.consoleOf(key)) ? Pack.toolkitSha256(c.getFilesDir(), key)
                                                             : Pack.toolkitSha256OfKey(c.getFilesDir(), key);
        return h == null ? null : h.substring(0, Math.min(12, h.length()));
    }

    /** The tags: see the class comment. end: exited / abnormal (null for the Benchmark); state: its hash or none. */
    static Map<String, String> tags(Context c, Summary s, String key, String end, String state) {
        Map<String, String> t = new LinkedHashMap<>();
        String console = CrashReport.consoleOf(key);
        t.put("console", "unknown".equals(console) && !s.console.isEmpty() ? s.console : console);
        t.put("game_key", key);
        t.put("device", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL);
        t.put("android", String.valueOf(android.os.Build.VERSION.SDK_INT));
        t.put("abi", android.os.Build.SUPPORTED_ABIS.length > 0 ? android.os.Build.SUPPORTED_ABIS[0] : "");
        t.put("toolkit", toolkit(c, key));
        t.put("runtime", s.console);
        t.put("benchmark", s.bench ? "1" : "0");
        if (end != null)
            t.put("end", end);
        if (state != null)
            t.put("state", state);
        t.put("samples", String.valueOf(s.samples));
        for (Map.Entry<String, String> e : s.words.entrySet())
            if (!t.containsKey(e.getKey()))
                t.put(e.getKey(), e.getValue());
        return t;
    }

    static byte[] envelope(Summary s, Map<String, String> tags, long startMs, long endMs, String release,
                           String environment) {
        return CrashReport.transactionEnvelope(NAME, OP, "ok", startMs, Math.max(startMs, endMs),
                                               Collections.emptyList(), tags, measures(s), release, environment);
    }

    // ---- A run that ended -----------------------------------------------------------------------------------------

    /** Where the send runs: a short daemon thread; tests run it in place. */
    static Executor work = r -> {
        Thread th = new Thread(r, "xba-game-perf");
        th.setDaemon(true);
        th.start();
    };

    /** A network « Send reports automatically » allows now (unmetered, or any with « Also on mobile data »). */
    interface Network {
        boolean ok(Context c);
    }

    static final Network SYSTEM_NETWORK = c -> {
        ConnectivityManager cm = c.getSystemService(ConnectivityManager.class);
        return cm != null && cm.getActiveNetwork() != null
               && (AutoReports.mobileData(c) || !cm.isActiveNetworkMetered());
    };
    static Network network = SYSTEM_NETWORK;

    /**
     * The newest run ended (AutoReports.collect, the option on): its summary leaves when it is this process's and the
     * run did not crash. The file is deleted unless it is another process's. True when a send was started.
     */
    static boolean runEnded(Context c, int pid, String status, String reason, long startMs, long endMs) {
        File f = file(c);
        Summary s = read(f);
        if (s != null && s.pid != pid)
            return false;   // another process's (a game running now): it stays
        f.delete();
        if (s == null || s.bench || AutoReports.CRASHED.equals(status)
            || "not responding".equals(reason) || s.seconds <= 0)
            return false;
        if (!AutoReports.enabled(c) || !Telemetry.allowed(c) || !network.ok(c))
            return false;   // never queued
        String key = GameStore.sessionTid(c);
        byte[] body = envelope(s, tags(c, s, key, status, null), startMs, endMs, CrashReport.release(c),
                               CrashReport.environment(c));
        work.execute(() -> {
            try {
                boolean ok = CrashReport.sendAuto(c, body);
                Log.i(TAG, "perf: game run " + (ok ? "sent" : "not sent (dropped)"));
            } catch (Throwable e) {   // a report must never break the launcher
                Log.w(TAG, "perf: game run not sent: " + e);
            }
        });
        return true;
    }

    // ---- The Benchmark --------------------------------------------------------------------------------------------

    /** How long the Benchmark plays, in the summary's seconds (pauses left out). */
    static final double BENCH_SECONDS = 60;

    /** The Benchmark's summary file (the game's process). */
    static File benchFile(Context c) {
        return new File(new File(c.getFilesDir(), "logs"), "bench-perf.json");
    }

    /**
     * A state's identity: the first 12 hex digits of SHA-256 over the game's key, the slot, when it was saved and its
     * thumbnail (the state's own picture); « none » without a state.
     */
    static String stateHash(String key, int slot, long savedAt, int[] thumb) {
        if (savedAt <= 0)
            return "none";
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            md.update((key == null ? "" : key).getBytes(StandardCharsets.UTF_8));
            md.update(("|" + slot + "|" + savedAt + "|").getBytes(StandardCharsets.UTF_8));
            if (thumb != null)
                for (int v : thumb)
                    md.update(new byte[] { (byte)(v >>> 24), (byte)(v >>> 16), (byte)(v >>> 8), (byte)v });
            StringBuilder b = new StringBuilder();
            for (byte x : md.digest())
                b.append(String.format(Locale.ROOT, "%02x", x));
            return b.substring(0, 12);
        } catch (NoSuchAlgorithmException e) {
            return "unknown";
        }
    }

    /** The Benchmark's result line: « 59.8 fps (5 %: 57.0) · p99 17.1 ms ». */
    static String resultLine(Summary s) {
        return String.format(Locale.ROOT, "%.1f fps (5 %%: %.1f) · p99 %.1f ms", s.fpsShownMedian, s.fpsShownP5,
                             s.frameP99);
    }

    /** The Benchmark's transaction (benchmark=1, the state's hash), for Send. */
    static byte[] benchEnvelope(Context c, Summary s, String key, String state, long endMs) {
        long start = endMs - Math.round(s.seconds * 1000);
        return envelope(s, tags(c, s, key, null, state), start, endMs, CrashReport.release(c),
                        CrashReport.environment(c));
    }

    /** « Start » without a state: the next start of the game's process measures its boot (one start). */
    static void setBenchPending(Context c) {
        File f = new File(new File(c.getFilesDir(), "logs"), "bench-pending");
        try {
            File dir = f.getParentFile();
            if (dir != null && (dir.mkdirs() || dir.isDirectory()))
                Files.write(f.toPath(), new byte[] { '1' });
        } catch (IOException e) {
            Log.w(TAG, "perf: cannot note the benchmark", e);
        }
    }

    /** True once after setBenchPending, within 5 minutes (the file goes as it is read). */
    static boolean takeBenchPending(Context c) {
        File f = new File(new File(c.getFilesDir(), "logs"), "bench-pending");
        boolean fresh = f.isFile() && System.currentTimeMillis() - f.lastModified() < 5 * 60_000L;
        f.delete();
        return fresh;
    }
}
