package fr.adserver.xrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * What the library folders hold, as last scanned (LibraryScanner): <files>/library/index.json. Rebuildable: a missing
 * or corrupt file is an empty index and the next scan opens every image again. Plain Java and org.json.
 *
 * <pre>
 * {"v":1, "root":"file:/storage/emulated/0/xbox", "scanned_at":0, "truncated":false, "items":[
 *  {"loc":"file:/…/Halo.iso", "kind":"iso|folder", "name":"Halo.iso", "size":0, "mtime":0, "status":"ok",
 *   "tid":"4D530004", "title":"Halo", "region":4, "disc":0, "version":0, "xbe_sha256":"…",
 *   "xbe":"xbe/4D530004-0123abcd/default.xbe", "folder":"content://…/tree/primary%3Axbox"},
 *  {"loc":"file:/…/gba/Game.gba", "kind":"rom", "name":"Game.gba", "size":0, "mtime":0, "status":"ok",
 *   "console":"gba", "key":"AXVE-1A2B3C4D", "tid":"gba-AXVE-1A2B3C4D", "title":"Game", "folder":"…"}]}
 * </pre>
 * size and mtime are the scanned file's: the image itself, or a game folder's default.xbe (its fingerprint; a folder's
 * total size is not computed at scan); a .cue's: the .cue and its tracks summed (parts: the tracks), the newest time.
 * xbe is relative to <files>/library. folder: the tree URI of the library folder the item is in (Library.Folder; absent
 * in an index from before several folders: the first folder). console: absent for an Xbox game; a game of another
 * console (XBA-178) has its generic title key (Console.identify) and, as tid, its library key « <console>-<key> »
 * (Console.packDirName), so games() and games.json key every console the same way.
 */
final class LibraryIndex {
    static final String KIND_ISO = "iso", KIND_FOLDER = "folder";
    /** A game file of another console (Console: .gba, .bin / .cue / .iso of a PS1 disc, .z64 / .n64 / .v64). */
    static final String KIND_ROM = "rom";
    /**
     * Item statuses: only « ok » items are games; the rest is the « Other files » line. NOT_ROM: a file named like a
     * game of another console whose content gives no title key (not a game, or a CHD, not read yet). IGNORED: a lone
     * .bin that is no PlayStation disc (firmware, a dump: a name too common to call it a game); kept for the stat-only
     * rescan, shown nowhere.
     */
    static final String OK = "ok", NOT_XISO = "not_xiso", NO_XBE = "no_xbe", BAD_XBE = "bad_xbe",
            UNREADABLE = "unreadable", NOT_ROM = "not_rom", IGNORED = "ignored";

    String root = "";
    long scannedAt;
    /** The scan stopped at its entry cap: the folder holds more than the index lists. */
    boolean truncated;
    final List<Item> items = new ArrayList<>();

    static final class Item {
        String loc = "", kind = KIND_ISO, name = "";
        long size, mtime;
        String status = OK;
        String tid = "", title = "";
        int region, disc, version;
        String xbeSha256 = "", xbe = "";
        /** A split image (XBA-126): every part's loc in order (loc is the first); a .cue: its tracks; else empty. */
        final java.util.List<String> parts = new java.util.ArrayList<>();
        /** The console (XBOX for every item of an index from before XBA-178) and, for another one, its title key. */
        Console console = Console.XBOX;
        String key = "";
        /** The library folder it was found in: its tree URI (Library.Folder.tree); "" in an older index. */
        String folder = "";

        boolean ok() {
            return OK.equals(status);
        }

        /** A game of another console than the Xbox: listed, not playable yet (no Prepare, no launch). */
        boolean otherConsole() {
            return console != Console.XBOX;
        }

        /** The cached default.xbe, or null. */
        File xbeFile(File filesDir) {
            return xbe.isEmpty() ? null : new File(Library.dir(filesDir), xbe);
        }

        /** The same scanned file, unchanged: location, size and modification time. */
        boolean sameFingerprint(String loc, long size, long mtime) {
            return this.loc.equals(loc) && this.size == size && this.mtime == mtime;
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("loc", loc);
            o.put("kind", kind);
            o.put("name", name);
            o.put("size", size);
            o.put("mtime", mtime);
            o.put("status", status);
            if (!parts.isEmpty())
                o.put("parts", new org.json.JSONArray(parts));
            if (!folder.isEmpty())
                o.put("folder", folder);
            if (otherConsole()) {
                o.put("console", console.id);
                o.put("key", key);
            }
            if (ok()) {
                o.put("tid", tid);
                o.put("title", title);
                o.put("region", region);
                o.put("disc", disc);
                o.put("version", version);
                o.put("xbe_sha256", xbeSha256);
                o.put("xbe", xbe);
            }
            return o;
        }

        static Item fromJson(JSONObject o) {
            Item i = new Item();
            i.loc = o.optString("loc", "");
            i.kind = o.optString("kind", KIND_ISO);
            i.name = o.optString("name", "");
            i.size = o.optLong("size", 0);
            i.mtime = o.optLong("mtime", 0);
            i.status = o.optString("status", UNREADABLE);
            i.tid = o.optString("tid", "");
            i.title = o.optString("title", "");
            i.region = o.optInt("region", 0);
            i.disc = o.optInt("disc", 0);
            i.version = o.optInt("version", 0);
            i.xbeSha256 = o.optString("xbe_sha256", "");
            i.xbe = o.optString("xbe", "");
            org.json.JSONArray parts = o.optJSONArray("parts");
            for (int k = 0; parts != null && k < parts.length(); k++)
                i.parts.add(parts.optString(k));
            i.folder = o.optString("folder", "");
            Console c = Console.fromPack(o.optString("console", ""));
            i.console = c == null ? Console.XBOX : c;
            i.key = o.optString("key", "");
            if (c == null)
                i.status = UNREADABLE;   // a console this build does not know
            if (i.ok() && i.tid.isEmpty())
                i.status = i.otherConsole() ? NOT_ROM : BAD_XBE;
            return i;
        }
    }

    Item find(String loc) {
        for (Item i : items) {
            if (i.loc.equals(loc))
                return i;
        }
        return null;
    }

    /**
     * The games: ok items grouped by title ID (a title's versions, regions and discs are its variants), sorted by
     * title; each title's variants most recently modified first (the default active source).
     */
    Map<String, List<Item>> games() {
        Map<String, List<Item>> m = new LinkedHashMap<>();
        List<Item> ok = new ArrayList<>();
        for (Item i : items) {
            if (i.ok())
                ok.add(i);
        }
        ok.sort((a, b) -> {
            int c = a.title.compareToIgnoreCase(b.title);
            return c != 0 ? c : a.tid.compareTo(b.tid);
        });
        for (Item i : ok)
            m.computeIfAbsent(i.tid, k -> new ArrayList<>()).add(i);
        for (List<Item> l : m.values())
            l.sort((a, b) -> a.mtime != b.mtime ? Long.compare(b.mtime, a.mtime) : a.loc.compareTo(b.loc));
        return m;
    }

    /** The consoles of the games listed (ok items). */
    java.util.Set<Console> consoles() {
        java.util.Set<Console> s = java.util.EnumSet.noneOf(Console.class);
        for (Item i : items) {
            if (i.ok())
                s.add(i.console);
        }
        return s;
    }

    /**
     * Drops the items of a library folder taken out of the library (folder: its tree URI); first: it was the first
     * folder, which also owns the items of an index from before several folders (no folder field).
     */
    void dropFolder(String folder, boolean first) {
        items.removeIf(i -> i.folder.equals(folder) || (first && i.folder.isEmpty()));
    }

    /** The items that are not games (not an Xbox image, no default.xbe, a bad XBE, unreadable). */
    List<Item> others() {
        List<Item> l = new ArrayList<>();
        for (Item i : items) {
            if (!i.ok() && !IGNORED.equals(i.status))
                l.add(i);
        }
        return l;
    }

    /** The active variant: the record's source when it is still there, else the most recently modified one. */
    static Item active(List<Item> variants, String preferredLoc) {
        if (variants == null || variants.isEmpty())
            return null;
        if (preferredLoc != null && !preferredLoc.isEmpty()) {
            for (Item i : variants) {
                if (i.loc.equals(preferredLoc))
                    return i;
            }
        }
        Item best = null;
        for (Item i : variants) {
            if (best == null || i.mtime > best.mtime)
                best = i;
        }
        return best;
    }

    JSONObject toJson() throws JSONException {
        JSONObject o = new JSONObject();
        o.put("v", 1);
        o.put("root", root);
        o.put("scanned_at", scannedAt);
        o.put("truncated", truncated);
        JSONArray a = new JSONArray();
        for (Item i : items)
            a.put(i.toJson());
        o.put("items", a);
        return o;
    }

    static LibraryIndex fromJson(JSONObject o) {
        LibraryIndex x = new LibraryIndex();
        x.root = o.optString("root", "");
        x.scannedAt = o.optLong("scanned_at", 0);
        x.truncated = o.optBoolean("truncated", false);
        JSONArray a = o.optJSONArray("items");
        if (a != null) {
            for (int k = 0; k < a.length(); k++) {
                JSONObject i = a.optJSONObject(k);
                if (i != null && !i.optString("loc", "").isEmpty())
                    x.items.add(Item.fromJson(i));
            }
        }
        return x;
    }

    /** index.json, or an empty index when it is missing or corrupt (the next scan rebuilds it). */
    static LibraryIndex read(File f) {
        if (!f.isFile())
            return new LibraryIndex();
        try {
            return fromJson(new JSONObject(PrepareProtocol.readText(f, 16 << 20)));
        } catch (IOException | JSONException e) {
            return new LibraryIndex();
        }
    }

    static void write(File f, LibraryIndex x) throws IOException {
        try {
            Library.writeAtomically(f, x.toJson().toString());
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    /** « file:/abs/path »: the location of a file read as a path. */
    static String fileLoc(File f) {
        return "file:" + f.getAbsolutePath();
    }

    /** « doc:<documentId> »: the location of a document read through the Storage Access Framework. */
    static String docLoc(String documentId) {
        return "doc:" + documentId;
    }

    /** The path of a « file: » location, or null for another kind. */
    static File locFile(String loc) {
        return loc != null && loc.startsWith("file:") ? new File(loc.substring(5)) : null;
    }
}
