package fr.adserver.xrecomp;

import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * A game's journal (docs/android/app-shell.md « Journal »): what happened to it, one JSON object per line in
 * files/journal/&lt;key&gt;.jsonl (the library key: an Xbox title ID, « gba-… »…), at most MAX lines. A Prepare that
 * ended well (its time, files compiled, whether it was a background round), a run that met code the preparation missed
 * (how much; a crash or not), a crash nothing new came out of. Written by PrepareService and the launcher; the
 * journal starts the day the app has it: a game prepared before gets one line from its pack.json (« seeded »).
 */
final class GameJournal {
    private GameJournal() {}

    static final int MAX = 200;
    static final String PREPARED = "prepared", FOUND = "found", NEEDS_FIX = "needs_fix";

    /** One line of the journal. */
    static final class Event {
        final long time;
        final String kind;
        final JSONObject o;

        Event(long time, String kind, JSONObject o) {
            this.time = time;
            this.kind = kind;
            this.o = o;
        }
    }

    static File file(File filesDir, String key) {
        return new File(new File(filesDir, "journal"), key + ".jsonl");
    }

    /** Appends an event of kind at now with fields (key, value pairs), keeping the last MAX lines. */
    static void add(File filesDir, String key, String kind, long now, Object... fields) {
        if (key == null || key.isEmpty())
            return;
        try {
            JSONObject o = new JSONObject().put("t", now).put("kind", kind);
            for (int i = 0; i + 1 < fields.length; i += 2)
                o.put(String.valueOf(fields[i]), fields[i + 1]);
            File f = file(filesDir, key);
            List<String> lines = lines(f);
            lines.add(o.toString());
            if (lines.size() > MAX)
                lines = lines.subList(lines.size() - MAX, lines.size());
            Library.writeAtomically(f, android.text.TextUtils.join("\n", lines) + "\n");
        } catch (JSONException | IOException e) {
            Log.w("xba", "journal: cannot write " + key, e);
        }
    }

    private static List<String> lines(File f) throws IOException {
        List<String> out = new ArrayList<>();
        if (!f.isFile())
            return out;
        for (String l : PrepareProtocol.readText(f, 1 << 20).split("\n"))
            if (!l.trim().isEmpty())
                out.add(l);
        return out;
    }

    /** A Prepare of key ended well: its pack.json's metrics, background: a round in the background. */
    static void prepared(File filesDir, String key, boolean background, long now) {
        JSONObject p = PrepareFacts.of(filesDir, key);
        add(filesDir, key, PREPARED, now, "total_s", p == null ? -1 : p.optDouble("total_s", -1),
            "files", p == null ? 0 : p.optInt("files", 0), "compiled", p == null ? 0 : p.optInt("compiled", 0),
            "background", background);
    }

    /**
     * The journal of key, newest first. None yet and a pack: one « prepared » line from the pack (built_at and its
     * prepare metrics), written so that it stays.
     */
    static List<Event> read(File filesDir, String key) {
        List<Event> out = new ArrayList<>();
        File f = file(filesDir, key);
        try {
            if (!f.isFile())
                seed(filesDir, key);
            for (String l : lines(f)) {
                try {
                    JSONObject o = new JSONObject(l);
                    out.add(new Event(o.optLong("t", 0), o.optString("kind", ""), o));
                } catch (JSONException ignored) {   // a torn line: skipped
                }
            }
        } catch (IOException e) {
            Log.w("xba", "journal: cannot read " + key, e);
        }
        Collections.reverse(out);
        return out;
    }

    private static void seed(File filesDir, String key) {
        File json = new File(Pack.packDir(filesDir, key), Pack.JSON);
        if (!json.isFile())
            return;
        long t = json.lastModified();
        try {
            String at = new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optString("built_at", "");
            if (!at.isEmpty()) {
                SimpleDateFormat iso = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.ROOT);
                iso.setTimeZone(TimeZone.getTimeZone("UTC"));
                t = iso.parse(at).getTime();
            }
        } catch (IOException | JSONException | java.text.ParseException ignored) {
        }
        JSONObject p = PrepareFacts.of(filesDir, key);
        add(filesDir, key, PREPARED, t, "total_s", p == null ? -1 : p.optDouble("total_s", -1),
            "files", p == null ? 0 : p.optInt("files", 0), "compiled", p == null ? 0 : p.optInt("compiled", 0),
            "seeded", true);
    }
}
