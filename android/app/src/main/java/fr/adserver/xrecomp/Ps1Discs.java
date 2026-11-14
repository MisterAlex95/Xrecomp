package fr.adserver.xrecomp;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The discs of a multi-disc PlayStation game (XBA-166), for the in-game menu's « Change disc »: from the disc being
 * played and the files beside it (a folder's listing, or the documents of the same folder of a library tree). Pure
 * Java: the activity lists the folder and opens the disc chosen (ShellActivity), this decides which files are discs of
 * the same game.
 *
 * <ul>
 *   <li>A .m3u playlist beside it that names the disc being played (one image per line, a path relative to the
 *       playlist or not: only its file name counts here, matched without case; blank lines and « # » comments
 *       skipped) gives the discs, in its order.</li>
 *   <li>Otherwise the disc images (.cue, .chd, .bin, .iso, .img) whose name is the same once the disc marker is taken
 *       out (« (Disc 1) », « Disc 2 », « [Disc 3] », « CD1 », « (CD 2) », « Disk 1 », « (Disc 1 of 2) », any case),
 *       one per disc number (the one being played for its own number, else .cue, .chd, .iso, .bin, .img), in disc
 *       number order. A .bin / .img that is the track of a .cue beside it (its name starts with the .cue's) is not a
 *       disc. The disc being played must have a marker.</li>
 * </ul>
 * Fewer than two discs: none (the menu shows no « Change disc »).
 */
final class Ps1Discs {
    private Ps1Discs() {
    }

    /** A disc: its file name, how to open it (a path, a document URI), its number (0 unknown), the one playing. */
    static final class Disc {
        final String name, spec;
        final int number;
        final boolean current;

        Disc(String name, String spec, int number, boolean current) {
            this.name = name;
            this.spec = spec;
            this.number = number;
            this.current = current;
        }

        @Override
        public String toString() {
            return name + (current ? " (current)" : "") + " -> " + spec;
        }
    }

    /** Reads a playlist's text (from its spec); IOException when it cannot. */
    interface Reader {
        String read(String spec) throws IOException;
    }

    /** The largest playlist read. */
    static final int M3U_MAX = 64 << 10;

    private static final Pattern MARKER = Pattern.compile(
            "[\\s_.-]*[(\\[]?\\s*(?<![a-z0-9])(?:disc|disk|cd)\\s*[_-]?\\s*(\\d{1,2})(?:\\s*(?:of|/)\\s*\\d{1,2})?\\s*[)\\]]?",
            Pattern.CASE_INSENSITIVE);
    private static final String[] IMAGE_EXT = { ".cue", ".chd", ".iso", ".bin", ".img" };

    static String ext(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? "" : name.substring(dot).toLowerCase(Locale.ROOT);
    }

    private static String stem(String name) {
        int dot = name.lastIndexOf('.');
        return dot < 0 ? name : name.substring(0, dot);
    }

    static boolean isImage(String name) {
        String e = ext(name);
        for (String x : IMAGE_EXT)
            if (x.equals(e))
                return true;
        return false;
    }

    /** The disc number in a file name (its last marker), or 0 without one. */
    static int number(String name) {
        Matcher m = MARKER.matcher(stem(name));
        int n = 0;
        while (m.find())
            n = Integer.parseInt(m.group(1));
        return n;
    }

    /** The name without its extension and its disc marker, lower case, spaces collapsed: what discs of one game share. */
    static String gameKey(String name) {
        String s = MARKER.matcher(stem(name)).replaceAll(" ");
        return s.toLowerCase(Locale.ROOT).replaceAll("[\\s_]+", " ").trim();
    }

    /** The file names a playlist's text names, in order (directories dropped). */
    static List<String> m3uNames(String text) {
        List<String> out = new ArrayList<>();
        if (text == null)
            return out;
        if (text.startsWith("﻿"))
            text = text.substring(1);
        for (String line : text.split("\r?\n|\r")) {
            String l = line.trim();
            if (l.isEmpty() || l.startsWith("#"))
                continue;
            int slash = Math.max(l.lastIndexOf('/'), l.lastIndexOf('\\'));
            out.add(slash >= 0 ? l.substring(slash + 1) : l);
        }
        return out;
    }

    private static int extRank(String name) {
        String e = ext(name);
        for (int i = 0; i < IMAGE_EXT.length; i++)
            if (IMAGE_EXT[i].equals(e))
                return i;
        return IMAGE_EXT.length;
    }

    /**
     * The discs of the game whose disc `current` (a file name) is playing, among siblings (file name -> spec, the
     * folder's files, the disc itself included); empty when fewer than two are found.
     */
    static List<Disc> find(String current, Map<String, String> siblings, Reader reader) {
        if (current == null || siblings == null)
            return Collections.emptyList();
        // 1: a playlist that names it
        for (Map.Entry<String, String> e : siblings.entrySet()) {
            if (!".m3u".equals(ext(e.getKey())) || reader == null)
                continue;
            String text;
            try {
                text = reader.read(e.getValue());
            } catch (IOException | RuntimeException ex) {
                continue;
            }
            List<String> names = m3uNames(text);
            boolean names_it = false;
            for (String n : names)
                names_it |= n.equalsIgnoreCase(current);
            if (!names_it)
                continue;
            List<Disc> out = new ArrayList<>();
            int i = 0;
            for (String n : names) {
                i++;
                String spec = lookup(siblings, n);
                if (spec != null)
                    out.add(new Disc(actualName(siblings, n), spec, i, n.equalsIgnoreCase(current)));
            }
            return out.size() >= 2 ? out : Collections.emptyList();
        }
        // 2: the same name but for the disc marker
        int curNo = number(current);
        if (curNo == 0)
            return Collections.emptyList();
        String key = gameKey(current);
        List<String> cues = new ArrayList<>();
        for (String n : siblings.keySet())
            if (".cue".equals(ext(n)))
                cues.add(stem(n).toLowerCase(Locale.ROOT));
        Map<Integer, String> best = new LinkedHashMap<>();
        for (String n : siblings.keySet()) {
            if (!isImage(n) || !gameKey(n).equals(key))
                continue;
            int no = number(n);
            if (no == 0)
                continue;
            String e = ext(n);
            if ((".bin".equals(e) || ".img".equals(e)) && !n.equalsIgnoreCase(current) && isTrack(n, cues))
                continue;
            String had = best.get(no);
            if (had == null || n.equalsIgnoreCase(current)
                || (!had.equalsIgnoreCase(current) && extRank(n) < extRank(had)))
                best.put(no, n);
        }
        if (best.size() < 2)
            return Collections.emptyList();
        List<Integer> nos = new ArrayList<>(best.keySet());
        Collections.sort(nos);
        List<Disc> out = new ArrayList<>();
        for (int no : nos) {
            String n = best.get(no);
            out.add(new Disc(n, siblings.get(n), no, no == curNo));
        }
        return out;
    }

    private static boolean isTrack(String bin, List<String> cueStems) {
        String s = stem(bin).toLowerCase(Locale.ROOT);
        for (String c : cueStems)
            if (s.startsWith(c))
                return true;
        return false;
    }

    private static String lookup(Map<String, String> siblings, String name) {
        String v = siblings.get(name);
        if (v != null)
            return v;
        for (Map.Entry<String, String> e : siblings.entrySet())
            if (e.getKey().equalsIgnoreCase(name))
                return e.getValue();
        return null;
    }

    private static String actualName(Map<String, String> siblings, String name) {
        for (String k : siblings.keySet())
            if (k.equalsIgnoreCase(name))
                return k;
        return name;
    }

    /**
     * The documents of the folder holding document docId, as the library index knows them (a fallback when the
     * folder cannot be listed): file name -> document ID, for every item and track (« doc: » locations) directly in
     * the same folder (document IDs « volume:dir/name »).
     */
    static Map<String, String> siblingsFromIndex(LibraryIndex index, String docId) {
        Map<String, String> out = new LinkedHashMap<>();
        if (index == null || docId == null)
            return out;
        int slash = docId.lastIndexOf('/');
        String dir = slash >= 0 ? docId.substring(0, slash + 1) : docId.substring(0, docId.indexOf(':') + 1);
        for (LibraryIndex.Item it : index.items) {
            List<String> locs = new ArrayList<>();
            locs.add(it.loc);
            locs.addAll(it.parts);
            for (String loc : locs) {
                if (!loc.startsWith("doc:"))
                    continue;
                String id = loc.substring(4);
                if (id.startsWith(dir) && id.indexOf('/', dir.length()) < 0 && id.length() > dir.length())
                    out.put(id.substring(dir.length()), id);
            }
        }
        return out;
    }
}
