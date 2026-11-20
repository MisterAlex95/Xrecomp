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
    static final String KIND_PREPARE = "prepare", KIND_GAME = "game";

    String kind = KIND_GAME;
    String type = "", value = "", message = "";
    String release = "", environment = "release";
    final Map<String, String> tags = new LinkedHashMap<>();
    final List<String> fingerprint = new ArrayList<>();
    String logName = "log.txt";
    byte[] log;

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
        field(ev, "level", "error").append(',');
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
        ev.append("\"exception\":{\"values\":[{");
        field(ev, "type", scrub(type)).append(',');
        field(ev, "value", scrub(value)).append("}]}}");
        byte[] event = ev.toString().getBytes(StandardCharsets.UTF_8);

        ByteArrayOutputStream b = new ByteArrayOutputStream();
        StringBuilder h = new StringBuilder("{");
        field(h, "event_id", eventId).append(',');
        field(h, "sent_at", ts).append("}\n");
        h.append("{\"type\":\"event\",\"length\":").append(event.length).append("}\n");
        write(b, h.toString());
        b.write(event, 0, event.length);
        write(b, "\n");
        if (log != null && log.length > 0) {
            byte[] a = trim(scrub(new String(log, StandardCharsets.UTF_8)).getBytes(StandardCharsets.UTF_8), LOG_MAX);
            StringBuilder ah = new StringBuilder("{\"type\":\"attachment\",\"length\":").append(a.length).append(',');
            field(ah, "filename", logName).append(',');
            field(ah, "content_type", "text/plain").append("}\n");
            write(b, ah.toString());
            b.write(a, 0, a.length);
            write(b, "\n");
        }
        return b.toByteArray();
    }

    /** What the player sees before sending, in four lines at most: the problem, the game, the device, the log. */
    String summary() {
        StringBuilder s = new StringBuilder(scrub(type)).append(value.isEmpty() ? "" : ": " + scrub(value));
        String game = tags.get("game"), key = tags.get("game_key");
        String g = (game == null || game.isEmpty() ? "" : scrub(game) + " ")
                   + (key == null || key.isEmpty() ? "" : "(" + scrub(key) + ") ") + tags.get("console");
        s.append('\n').append(g.trim());
        s.append('\n').append(tags.get("device")).append(" · Android ").append(tags.get("android")).append(" · ")
         .append(release);
        if (log != null && log.length > 0)
            s.append("\n+ ").append(logName).append(" (").append(Math.max(1, Math.min(log.length, LOG_MAX) / 1024))
             .append(" KB)");
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
        boolean ok = post(d, body, c.getString(R.string.xba_version));
        if (!ok)
            ok = post(d, body, c.getString(R.string.xba_version));   // one retry
        Log.i(TAG, "report: " + kind + " " + id + (ok ? " sent" : " not sent: kept"));
        if (!ok) {
            keep(c, id, body);
            return false;
        }
        flush(c, d);
        return true;
    }

    private static boolean post(Dsn d, byte[] body, String version) {
        Map<String, String> h = new LinkedHashMap<>();
        h.put("Content-Type", "application/x-sentry-envelope");
        h.put("X-Sentry-Auth", "Sentry sentry_version=7, sentry_key=" + d.key + ", sentry_client=xrecomp/" + version);
        try {
            int code = transport.post(d.endpoint, h, body);
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
            if (!post(d, body, c.getString(R.string.xba_version)))
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

    private static CrashReport base(Context c, String kind) {
        CrashReport r = new CrashReport();
        r.kind = kind;
        r.release = "xrecomp@" + c.getString(R.string.xba_version);
        r.environment = (c.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
                        ? "debug" : "release";
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
