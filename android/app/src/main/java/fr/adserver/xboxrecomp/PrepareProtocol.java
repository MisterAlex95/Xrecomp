package fr.adserver.xboxrecomp;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The on-device Prepare's wire formats (docs/android/prepare.md): the lines `python -m tools.prepare --android`
 * prints on stdout, and status.json, which PrepareService (process ":prepare") writes and LauncherActivity
 * (process ":launcher") polls. Plain Java and org.json, so the JVM unit tests run it as is.
 *
 * <pre>
 * XBA_PROGRESS step=&lt;i&gt;/&lt;n&gt; id=&lt;identify|analyse|lift|compile|pack&gt; pct=&lt;0-100&gt; [eta=&lt;s&gt;] msg=&lt;rest&gt;
 * XBA_RESULT ok pack=&lt;dir&gt;
 * XBA_RESULT error code=&lt;UPPER_SNAKE&gt; msg=&lt;text&gt;
 * </pre>
 * msg is last and takes the rest of the line; unknown keys are ignored. Exit 0 ok, 1 error, 130 cancelled.
 */
final class PrepareProtocol {
    private PrepareProtocol() {}

    static final String PROGRESS = "XBA_PROGRESS";
    static final String RESULT = "XBA_RESULT";

    /** The steps of tools.prepare, in order; « install » is the app's own phase before it (the tools' assets). */
    static final String[] STEPS = { "identify", "analyse", "lift", "compile", "pack" };
    static final String STEP_INSTALL = "install";

    /** One XBA_PROGRESS line. */
    static final class Progress {
        int step, n, pct;
        String id = "";
        /** Seconds left, or -1 when not given. */
        long eta = -1;
        String msg = "";
    }

    /** One XBA_RESULT line. */
    static final class Result {
        boolean ok;
        String pack = "", code = "", msg = "";
    }

    /**
     * Splits "key=value key=value msg=rest of the line" after the line's first word. Calls sink for each pair;
     * a key in greedy takes the rest of the line. Bare words (no '=') go to sink with a null value.
     */
    private interface Sink {
        void put(String key, String value);
    }

    private static void fields(String rest, Sink sink, String... greedy) {
        int i = 0, len = rest.length();
        while (i < len) {
            while (i < len && rest.charAt(i) == ' ')
                i++;
            if (i >= len)
                break;
            int end = rest.indexOf(' ', i);
            if (end < 0)
                end = len;
            String tok = rest.substring(i, end);
            int eq = tok.indexOf('=');
            if (eq <= 0) {
                sink.put(tok, null);
                i = end;
                continue;
            }
            String key = tok.substring(0, eq);
            for (String g : greedy) {
                if (g.equals(key)) {
                    sink.put(key, rest.substring(i + eq + 1));
                    return;
                }
            }
            sink.put(key, tok.substring(eq + 1));
            i = end;
        }
    }

    private static String afterWord(String line, String word) {
        if (line == null)
            return null;
        String l = line;
        if (l.endsWith("\r"))
            l = l.substring(0, l.length() - 1);
        if (!l.startsWith(word))
            return null;
        if (l.length() == word.length())
            return "";
        if (l.charAt(word.length()) != ' ')
            return null;
        return l.substring(word.length() + 1);
    }

    /** A progress line, or null when the line is not one (or is malformed: no step i/n, no id, no pct). */
    static Progress parseProgress(String line) {
        String rest = afterWord(line, PROGRESS);
        if (rest == null)
            return null;
        Progress p = new Progress();
        boolean[] seen = new boolean[3];
        boolean[] bad = new boolean[1];
        fields(rest, (k, v) -> {
            if (v == null)
                return;
            switch (k) {
            case "step": {
                int slash = v.indexOf('/');
                try {
                    p.step = Integer.parseInt(v.substring(0, slash < 0 ? v.length() : slash));
                    p.n = slash < 0 ? 0 : Integer.parseInt(v.substring(slash + 1));
                    seen[0] = slash > 0 && p.n > 0 && p.step >= 0 && p.step <= p.n;
                    if (!seen[0])
                        bad[0] = true;
                } catch (NumberFormatException e) {
                    bad[0] = true;
                }
                break;
            }
            case "id":
                p.id = v;
                seen[1] = !v.isEmpty();
                break;
            case "pct":
                try {
                    double d = Double.parseDouble(v);
                    if (Double.isNaN(d))
                        throw new NumberFormatException();
                    p.pct = (int)Math.round(Math.max(0, Math.min(100, d)));
                    seen[2] = true;
                } catch (NumberFormatException e) {
                    bad[0] = true;
                }
                break;
            case "eta":
                try {
                    double d = Double.parseDouble(v);
                    p.eta = d >= 0 && !Double.isNaN(d) ? Math.round(d) : -1;
                } catch (NumberFormatException e) {
                    p.eta = -1;   // optional: a bad eta drops only the eta
                }
                break;
            case "msg":
                p.msg = v;
                break;
            default:
                break;   // unknown keys are ignored
            }
        }, "msg");
        if (bad[0] || !seen[0] || !seen[1] || !seen[2])
            return null;
        return p;
    }

    /** A result line, or null when the line is not one. */
    static Result parseResult(String line) {
        String rest = afterWord(line, RESULT);
        if (rest == null)
            return null;
        Result r = new Result();
        int[] word = { 0 };
        boolean[] valid = new boolean[1];
        fields(rest, (k, v) -> {
            if (v == null) {
                if (word[0]++ == 0) {
                    if (k.equals("ok")) {
                        r.ok = true;
                        valid[0] = true;
                    } else if (k.equals("error")) {
                        valid[0] = true;
                    }
                }
                return;
            }
            switch (k) {
            case "pack":
                r.pack = v;
                break;
            case "code":
                r.code = v;
                break;
            case "msg":
                r.msg = v;
                break;
            default:
                break;
            }
        }, "msg", "pack");
        if (!valid[0])
            return null;
        if (!r.ok && r.code.isEmpty())
            r.code = "UNKNOWN";
        return r;
    }

    /** Overall progress 0..1 from a step i of n at pct (step 0, the install phase, counts as 0). */
    static float overall(int step, int n, int pct) {
        if (n <= 0 || step <= 0)
            return 0;
        return Math.max(0, Math.min(1, ((step - 1) + pct / 100f) / n));
    }

    /** Index of a step id in STEPS, -1 for install or an unknown id. */
    static int stepIndex(String id) {
        for (int i = 0; i < STEPS.length; i++) {
            if (STEPS[i].equals(id))
                return i;
        }
        return -1;
    }

    // ---- status.json --------------------------------------------------------------------------------------------

    static final String RUNNING = "running", DONE = "done", FAILED = "failed", CANCELLED = "cancelled";

    /** <files>/prepare/<TID>/status.json, written atomically (temporary file, then rename). */
    static final class Status {
        String state = RUNNING;
        int step, n = STEPS.length, pct;
        String id = STEP_INSTALL;
        long eta = -1;
        String msg = "", code = "";
        /** Milliseconds since the epoch. */
        long startedAt, updatedAt;
        String log = "";
        /** The jobs of the current attempt and the attempt number (1, then 2 after a kill). */
        int jobs, attempt = 1;
        String titleId = "";

        boolean running() {
            return RUNNING.equals(state);
        }

        JSONObject toJson() {
            JSONObject o = new JSONObject();
            try {
                o.put("state", state);
                o.put("step", step);
                o.put("n", n);
                o.put("id", id);
                o.put("pct", pct);
                o.put("eta", eta);
                o.put("msg", msg);
                o.put("code", code);
                o.put("started_at", startedAt);
                o.put("updated_at", updatedAt);
                o.put("log", log);
                o.put("jobs", jobs);
                o.put("attempt", attempt);
                o.put("title_id", titleId);
            } catch (JSONException e) {
                throw new IllegalStateException(e);
            }
            return o;
        }

        static Status fromJson(JSONObject o) {
            Status s = new Status();
            s.state = o.optString("state", FAILED);
            s.step = o.optInt("step", 0);
            s.n = o.optInt("n", STEPS.length);
            s.id = o.optString("id", "");
            s.pct = o.optInt("pct", 0);
            s.eta = o.optLong("eta", -1);
            s.msg = o.optString("msg", "");
            s.code = o.optString("code", "");
            s.startedAt = o.optLong("started_at", 0);
            s.updatedAt = o.optLong("updated_at", 0);
            s.log = o.optString("log", "");
            s.jobs = o.optInt("jobs", 0);
            s.attempt = o.optInt("attempt", 1);
            s.titleId = o.optString("title_id", "");
            return s;
        }

        Status copy() {
            return fromJson(toJson());
        }
    }

    static void writeStatus(File file, Status s) throws IOException {
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs())
            throw new IOException("cannot create " + dir);
        File tmp = new File(file.getPath() + ".tmp");
        byte[] bytes;
        try {
            bytes = s.toJson().toString(1).getBytes(StandardCharsets.UTF_8);
        } catch (JSONException e) {
            throw new IOException(e);
        }
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(bytes);
            out.getFD().sync();
        }
        if (!tmp.renameTo(file))
            throw new IOException("cannot rename " + tmp + " to " + file);
    }

    /** status.json, or null when missing or unreadable. */
    static Status readStatus(File file) {
        if (!file.isFile())
            return null;
        try {
            return Status.fromJson(new JSONObject(readText(file, 1 << 20)));
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    static String readText(File f, int max) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                b.write(buf, 0, n);
                if (b.size() > max)
                    throw new IOException(f + " is too big");
            }
            return b.toString("UTF-8");
        }
    }

    /** « 12 min », « 1 h 05 » from seconds. */
    static String formatEta(long s) {
        if (s < 60)
            return "< 1 min";
        long m = (s + 59) / 60;
        if (m < 60)
            return m + " min";
        return String.format(Locale.ROOT, "%d h %02d", m / 60, m % 60);
    }
}
