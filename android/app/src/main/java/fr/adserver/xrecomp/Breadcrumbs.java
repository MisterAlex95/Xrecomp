package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;

/**
 * The last events of a game's run (XBA-338, XBA-340): a small ring in files/logs/breadcrumbs.txt that the game process
 * appends to and the launcher reads when it builds a report. Privacy by construction: an event is a name from
 * {@link #ALLOWED} and at most four key=value pairs whose keys and values are short identifiers ([A-Za-z0-9_.-], 24
 * characters at most): a slot number, on/off, a renderer's name. No free text, file, save or game names ever get in;
 * anything else is dropped at write time and again at read time.
 *
 * One line per event: « epochSeconds TAB name [TAB k=v[,k=v…]] ». Any process may write (an append is one write call);
 * the file is cut back to the last {@link #KEEP} lines when it grows past {@link #CUT}.
 */
final class Breadcrumbs {
    static final String FILE = "breadcrumbs.txt";
    /** What a report carries, and when the file is cut back. */
    static final int KEEP = 50, CUT = 200;

    /** Every event a breadcrumb may name. Adding one is a code change, reviewed like any other. */
    static final Set<String> ALLOWED = Collections.unmodifiableSet(new HashSet<>(Arrays.asList(
            "game_start", "game_end", "savestate_save", "savestate_load", "rewind", "fast_forward", "cheat", "mod",
            "renderer", "resolution", "speed", "setting", "pause", "resume", "menu_open", "menu_close",
            "pad_connect", "pad_disconnect", "low_memory", "disc_change", "prepare_start", "prepare_end")));

    private static final Pattern TOKEN = Pattern.compile("[A-Za-z0-9_.-]{1,24}");

    static final class Crumb {
        final long seconds;
        final String name;
        /** key, value, key, value… */
        final List<String> data;

        Crumb(long seconds, String name, List<String> data) {
            this.seconds = seconds;
            this.name = name;
            this.data = data;
        }
    }

    private Breadcrumbs() {}

    static File file(Context c) {
        return new File(new File(c.getFilesDir(), "logs"), FILE);
    }

    /** A new run: the ring starts empty, with « game_start ». */
    static void startRun(Context c, String... kv) {
        File f = file(c);
        f.delete();
        add(c, "game_start", kv);
    }

    /** Appends one event; kv is key, value, key, value… Silently ignores what the allow-list refuses. */
    static void add(Context c, String name, String... kv) {
        String line = line(System.currentTimeMillis() / 1000, name, kv);
        if (line == null)
            return;
        File f = file(c);
        synchronized (Breadcrumbs.class) {
            File dir = f.getParentFile();
            if (dir != null && !(dir.mkdirs() || dir.isDirectory()))
                return;
            try (FileOutputStream o = new FileOutputStream(f, true)) {
                o.write(line.getBytes(StandardCharsets.UTF_8));
            } catch (IOException e) {
                return;
            }
            if (f.length() > CUT * 16L) {   // the shortest line is ~18 bytes: count only past that
                List<Crumb> all = read(f, Integer.MAX_VALUE);
                if (all.size() > CUT)
                    rewrite(f, all.subList(all.size() - KEEP, all.size()));
            }
        }
    }

    /** The file's line for an event, or null when the allow-list refuses it. */
    static String line(long seconds, String name, String... kv) {
        if (name == null || !ALLOWED.contains(name) || (kv != null && (kv.length % 2 != 0 || kv.length > 8)))
            return null;
        StringBuilder b = new StringBuilder().append(seconds).append('\t').append(name);
        if (kv != null && kv.length > 0) {
            b.append('\t');
            for (int i = 0; i < kv.length; i += 2) {
                if (kv[i] == null || kv[i + 1] == null || !TOKEN.matcher(kv[i]).matches()
                    || !TOKEN.matcher(kv[i + 1]).matches())
                    return null;
                if (i > 0)
                    b.append(',');
                b.append(kv[i]).append('=').append(kv[i + 1]);
            }
        }
        return b.append('\n').toString();
    }

    /** The last max events of the file, oldest first; lines the allow-list refuses are skipped. */
    static List<Crumb> read(File f, int max) {
        List<Crumb> out = new ArrayList<>();
        if (f == null || !f.isFile())
            return out;
        List<String> lines;
        try {
            lines = Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return out;
        }
        for (String l : lines) {
            Crumb c = parse(l);
            if (c != null)
                out.add(c);
        }
        return out.size() > max ? new ArrayList<>(out.subList(out.size() - max, out.size())) : out;
    }

    static Crumb parse(String l) {
        String[] p = l.split("\t", -1);
        if (p.length < 2 || p.length > 3 || !ALLOWED.contains(p[1]))
            return null;
        long t;
        try {
            t = Long.parseLong(p[0]);
        } catch (NumberFormatException e) {
            return null;
        }
        List<String> data = new ArrayList<>();
        if (p.length == 3 && !p[2].isEmpty()) {
            for (String kv : p[2].split(",", -1)) {
                int eq = kv.indexOf('=');
                if (eq < 0)
                    return null;
                String k = kv.substring(0, eq), v = kv.substring(eq + 1);
                if (!TOKEN.matcher(k).matches() || !TOKEN.matcher(v).matches())
                    return null;
                data.add(k);
                data.add(v);
            }
            if (data.size() > 8)
                return null;
        }
        return new Crumb(t, p[1], data);
    }

    private static void rewrite(File f, List<Crumb> keep) {
        StringBuilder b = new StringBuilder();
        for (Crumb c : keep)
            b.append(line(c.seconds, c.name, c.data.toArray(new String[0])));
        File tmp = new File(f.getPath() + ".tmp");
        try {
            Files.write(tmp.toPath(), b.toString().getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(f))
                tmp.delete();
        } catch (IOException e) {
            tmp.delete();
        }
    }
}
