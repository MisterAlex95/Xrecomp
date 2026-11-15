package fr.adserver.xrecomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.text.Normalizer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Properties;
import java.util.Set;

/**
 * How the library shows its games (XBA-206, docs/android/app-shell.md « Game library »): the sort (name, last played,
 * console, most played), the console filter (the top bar's tabs, L1 / R1), the search typed on the on-screen keyboard,
 * the favourites (first in the grid, X on a card or on a game's Home) and the « Recently played » row. Pure: the
 * launcher hands it the entries; the view state and the favourites are one small file of this device,
 * files/library_view.properties (the search is not kept: it goes with the screen).
 */
final class LibraryView {
    private LibraryView() {}

    static final String FILE = "library_view.properties";
    /** The « Recently played » row: one row of the grid. */
    static final int RECENT_MAX = LibraryScreen.COLUMNS;

    enum Sort {
        NAME(R.string.LIB_SORT_NAME), LAST_PLAYED(R.string.LIB_SORT_LAST_PLAYED), CONSOLE(R.string.LIB_SORT_CONSOLE),
        MOST_PLAYED(R.string.LIB_SORT_MOST_PLAYED);

        final int label;

        Sort(int label) {
            this.label = label;
        }

        Sort next() {
            return values()[(ordinal() + 1) % values().length];
        }

        static Sort parse(String s) {
            for (Sort x : values())
                if (x.name().equalsIgnoreCase(s))
                    return x;
            return NAME;
        }
    }

    /** The view state: its sort and console filter are kept, the search is not. */
    static final class State {
        Sort sort = Sort.NAME;
        /** null: every console. */
        Console console;
        String query = "";
        final Set<String> favourites = new LinkedHashSet<>();

        boolean favourite(String key) {
            return key != null && favourites.contains(key);
        }

        /** Adds or removes key; true when it is a favourite now. */
        boolean toggleFavourite(String key) {
            if (key == null || key.isEmpty())
                return false;
            if (favourites.remove(key))
                return false;
            favourites.add(key);
            return true;
        }

        /** Something narrows the grid (a console or a search): the « Recently played » row steps aside. */
        boolean narrowed() {
            return console != null || !query.trim().isEmpty();
        }
    }

    static File file(File filesDir) {
        return new File(filesDir, FILE);
    }

    /** The saved state, the defaults when there is none or it cannot be read. */
    static State load(File filesDir) {
        State s = new State();
        File f = file(filesDir);
        if (!f.isFile())
            return s;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(f)) {
            p.load(new InputStreamReader(in, StandardCharsets.UTF_8));
        } catch (IOException | IllegalArgumentException e) {
            return s;
        }
        s.sort = Sort.parse(p.getProperty("sort", ""));
        String con = p.getProperty("console", "");
        s.console = con.isEmpty() ? null : Console.fromPack(con);   // (fromPack would read "" as the Xbox)
        for (String k : p.getProperty("favourites", "").split("\n"))
            if (!k.trim().isEmpty())
                s.favourites.add(k.trim());
        return s;
    }

    static boolean save(File filesDir, State s) {
        StringBuilder b = new StringBuilder("# XRecomp library view (XBA-206)\n");
        b.append("sort=").append(s.sort.name().toLowerCase(Locale.ROOT)).append('\n');
        b.append("console=").append(s.console == null ? "" : s.console.id).append('\n');
        b.append("favourites=").append(escape(String.join("\n", s.favourites))).append('\n');
        try {
            Library.writeAtomically(file(filesDir), b.toString());
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /** A Properties value: newlines and backslashes escaped (the keys are title IDs and library keys). */
    private static String escape(String v) {
        return v.replace("\\", "\\\\").replace("\n", "\\n");
    }

    // ---- What the grid shows ------------------------------------------------------------------------------------

    /** Lower case, accents gone: « Pokémon » matches « pokemon ». */
    static String fold(String s) {
        if (s == null)
            return "";
        String n = Normalizer.normalize(s, Normalizer.Form.NFD).replaceAll("\\p{M}+", "");
        return n.toLowerCase(Locale.ROOT).trim();
    }

    /** title holds every word of query (any order, any case, no accents needed); an empty query matches all. */
    static boolean matches(String title, String query) {
        String t = fold(title), q = fold(query);
        if (q.isEmpty())
            return true;
        for (String w : q.split("\\s+"))
            if (!w.isEmpty() && !t.contains(w))
                return false;
        return true;
    }

    static long lastPlayed(LibraryScreen.Entry e) {
        return e.record == null ? 0 : e.record.lastPlayed;
    }

    static long playMs(LibraryScreen.Entry e) {
        return e.record == null ? 0 : e.record.playMs;
    }

    static Comparator<LibraryScreen.Entry> byName() {
        return (a, b) -> {   // « Écran » with the E's, not after the Z's
            int c = fold(a.title()).compareTo(fold(b.title()));
            if (c == 0)
                c = a.title().compareToIgnoreCase(b.title());
            return c != 0 ? c : a.tid.compareTo(b.tid);
        };
    }

    static Comparator<LibraryScreen.Entry> comparator(Sort s) {
        Comparator<LibraryScreen.Entry> name = byName();
        switch (s) {
        case LAST_PLAYED:   // never played last, by name
            return Comparator.<LibraryScreen.Entry>comparingLong(e -> -lastPlayed(e)).thenComparing(name);
        case MOST_PLAYED:
            return Comparator.<LibraryScreen.Entry>comparingLong(e -> -playMs(e)).thenComparing(name);
        case CONSOLE:       // Xbox, GBA, PS1, N64 (the Console order), by name in each
            return Comparator.<LibraryScreen.Entry>comparingInt(e -> e.console().ordinal()).thenComparing(name);
        default:
            return name;
        }
    }

    /** The grid: the entries of the console filter that match the search, favourites first, each part sorted. */
    static List<LibraryScreen.Entry> apply(List<LibraryScreen.Entry> all, State s) {
        List<LibraryScreen.Entry> out = new ArrayList<>();
        for (LibraryScreen.Entry e : all)
            if ((s.console == null || e.console() == s.console) && matches(e.title(), s.query))
                out.add(e);
        Comparator<LibraryScreen.Entry> c = comparator(s.sort);
        out.sort(Comparator.<LibraryScreen.Entry>comparingInt(e -> s.favourite(e.tid) ? 0 : 1).thenComparing(c));
        return out;
    }

    /** The « Recently played » row: the games played, most recent first, at most max, any console. */
    static List<LibraryScreen.Entry> recent(List<LibraryScreen.Entry> all, int max) {
        List<LibraryScreen.Entry> out = new ArrayList<>();
        for (LibraryScreen.Entry e : all)
            if (lastPlayed(e) > 0)
                out.add(e);
        out.sort(comparator(Sort.LAST_PLAYED));
        return out.size() > max ? new ArrayList<>(out.subList(0, max)) : out;
    }

    /** The consoles of the entries, in the Console order (the filter's tabs after « All »). */
    static List<Console> consoles(List<LibraryScreen.Entry> all) {
        Set<Console> s = java.util.EnumSet.noneOf(Console.class);
        for (LibraryScreen.Entry e : all)
            s.add(e.console());
        return Collections.unmodifiableList(new ArrayList<>(s));
    }
}
