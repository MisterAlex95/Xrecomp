package fr.adserver.xrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;

/**
 * The network library (docs/android/app-shell.md « Network library », XBA-319): the torrent sources the player added
 * and the games they hold, listed in the library before they are downloaded. Plain Java over files, so the JVM tests
 * cover it.
 *
 * Files, under <files>/net/ (never scanned, never in index.json: a rescan rewrites the index from the folders):
 * sources.json, the sources in the order they were added ({"v":1, "sources":[{"id":"<info hash>", "name", "console",
 * "magnet", "added_at"}]}, console "" = each file's own); <id>.torrent, the metadata (the .torrent the player picked,
 * or the one a magnet's metadata exchange gave) — a source without it is still « fetching ».
 *
 * A source's games ({@link #games}): every file a library folder's scan would list as a game, by name only (nothing
 * is read before the download: Alex, 2026-11-20, « on s'en fiche de l'image »): a disc image or a split set (one game,
 * its parts' sizes summed), a .cue with the tracks beside it, a lone .bin, a ROM. The console: the file's own when
 * its name says (Console.byName), else the source's, else the Xbox. The title: the file name cleaned
 * ({@link #cleanTitle}). The key: « [<console>-]net-<12 hex of the info hash>-<CRC32 of the path> », stable across
 * sessions and rescans, never a title ID ({@link #isRemoteKey}).
 */
final class RemoteLibrary {
    static final String KIND_TORRENT = "torrent";

    static final class Source {
        String id = "";
        String kind = KIND_TORRENT;
        String name = "";
        /** The console the player chose, or null: each file's own (by name; a plain .iso then counts as Xbox). */
        Console console;
        /** The magnet link it was added with, or "" (added from a .torrent file). */
        String magnet = "";
        long addedAt;

        /** The 12 hex digits of the info hash that keys its games. */
        String shortId() {
            return id.length() > 12 ? id.substring(0, 12) : id;
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("id", id);
            o.put("kind", kind);
            o.put("name", name);
            o.put("console", console == null ? "" : console.id);
            o.put("magnet", magnet);
            o.put("added_at", addedAt);
            return o;
        }

        static Source fromJson(JSONObject o) {
            Source s = new Source();
            s.id = o.optString("id", "");
            s.kind = o.optString("kind", KIND_TORRENT);
            s.name = o.optString("name", "");
            s.console = byId(o.optString("console", ""));
            s.magnet = o.optString("magnet", "");
            s.addedAt = o.optLong("added_at", 0);
            return s;
        }
    }

    /** One game of a source: its files in the torrent (the first one names it), its console, title and key. */
    static final class Game {
        final Source source;
        final String key;
        final Console console;
        final String title;
        /** The first file (the image, the first part, the .cue, the ROM), then the rest of the set. */
        final List<TorrentMeta.File> files;

        Game(Source source, Console console, List<TorrentMeta.File> files) {
            this.source = source;
            this.console = console;
            this.files = files;
            this.key = key(source, console, files.get(0).path);
            this.title = cleanTitle(files.get(0).name());
        }

        /** The first file's name, as a scan would record it (LibraryIndex.Item.name). */
        String name() {
            return files.get(0).name();
        }

        /** The sizes summed, as a scan records a split set or a .cue with its tracks. */
        long size() {
            long s = 0;
            for (TorrentMeta.File f : files)
                s += f.size;
            return s;
        }
    }

    // ---- Keys -------------------------------------------------------------------------------------------------------

    static String key(Source s, Console c, String path) {
        CRC32 crc = new CRC32();
        crc.update(path.getBytes(StandardCharsets.UTF_8));
        String k = "net-" + s.shortId() + "-" + String.format(Locale.ROOT, "%08x", crc.getValue());
        return c == null || c == Console.XBOX ? k : c.id + "-" + k;
    }

    /** A game key of the network library (any console). */
    static boolean isRemoteKey(String key) {
        if (key == null)
            return false;
        Console c = Console.ofKey(key);
        String rest = c == Console.XBOX ? key : key.substring(c.id.length() + 1);
        return rest.startsWith("net-");
    }

    static Console byId(String id) {
        for (Console c : Console.values())
            if (c.id.equals(id))
                return c;
        return null;
    }

    // ---- Games ------------------------------------------------------------------------------------------------------

    /** The games of source s, whose torrent is m, in torrent order. */
    static List<Game> games(Source s, TorrentMeta m) {
        List<Game> out = new ArrayList<>();
        Map<String, List<TorrentMeta.File>> byDir = new LinkedHashMap<>();
        for (TorrentMeta.File f : m.files)
            byDir.computeIfAbsent(f.dir(), d -> new ArrayList<>()).add(f);
        for (List<TorrentMeta.File> kids : byDir.values()) {
            Set<TorrentMeta.File> taken = new HashSet<>();
            boolean hasCue = false;
            for (TorrentMeta.File f : kids)
                hasCue |= LibraryScanner.isCue(f.name());
            for (TorrentMeta.File f : kids) {
                if (taken.contains(f))
                    continue;
                String n = f.name();
                if (n.startsWith("."))
                    continue;
                if (LibraryScanner.isCue(n)) {   // the .cue stands for the disc; its tracks are the .bin beside it
                    List<TorrentMeta.File> set = new ArrayList<>();
                    set.add(f);
                    String base = LibraryScanner.stripExtension(n);
                    for (TorrentMeta.File t : kids)
                        if (t != f && isTrack(t.name()) && t.name().startsWith(base))
                            set.add(t);
                    taken.addAll(set);
                    out.add(new Game(s, Console.PS1, set));
                } else if (LibraryScanner.isImage(n)) {
                    List<TorrentMeta.File> set = splitSet(kids, f);
                    if (set == null)
                        continue;   // a later part of a set listed with its first
                    taken.addAll(set);
                    out.add(new Game(s, consoleOf(s, n), set));
                } else if (LibraryScanner.isRom(n)) {
                    if (isTrack(n) && hasCue)
                        continue;   // a track of a .cue in this folder (named otherwise): not a game of its own
                    List<TorrentMeta.File> one = new ArrayList<>();
                    one.add(f);
                    out.add(new Game(s, Console.byName(n), one));
                }
            }
        }
        return out;
    }

    private static boolean isTrack(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".bin") || n.endsWith(".img");
    }

    /** A disc image's console: its name's (.xiso, .cso… are Xbox), else the source's, else the Xbox. */
    static Console consoleOf(Source s, String name) {
        Console c = Console.byName(name);
        if (c != null)
            return s.console != null && Console.isPlainIso(name) ? s.console : c;
        return s.console != null ? s.console : Console.XBOX;
    }

    /** f's split set among kids (LibraryScanner.splitSet by name); null when f is a later part of a complete set. */
    static List<TorrentMeta.File> splitSet(List<TorrentMeta.File> kids, TorrentMeta.File f) {
        List<TorrentMeta.File> one = new ArrayList<>();
        one.add(f);
        DiscImages.Part p = DiscImages.part(f.name());
        if (p == null)
            return one;
        int first = p.number == 0 ? 0 : 1;
        List<TorrentMeta.File> parts = new ArrayList<>();
        for (int n = first; n < first + DiscImages.MAX_PARTS; n++) {
            String want = p.name(n);
            TorrentMeta.File hit = null;
            for (TorrentMeta.File k : kids)
                if (k.name().equals(want))
                    hit = k;
            if (hit == null)
                break;
            parts.add(hit);
        }
        if (parts.size() < 2 || !parts.contains(f))
            return one;
        return parts.get(0) == f ? parts : null;
    }

    /**
     * A file name as a title: no extension (nor a split part's number), no bracketed tags (« (USA) », « [!] »,
     * « (Rev 1) »), underscores as spaces, spaces collapsed. « Halo (USA) (Rev 1).iso » → « Halo ». Nothing left: the
     * name without its extension.
     */
    static String cleanTitle(String fileName) {
        String n = fileName;
        DiscImages.Part p = DiscImages.part(n);
        if (p != null) {   // the set's base name: « Game.iso. », « Game.part », « Game. » before the number
            n = p.prefix.replaceAll("(?i)(\\.part)?\\.?$", "");
            if (DiscImages.isImageName(n))
                n = LibraryScanner.stripExtension(n);
        } else {
            n = LibraryScanner.stripExtension(n);
        }
        String t = n.replaceAll("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]", " ").replace('_', ' ').replaceAll("\\s+", " ").trim();
        return t.isEmpty() ? LibraryScanner.stripExtension(fileName) : t;
    }

    // ---- Files ------------------------------------------------------------------------------------------------------

    static File dir(File filesDir) {
        return new File(filesDir, "net");
    }

    static File sourcesFile(File filesDir) {
        return new File(dir(filesDir), "sources.json");
    }

    static File torrentFile(File filesDir, String id) {
        return new File(dir(filesDir), id + ".torrent");
    }

    /** The sources, in the order they were added; none when the file is missing or unreadable. */
    static List<Source> sources(File filesDir) {
        List<Source> l = new ArrayList<>();
        File f = sourcesFile(filesDir);
        if (!f.isFile())
            return l;
        try {
            JSONArray a = new JSONObject(PrepareProtocol.readText(f, 1 << 20)).optJSONArray("sources");
            for (int i = 0; a != null && i < a.length(); i++) {
                JSONObject o = a.optJSONObject(i);
                if (o == null)
                    continue;
                Source s = Source.fromJson(o);
                if (s.id.matches("[0-9a-f]{40}"))
                    l.add(s);
            }
        } catch (IOException | JSONException e) {
            LibraryScanner.warn("net: unreadable " + f + ": " + e);
        }
        return l;
    }

    static void writeSources(File filesDir, List<Source> l) throws IOException {
        try {
            JSONArray a = new JSONArray();
            for (Source s : l)
                a.put(s.toJson());
            JSONObject o = new JSONObject();
            o.put("v", 1);
            o.put("sources", a);
            dir(filesDir).mkdirs();
            Library.writeAtomically(sourcesFile(filesDir), o.toString());
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    /** s's metadata, or null while a magnet's is being fetched (or the file is damaged: it is fetched again). */
    static TorrentMeta meta(File filesDir, String id) {
        File f = torrentFile(filesDir, id);
        if (!f.isFile() || f.length() > MAX_TORRENT)
            return null;
        try {
            TorrentMeta m = TorrentMeta.parse(java.nio.file.Files.readAllBytes(f.toPath()));
            return m.infoHash.equals(id) ? m : null;
        } catch (IOException e) {
            LibraryScanner.warn("net: bad " + f + ": " + e);
            return null;
        }
    }

    /** The largest .torrent taken (a library of a few thousand files: well under a megabyte or two). */
    static final int MAX_TORRENT = 16 << 20;

    /**
     * Adds a source from a .torrent (its bytes): written as <id>.torrent; the same torrent again only updates its
     * console. Returns the source.
     */
    static Source addTorrent(File filesDir, byte[] torrent, Console console, long now) throws IOException {
        if (torrent.length > MAX_TORRENT)
            throw new IOException("torrent too large");
        TorrentMeta m = TorrentMeta.parse(torrent);
        dir(filesDir).mkdirs();
        Library.writeAtomically(torrentFile(filesDir, m.infoHash), torrent);
        List<Source> l = sources(filesDir);
        Source s = find(l, m.infoHash);
        if (s == null) {
            s = new Source();
            s.id = m.infoHash;
            s.addedAt = now;
            l.add(s);
        }
        s.name = m.name;
        s.console = console;
        writeSources(filesDir, l);
        return s;
    }

    /** Adds a source from a magnet link (its metadata comes later, {@link #saveMetadata}); null: not a magnet link. */
    static Source addMagnet(File filesDir, String magnet, Console console, long now) throws IOException {
        String id = TorrentMeta.magnetHash(magnet);
        if (id == null)
            return null;
        List<Source> l = sources(filesDir);
        Source s = find(l, id);
        if (s == null) {
            s = new Source();
            s.id = id;
            s.addedAt = now;
            s.name = TorrentMeta.magnetName(magnet);
            l.add(s);
        }
        s.magnet = magnet.trim();
        s.console = console;
        writeSources(filesDir, l);
        return s;
    }

    /**
     * The metadata a magnet's exchange gave (a whole .torrent, or a bare info dictionary): kept when its hash is the
     * source's; the source takes the torrent's name.
     */
    static boolean saveMetadata(File filesDir, String id, byte[] torrent) throws IOException {
        TorrentMeta m = TorrentMeta.parse(torrent);
        if (!m.infoHash.equals(id))
            return false;
        Library.writeAtomically(torrentFile(filesDir, id), torrent);
        List<Source> l = sources(filesDir);
        Source s = find(l, id);
        if (s != null) {
            s.name = m.name;
            writeSources(filesDir, l);
        }
        return true;
    }

    /** Takes source id out of the network library (its metadata too; downloaded games stay where they are). */
    static void remove(File filesDir, String id) throws IOException {
        List<Source> l = sources(filesDir);
        l.removeIf(s -> s.id.equals(id));
        writeSources(filesDir, l);
        torrentFile(filesDir, id).delete();
    }

    static Source find(List<Source> l, String id) {
        for (Source s : l)
            if (s.id.equals(id))
                return s;
        return null;
    }

    // ---- The library's view ------------------------------------------------------------------------------------------

    /**
     * Every source's games that are not on the device: a game whose name and size match a scanned item (a library
     * folder, or the download folder) is that local game now, and is left out. Keyed by game key, in source then
     * torrent order.
     */
    static Map<String, Game> notLocal(File filesDir, LibraryIndex local) {
        Set<String> here = new HashSet<>();
        if (local != null)
            for (LibraryIndex.Item i : local.items)
                here.add(i.name + "\u0000" + i.size);
        Map<String, Game> out = new LinkedHashMap<>();
        for (Source s : sources(filesDir)) {
            TorrentMeta m = meta(filesDir, s.id);
            if (m == null)
                continue;
            for (Game g : games(s, m))
                if (!here.contains(g.name() + "\u0000" + g.size()))
                    out.putIfAbsent(g.key, g);
        }
        return out;
    }
}
