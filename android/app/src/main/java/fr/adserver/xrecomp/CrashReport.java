package fr.adserver.xrecomp;

import android.content.Context;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * « Send report » (XBA-335): one report about a failed Prepare or the last game's run, sent only when the player taps
 * Send. A Sentry envelope (an event and the log as an attachment) posted to the DSN the build carries
 * (R.string.xba_report_dsn: -PxbaReportDsn / XBA_REPORT_DSN, default the project's Rustrak server; empty: no button).
 * No Sentry SDK (the app has no AndroidX). Everything that leaves goes through {@link #scrub} first: no storage
 * paths, URLs' hosts and logins, magnet links, IP or e-mail addresses; never any game data (the log is text the
 * app wrote). A report that could not be sent waits in files/reports/ (the newest {@link #QUEUE_MAX}) and leaves
 * with the next one.
 */
final class CrashReport {
    private static final String TAG = "xba";
    /** The log's share of a report: its head (how the run ended) and its tail (the crash buffer) are kept. */
    static final int LOG_MAX = 256 * 1024, LOG_HEAD = 32 * 1024;
    static final int QUEUE_MAX = 5;
    static final String KIND_PREPARE = "prepare", KIND_GAME = "game", KIND_FEEDBACK = "feedback";
    static final int SHOT_MAX = 512 * 1024;

    String kind = KIND_GAME;
    String type = "", value = "", message = "";
    String release = "", environment = "release";
    final Map<String, String> tags = new LinkedHashMap<>();
    final List<String> fingerprint = new ArrayList<>();
    String logName = "log.txt";
    byte[] log;
    /** The last events of the run (XBA-340), oldest first: allow-listed names and tokens only. */
    final List<Breadcrumbs.Crumb> breadcrumbs = new ArrayList<>();
    /** KIND_FEEDBACK: what the player typed (scrubbed like the rest). */
    String feedback = "";
    /** Optional image (a screenshot the player chose to add, already downscaled): sent as is, never scrubbed. */
    byte[] image;
    String imageName = "screenshot.jpg", imageType = "image/jpeg";

    /** One more attachment besides the log and the image: text, scrubbed. */
    static final class Text {
        final String name;
        final byte[] bytes;

        Text(String name, byte[] bytes) {
            this.name = name;
            this.bytes = bytes;
        }
    }

    final List<Text> texts = new ArrayList<>();

    // ---- DSN ---------------------------------------------------------------------------------------------------

    /** A Sentry DSN: https://KEY@HOST[/PATH]/PROJECT → the envelope endpoint and the public key. */
    static final class Dsn {
        final String endpoint, key;

        private Dsn(String endpoint, String key) {
            this.endpoint = endpoint;
            this.key = key;
        }

        /** Null when s is empty or not a DSN. */
        static Dsn parse(String s) {
            if (s == null)
                return null;
            Matcher m = Pattern.compile("^(https?)://([0-9A-Za-z]+)(?::[^@]*)?@([^/]+)(/.*)?/([0-9]+)/?$")
                    .matcher(s.trim());
            if (!m.matches())
                return null;
            String path = m.group(4) == null ? "" : m.group(4);
            return new Dsn(m.group(1) + "://" + m.group(3) + path + "/api/" + m.group(5) + "/envelope/", m.group(2));
        }
    }

    /** The build's DSN, or null (no « Send report » then). */
    static Dsn dsn(Context c) {
        return Dsn.parse(c.getString(R.string.xba_report_dsn));
    }

    static boolean available(Context c) {
        return dsn(c) != null;
    }

    // ---- Scrubbing ---------------------------------------------------------------------------------------------

    private static final Pattern MAGNET = Pattern.compile("magnet:\\?\\S+");
    private static final Pattern URL_ANY = Pattern.compile("\\b([A-Za-z][A-Za-z0-9+.-]{1,15})://[^/\\s\"'<>]+");
    private static final Pattern EMAIL = Pattern.compile("[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)*\\.[A-Za-z]{2,}");
    private static final Pattern IPV4 = Pattern.compile("(?<![\\d.])(?:\\d{1,3}\\.){3}\\d{1,3}(?![\\d.])");
    private static final Pattern IPV6 = Pattern.compile("(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){4,7}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])");
    /** The app's own folders: kept (they name title IDs and our files), the user id made generic. */
    private static final Pattern APP_DIR = Pattern.compile("/data/(?:user/\\d+|data)/([A-Za-z0-9_.]+)/");
    /** Shared storage, SD cards, USB drives: the folders go (spaces and all), the file name stays (up to a space). */
    private static final Pattern STORAGE = Pattern.compile(
            "(?:/storage|/sdcard|/mnt|/data/media|/data/user/\\d+|/data/data)(?:/[^/\\n\\t\"<>:]+(?=/))*/([^/\\s\"<>:]+)");
    /**
     * Desktop logs (XBA-342, the same patterns as the macOS launcher's CrashReport.swift): the Mac launcher's data
     * folder keeps its names, like APP_DIR; a home folder (/Users/NAME, /home/NAME, C:\Users\NAME) keeps only the
     * file name, and a bare home folder goes whole.
     */
    private static final Pattern MAC_APP_DIR = Pattern.compile(
            "(?:/Users/[^/\\s\"<>:]+|~)/Library/Application Support/xboxrecomp/");
    private static final Pattern HOME_DIR = Pattern.compile(
            "(?:/Users|/home)/(?:[^/\\n\\t\"<>:]+(?=/)|[^/\\s\"<>:]+)(?:/[^/\\n\\t\"<>:]+(?=/))*(?:/([^/\\s\"<>:]+))?");
    private static final Pattern WIN_HOME_DIR = Pattern.compile(
            "[A-Za-z]:[\\\\/](?i:users)[\\\\/](?:[^\\\\/\\n\\t\"<>:]+(?=[\\\\/])|[^\\\\/\\s\"<>:]+)"
            + "(?:[\\\\/][^\\\\/\\n\\t\"<>:]+(?=[\\\\/]))*(?:[\\\\/]([^\\\\/\\s\"<>:]+))?");

    /** Text safe to send: see the class comment. */
    static String scrub(String s) {
        if (s == null || s.isEmpty())
            return "";
        s = MAGNET.matcher(s).replaceAll("magnet:<link>");
        s = URL_ANY.matcher(s).replaceAll("$1://<host>");
        s = EMAIL.matcher(s).replaceAll("<email>");
        s = IPV4.matcher(s).replaceAll("<ip>");
        s = IPV6.matcher(s).replaceAll("<ip>");
        s = APP_DIR.matcher(s).replaceAll("<app:$1>/");
        StringBuffer out = new StringBuffer();
        Matcher m = STORAGE.matcher(s);
        while (m.find())
            m.appendReplacement(out, Matcher.quoteReplacement("<path>/" + m.group(1)));
        m.appendTail(out);
        s = MAC_APP_DIR.matcher(out.toString()).replaceAll("<app:xboxrecomp>/");
        s = homes(HOME_DIR, s);
        return homes(WIN_HOME_DIR, s);
    }

    /** A home folder path → « <path>/file », or « <path> » when it names no file. */
    private static String homes(Pattern p, String s) {
        StringBuffer out = new StringBuffer();
        Matcher m = p.matcher(s);
        while (m.find())
            m.appendReplacement(out, Matcher.quoteReplacement(m.group(1) == null ? "<path>" : "<path>/" + m.group(1)));
        m.appendTail(out);
        return out.toString();
    }

    /** At most max bytes: the first LOG_HEAD and the end, with a line saying what was cut. */
    static byte[] trim(byte[] log, int max) {
        if (log == null)
            return new byte[0];
        if (log.length <= max)
            return log;
        int head = Math.min(LOG_HEAD, max / 4);
        byte[] mark = ("\n# ---- " + (log.length - max) + " bytes cut ----\n").getBytes(StandardCharsets.UTF_8);
        int tail = max - head - mark.length;
        ByteArrayOutputStream b = new ByteArrayOutputStream(max);
        b.write(log, 0, head);
        b.write(mark, 0, mark.length);
        b.write(log, log.length - tail, tail);
        return b.toByteArray();
    }

    // ---- The envelope ------------------------------------------------------------------------------------------

    /** The Sentry envelope: a header, the event and (when there is one) the log as a text attachment. */
    byte[] envelope(String eventId, long now) {
        String ts = iso(now);
        StringBuilder ev = new StringBuilder("{");
        field(ev, "event_id", eventId).append(',');
        field(ev, "timestamp", ts).append(',');
        field(ev, "platform", "java").append(',');
        field(ev, "level", KIND_FEEDBACK.equals(kind) ? "info" : "error").append(',');
        field(ev, "logger", "xrecomp." + kind).append(',');
        field(ev, "release", release).append(',');
        field(ev, "environment", environment).append(',');
        ev.append("\"tags\":{");
        boolean first = true;
        for (Map.Entry<String, String> t : tags.entrySet()) {
            if (t.getValue() == null || t.getValue().isEmpty())
                continue;
            if (!first)
                ev.append(',');
            first = false;
            field(ev, t.getKey(), scrub(t.getValue()));
        }
        ev.append("},");
        if (!fingerprint.isEmpty()) {
            ev.append("\"fingerprint\":[");
            for (int i = 0; i < fingerprint.size(); i++) {
                if (i > 0)
                    ev.append(',');
                str(ev, scrub(fingerprint.get(i)));
            }
            ev.append("],");
        }
        if (!message.isEmpty()) {
            ev.append("\"message\":{");
            field(ev, "formatted", scrub(message)).append("},");
        }
        if (!breadcrumbs.isEmpty()) {
            ev.append("\"breadcrumbs\":{\"values\":[");
            for (int i = 0; i < breadcrumbs.size(); i++) {
                Breadcrumbs.Crumb cr = breadcrumbs.get(i);
                ev.append(i > 0 ? ",{" : "{").append("\"timestamp\":").append(cr.seconds).append(',');
                field(ev, "category", "game").append(',');
                field(ev, "message", cr.name);
                if (!cr.data.isEmpty()) {
                    ev.append(",\"data\":{");
                    for (int k = 0; k < cr.data.size(); k += 2) {
                        if (k > 0)
                            ev.append(',');
                        field(ev, cr.data.get(k), cr.data.get(k + 1));
                    }
                    ev.append('}');
                }
                ev.append('}');
            }
            ev.append("]},");
        }
        if (KIND_FEEDBACK.equals(kind)) {
            ev.append("\"contexts\":{\"feedback\":{");
            field(ev, "message", scrub(feedback)).append("}}}");
        } else {
            ev.append("\"exception\":{\"values\":[{");
            field(ev, "type", scrub(type)).append(',');
            field(ev, "value", scrub(value)).append("}]}}");
        }
        byte[] event = ev.toString().getBytes(StandardCharsets.UTF_8);

        ByteArrayOutputStream b = new ByteArrayOutputStream();
        StringBuilder h = new StringBuilder("{");
        field(h, "event_id", eventId).append(',');
        field(h, "sent_at", ts).append("}\n");
        h.append("{\"type\":\"").append(KIND_FEEDBACK.equals(kind) ? "feedback" : "event")
         .append("\",\"length\":").append(event.length).append("}\n");
        write(b, h.toString());
        b.write(event, 0, event.length);
        write(b, "\n");
        if (log != null && log.length > 0)
            attachment(b, logName, "text/plain",
                       trim(scrub(new String(log, StandardCharsets.UTF_8)).getBytes(StandardCharsets.UTF_8), LOG_MAX));
        for (Text t : texts)
            if (t.bytes != null && t.bytes.length > 0)
                attachment(b, t.name, "text/plain", trim(scrub(new String(t.bytes, StandardCharsets.UTF_8))
                        .getBytes(StandardCharsets.UTF_8), LOG_MAX));
        if (image != null && image.length > 0 && image.length <= SHOT_MAX)
            attachment(b, imageName, imageType, image);
        return b.toByteArray();
    }

    private static void attachment(ByteArrayOutputStream b, String name, String type, byte[] a) {
        StringBuilder ah = new StringBuilder("{\"type\":\"attachment\",\"length\":").append(a.length).append(',');
        field(ah, "filename", name).append(',');
        field(ah, "content_type", type).append("}\n");
        write(b, ah.toString());
        b.write(a, 0, a.length);
        write(b, "\n");
    }

    // ---- Items that leave without a tap (behind Telemetry's gate) ---------------------------------------------

    /**
     * One release-health session (XBA-336): a game run, init true on its first update. status: ok, exited, crashed or
     * abnormal. did: the install id (Telemetry.installId), never anything else.
     */
    static byte[] sessionEnvelope(String sid, String did, boolean init, String status, long startedMs, long nowMs,
                                  int errors, String release, String environment) {
        StringBuilder s = new StringBuilder("{");
        field(s, "sid", sid).append(',');
        if (did != null && !did.isEmpty())
            field(s, "did", did).append(',');
        s.append("\"init\":").append(init).append(',');
        field(s, "started", iso(startedMs)).append(',');
        field(s, "timestamp", iso(nowMs)).append(',');
        field(s, "status", status).append(',');
        s.append("\"errors\":").append(errors).append(',');
        s.append("\"duration\":").append(String.format(Locale.ROOT, "%.3f", Math.max(0, nowMs - startedMs) / 1000.0))
         .append(',');
        s.append("\"attrs\":{");
        field(s, "release", release).append(',');
        field(s, "environment", environment).append("}}");
        return item("{\"sent_at\":\"" + iso(nowMs) + "\"}", "session", s.toString());
    }

    /** One timed stage of a transaction: op (« prepare.recomp »), wall-clock start and end. */
    static final class Span {
        final String op;
        final long startMs, endMs;

        Span(String op, long startMs, long endMs) {
            this.op = op;
            this.startMs = startMs;
            this.endMs = endMs;
        }
    }

    /**
     * A performance transaction (XBA-341): name and op (« prepare »), its spans, status ok or internal_error, tags
     * (scrubbed). No user, no id beyond the random trace ids.
     */
    static byte[] transactionEnvelope(String name, String op, String status, long startMs, long endMs,
                                      List<Span> spans, Map<String, String> tags, String release, String environment) {
        return transactionEnvelope(name, op, status, startMs, endMs, spans, tags, null, release, environment);
    }

    /** A measurement of a transaction (Sentry « measurements »): its value and unit (« millisecond », « none »). */
    static final class Measure {
        final double value;
        final String unit;

        Measure(double value, String unit) {
            this.value = value;
            this.unit = unit;
        }
    }

    /**
     * The same with measurements (XBA game performance): numbers only, their names lower case letters, digits, « _ »
     * and « . »; a value that is not finite is left out.
     */
    static byte[] transactionEnvelope(String name, String op, String status, long startMs, long endMs,
                                      List<Span> spans, Map<String, String> tags, Map<String, Measure> measures,
                                      String release, String environment) {
        String id = UUID.randomUUID().toString().replace("-", "");
        String trace = UUID.randomUUID().toString().replace("-", "");
        String root = id.substring(0, 16);
        StringBuilder t = new StringBuilder("{");
        field(t, "event_id", id).append(',');
        field(t, "type", "transaction").append(',');
        field(t, "transaction", name).append(',');
        field(t, "platform", "java").append(',');
        field(t, "release", release).append(',');
        field(t, "environment", environment).append(',');
        t.append("\"start_timestamp\":").append(secs(startMs)).append(',');
        t.append("\"timestamp\":").append(secs(endMs)).append(',');
        t.append("\"contexts\":{\"trace\":{");
        field(t, "trace_id", trace).append(',');
        field(t, "span_id", root).append(',');
        field(t, "op", op).append(',');
        field(t, "status", status).append("}},");
        t.append("\"tags\":{");
        boolean first = true;
        for (Map.Entry<String, String> e : tags.entrySet()) {
            if (e.getValue() == null || e.getValue().isEmpty())
                continue;
            if (!first)
                t.append(',');
            first = false;
            field(t, e.getKey(), scrub(e.getValue()));
        }
        t.append('}');
        if (measures != null && !measures.isEmpty()) {
            t.append(",\"measurements\":{");
            boolean firstM = true;
            for (Map.Entry<String, Measure> e : measures.entrySet()) {
                Measure m = e.getValue();
                if (m == null || Double.isNaN(m.value) || Double.isInfinite(m.value)
                    || !e.getKey().matches("[a-z0-9_.]{1,64}"))
                    continue;
                if (!firstM)
                    t.append(',');
                firstM = false;
                str(t, e.getKey()).append(":{\"value\":")
                 .append(String.format(Locale.ROOT, "%.3f", m.value)).append(',');
                field(t, "unit", m.unit == null ? "none" : m.unit).append('}');
            }
            t.append('}');
        }
        t.append(",\"spans\":[");
        for (int i = 0; i < spans.size(); i++) {
            Span sp = spans.get(i);
            t.append(i > 0 ? ",{" : "{");
            field(t, "span_id", UUID.randomUUID().toString().replace("-", "").substring(0, 16)).append(',');
            field(t, "parent_span_id", root).append(',');
            field(t, "trace_id", trace).append(',');
            field(t, "op", sp.op).append(',');
            field(t, "description", sp.op).append(',');
            t.append("\"start_timestamp\":").append(secs(sp.startMs)).append(',');
            t.append("\"timestamp\":").append(secs(sp.endMs)).append('}');
        }
        t.append("]}");
        return item("{\"event_id\":\"" + id + "\",\"sent_at\":\"" + iso(endMs) + "\"}", "transaction",
                    t.toString());
    }

    private static String secs(long ms) {
        return String.format(Locale.ROOT, "%.3f", ms / 1000.0);
    }

    private static byte[] item(String header, String type, String payload) {
        byte[] p = payload.getBytes(StandardCharsets.UTF_8);
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        write(b, header + "\n{\"type\":\"" + type + "\",\"length\":" + p.length + "}\n");
        b.write(p, 0, p.length);
        write(b, "\n");
        return b.toByteArray();
    }

    /**
     * Sends an item that leaves without a tap (a session, a transaction, an automatic report), off the UI thread.
     * Only when Telemetry allows it; never queued (what could not leave is dropped). True when accepted.
     */
    static boolean sendAuto(Context c, byte[] body) {
        if (!Telemetry.allowed(c))
            return false;
        Dsn d = dsn(c);
        return d != null && post(c, d, body);
    }

    /**
     * Sends an item the player chose to send with a tap (the Benchmark's result): one post, never queued, not behind
     * « Send reports automatically » (the tap is the consent). Off the UI thread. True when accepted.
     */
    static boolean sendOnce(Context c, byte[] body) {
        Dsn d = dsn(c);
        return d != null && body != null && post(c, d, body);
    }

    /** What the player sees before sending, in four lines at most: the problem, the game, the device, the log. */
    String summary() {
        StringBuilder s = KIND_FEEDBACK.equals(kind) ? new StringBuilder(scrub(feedback))
                          : new StringBuilder(scrub(type)).append(value.isEmpty() ? "" : ": " + scrub(value));
        String game = tags.get("game"), key = tags.get("game_key");
        String g = (game == null || game.isEmpty() ? "" : scrub(game) + " ")
                   + (key == null || key.isEmpty() ? "" : "(" + scrub(key) + ") ") + tags.get("console");
        s.append('\n').append(g.trim());
        s.append('\n').append(tags.get("device")).append(" · Android ").append(tags.get("android")).append(" · ")
         .append(release);
        if (log != null && log.length > 0)
            s.append("\n+ ").append(logName).append(" (").append(Math.max(1, Math.min(log.length, LOG_MAX) / 1024))
             .append(" KB)");
        if (!breadcrumbs.isEmpty())
            s.append(log != null && log.length > 0 ? ", " : "\n+ ").append(breadcrumbs.size()).append(" game events");
        if (image != null && image.length > 0)
            s.append(", ").append(imageName).append(" (").append(Math.max(1, image.length / 1024)).append(" KB)");
        return s.toString();
    }

    // ---- Sending -----------------------------------------------------------------------------------------------

    /** The HTTP post, a field so the tests use a fake: the status code, or an IOException. */
    interface Transport {
        int post(String url, Map<String, String> headers, byte[] body) throws IOException;
    }

    static final Transport HTTP = CrashReport::httpPost;
    static Transport transport = HTTP;

    private static int httpPost(String url, Map<String, String> headers, byte[] body) throws IOException {
        HttpURLConnection c = (HttpURLConnection)new URL(url).openConnection();
        try {
            c.setConnectTimeout(15_000);
            c.setReadTimeout(15_000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setFixedLengthStreamingMode(body.length);
            for (Map.Entry<String, String> h : headers.entrySet())
                c.setRequestProperty(h.getKey(), h.getValue());
            try (OutputStream o = c.getOutputStream()) {
                o.write(body);
            }
            int code = c.getResponseCode();
            try (InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream()) {
                if (in != null)
                    while (in.read() >= 0) { /* drain */ }
            } catch (IOException ignored) {
                // the code is what counts
            }
            return code;
        } finally {
            c.disconnect();
        }
    }

    /** files/reports/: the reports that could not be sent. */
    static File queueDir(Context c) {
        return new File(c.getFilesDir(), "reports");
    }

    /**
     * Sends this report (off the UI thread), then the ones waiting from earlier. True when this one was accepted; when
     * not, it waits in files/reports/ for the next send.
     */
    boolean send(Context c) {
        Dsn d = dsn(c);
        if (d == null)
            return false;
        String id = UUID.randomUUID().toString().replace("-", "");
        byte[] body = envelope(id, System.currentTimeMillis());
        boolean ok = post(c, d, body);
        if (!ok)
            ok = post(c, d, body);   // one retry
        Log.i(TAG, "report: " + kind + " " + id + (ok ? " sent" : " not sent: kept"));
        if (!ok) {
            keep(c, id, body);
            return false;
        }
        flush(c, d);
        return true;
    }

    private static boolean post(Context c, Dsn d, byte[] body) {
        String version = c.getString(R.string.xba_version);
        Map<String, String> h = new LinkedHashMap<>();
        h.put("Content-Type", "application/x-sentry-envelope");
        h.put("X-Sentry-Auth", "Sentry sentry_version=7, sentry_key=" + d.key + ", sentry_client=xrecomp/" + version);
        try {
            int code = transport.post(d.endpoint, h, body);
            Telemetry.answered(c, code, 0);
            return code >= 200 && code < 300;
        } catch (IOException | RuntimeException e) {
            Log.i(TAG, "report: post failed: " + e);
            return false;
        }
    }

    private static void keep(Context c, String id, byte[] body) {
        File dir = queueDir(c);
        if (!(dir.mkdirs() || dir.isDirectory()))
            return;
        try {
            Files.write(new File(dir, System.currentTimeMillis() + "-" + id + ".envelope").toPath(), body);
        } catch (IOException e) {
            return;
        }
        File[] all = queued(c);
        for (int i = 0; i < all.length - QUEUE_MAX; i++)   // oldest first
            all[i].delete();
    }

    /** The waiting reports, oldest first. */
    static File[] queued(Context c) {
        File[] f = queueDir(c).listFiles((dir, name) -> name.endsWith(".envelope"));
        if (f == null)
            return new File[0];
        Arrays.sort(f, (a, b) -> a.getName().compareTo(b.getName()));
        return f;
    }

    private static void flush(Context c, Dsn d) {
        for (File f : queued(c)) {
            byte[] body;
            try {
                body = Files.readAllBytes(f.toPath());
            } catch (IOException e) {
                f.delete();
                continue;
            }
            if (!post(c, d, body))
                return;   // still offline: the rest wait
            f.delete();
        }
    }

    // ---- The two kinds -----------------------------------------------------------------------------------------

    /** A failed Prepare: its code and message, the title, and prepare.log. */
    static CrashReport prepare(Context c, String key, String title, String code, String msg, File log) {
        CrashReport r = base(c, KIND_PREPARE);
        r.type = "PrepareFailed";
        r.value = (code == null || code.isEmpty() ? "?" : code) + (msg == null || msg.isEmpty() ? "" : ": " + msg);
        game(r, key, title);
        r.tags.put("prepare_code", code);
        r.fingerprint.addAll(Arrays.asList("prepare", r.tags.get("console"), key == null ? "" : key,
                                           code == null ? "" : code));
        r.logName = "prepare.log";
        r.log = read(log);
        return r;
    }

    /**
     * The last game's run, from the file LogProvider.dumpLastGame wrote: its header says how the process ended, the
     * crash buffer holds the backtrace. key: the title of the last session (GameStore.sessionTid), or null.
     */
    static CrashReport game(Context c, String key, String title, File lastGameLog) {
        CrashReport r = base(c, KIND_GAME);
        r.log = read(lastGameLog);
        String text = r.log == null ? "" : new String(r.log, 0, Math.min(r.log.length, 4096), StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("(?m)^# game process \\d+ ended [^:]+:\\d\\d:[\\d.]+: ([^,(\\n]+)(.*)$").matcher(text);
        boolean found = m.find();
        String reason = found ? m.group(1).trim() : "unknown end";
        r.type = "Game: " + reason;
        r.value = found ? m.group(2).replaceFirst("^[,\\s]+", "").trim() : "";
        game(r, key, title);
        r.tags.put("end", reason);
        String frame = topFrame(r.log);
        r.message = frame.isEmpty() ? "" : "top frame: " + frame;
        r.fingerprint.addAll(Arrays.asList("game", r.tags.get("console"), key == null ? "" : key, reason, frame));
        r.logName = LogProvider.LAST_GAME_LOG;
        r.breadcrumbs.addAll(Breadcrumbs.read(Breadcrumbs.file(c), Breadcrumbs.KEEP));
        return r;
    }

    /**
     * « Report a problem » (XBA-339): what the player typed, the game (or none), the log and the run's breadcrumbs.
     * Tap-only. The screenshot, when the player ticks it, is set by the caller ({@link #image}).
     */
    static CrashReport feedback(Context c, String key, String title, String text, File log, String logName) {
        CrashReport r = base(c, KIND_FEEDBACK);
        r.feedback = text == null ? "" : text.trim();
        game(r, key, title);
        r.logName = logName == null ? "log.txt" : logName;
        r.log = read(log);
        r.breadcrumbs.addAll(Breadcrumbs.read(Breadcrumbs.file(c), Breadcrumbs.KEEP));
        return r;
    }

    /** The first native frame of the crash's backtrace (« #00 pc … lib (symbol+off) »), its addresses dropped. */
    static String topFrame(byte[] log) {
        if (log == null)
            return "";
        Matcher m = Pattern.compile("#00 pc [0-9a-fA-F]+\\s+(\\S+)(?:\\s+\\(([^)+]+))?").matcher(
                new String(log, StandardCharsets.UTF_8));
        if (!m.find())
            return "";
        String lib = m.group(1).replaceAll(".*/", "");
        return m.group(2) == null ? lib : lib + " " + m.group(2);
    }

    /** Every item's release (« xrecomp@<version> ») and environment (a debug or a release build). */
    static String release(Context c) {
        return "xrecomp@" + c.getString(R.string.xba_version);
    }

    static String environment(Context c) {
        return (c.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0 ? "debug"
                                                                                                       : "release";
    }

    private static CrashReport base(Context c, String kind) {
        CrashReport r = new CrashReport();
        r.kind = kind;
        r.release = release(c);
        r.environment = environment(c);
        r.tags.put("kind", kind);
        r.tags.put("device", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL);
        r.tags.put("android", String.valueOf(android.os.Build.VERSION.SDK_INT));
        r.tags.put("abi", android.os.Build.SUPPORTED_ABIS.length > 0 ? android.os.Build.SUPPORTED_ABIS[0] : "");
        return r;
    }

    private static void game(CrashReport r, String key, String title) {
        r.tags.put("console", consoleOf(key));
        r.tags.put("game_key", key);
        r.tags.put("game", title);
    }

    /** The console of a library key: its prefix (« n64-… », « gc-… »); an Xbox title ID has none. */
    static String consoleOf(String key) {
        if (key == null || key.isEmpty())
            return "unknown";
        Matcher m = Pattern.compile("^([a-z][a-z0-9]{1,5})-").matcher(key);
        return m.find() ? m.group(1) : "xbox";
    }

    private static byte[] read(File f) {
        if (f == null || !f.isFile())
            return null;
        try {
            byte[] all = Files.readAllBytes(f.toPath());
            return all.length > 8 * LOG_MAX ? Arrays.copyOfRange(all, all.length - 8 * LOG_MAX, all.length) : all;
        } catch (IOException e) {
            return null;
        }
    }

    // ---- JSON --------------------------------------------------------------------------------------------------

    private static StringBuilder field(StringBuilder b, String k, String v) {
        str(b, k).append(':');
        return str(b, v == null ? "" : v);
    }

    static StringBuilder str(StringBuilder b, String s) {
        b.append('"');
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            switch (ch) {
            case '"': b.append("\\\""); break;
            case '\\': b.append("\\\\"); break;
            case '\n': b.append("\\n"); break;
            case '\r': b.append("\\r"); break;
            case '\t': b.append("\\t"); break;
            default:
                if (ch < 0x20)
                    b.append(String.format(Locale.ROOT, "\\u%04x", (int)ch));
                else
                    b.append(ch);
            }
        }
        return b.append('"');
    }

    private static void write(ByteArrayOutputStream b, String s) {
        byte[] x = s.getBytes(StandardCharsets.UTF_8);
        b.write(x, 0, x.length);
    }

    static String iso(long t) {
        SimpleDateFormat f = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return f.format(new Date(t));
    }
}
