package fr.adserver.xrecomp;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The game library's records (docs: « Game library »): the games the launcher knows, one per title ID, in
 * <files>/library/games.json, written atomically (temporary file, then rename). The scan index (LibraryIndex,
 * index.json) says what the library folder holds and can be rebuilt; this file says what the player did (imported,
 * played, how long) and is the launcher's only.
 *
 * <pre>
 * {"v":1, "selected":"4D5300D1", "games":{"4D5300D1":{"title":"…", "source":"file:/…iso", "data_dir":"/…/files/game",
 *  "data_kind":"imported|in_place|cfg", "xbe_sha256":"…", "added_at":0, "last_played":0, "play_ms":0}}}
 * </pre>
 * « selected » is the game the launcher shows; the preferences game_dir / title / title_id (GameStore) keep meaning the
 * game the process runs or last ran, written only at launch.
 *
 * Plain Java and org.json except migrate(Context) and the library folder preferences.
 */
final class Library {
    private Library() {}

    static final String TAG = "xboxrecomp";
    static final int VERSION = 1;
    static final String DIR = "library", GAMES = "games.json", INDEX = "index.json", XBE_DIR = "xbe",
            CACHE_OWNER = "cache_owner";
    /** <external files>/games/<TID>: where an import extracts a game. */
    static final String GAMES_SUBDIR = "games";

    static final String KIND_IMPORTED = "imported", KIND_IN_PLACE = "in_place", KIND_CFG = "cfg";
    /** A disc image read in place (XBA-126): data_dir is its companion folder (DiscImages). */
    static final String KIND_IMAGE = "image";

    /**
     * Preferences (GameStore.PREFS): the launched game's title ID (next to game_dir and title), the library folders
     * (library_folders, XBA-178; library_tree / library_path: the one folder before, migrated as the first entry).
     */
    static final String PREF_TITLE_ID = "title_id", PREF_TREE = "library_tree", PREF_PATH = "library_path",
            PREF_FOLDERS = "library_folders", PREF_SCANNED_AT = "library_scanned_at";

    static File dir(File filesDir) {
        return new File(filesDir, DIR);
    }

    static File gamesFile(File filesDir) {
        return new File(dir(filesDir), GAMES);
    }

    static File indexFile(File filesDir) {
        return new File(dir(filesDir), INDEX);
    }

    /** files/library/xbe: one default.xbe per <TID>-<sha8> (art, theme, hash, the Prepare's source before import). */
    static File xbeCacheDir(File filesDir) {
        return new File(dir(filesDir), XBE_DIR);
    }

    static File cacheOwnerFile(File filesDir) {
        return new File(dir(filesDir), CACHE_OWNER);
    }

    /** <external files>/games/<TID>, the import target of a title. */
    static File importDir(File externalFilesDir, String titleId) {
        return new File(new File(externalFilesDir, GAMES_SUBDIR), titleId);
    }

    // ---- Records --------------------------------------------------------------------------------------------------

    static final class Record {
        /**
         * The key: 8 upper-case hex digits, or « local-xxxxxxxx » for an XBE without a title ID; a game of another
         * console: « <console>-<title key> » (Console.packDirName, XBA-178).
         */
        String titleId = "";
        String title = "";
        /** The scan location of the active source (LibraryIndex.Item.loc: « file:/… », « doc:… »), "" for none. */
        String source = "";
        /** Where the game's files are, absolute; "" until imported (an in-place folder: the folder itself). */
        String dataDir = "";
        String dataKind = "";
        String xbeSha256 = "";
        long addedAt, lastPlayed, playMs;

        /** data_dir is set and holds a default.xbe. */
        boolean hasData() {
            if (dataDir.isEmpty() || GameStore.findXbe(new File(dataDir)) == null)
                return false;
            // A disc image read in place by path: the image must still be there (SD card in). By document: known
            // only when the game opens it (the shell then reports no game data).
            DiscImages.Disc d = KIND_IMAGE.equals(dataKind) ? DiscImages.read(new File(dataDir)) : null;
            return d == null || !d.viaPath() || new File(d.path).isFile();
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("title", title);
            o.put("source", source);
            o.put("data_dir", dataDir);
            o.put("data_kind", dataKind);
            o.put("xbe_sha256", xbeSha256);
            o.put("added_at", addedAt);
            o.put("last_played", lastPlayed);
            o.put("play_ms", playMs);
            return o;
        }

        static Record fromJson(String titleId, JSONObject o) {
            Record r = new Record();
            r.titleId = titleId;
            r.title = o.optString("title", "");
            r.source = o.optString("source", "");
            r.dataDir = o.optString("data_dir", "");
            r.dataKind = o.optString("data_kind", "");
            r.xbeSha256 = o.optString("xbe_sha256", "");
            r.addedAt = o.optLong("added_at", 0);
            r.lastPlayed = o.optLong("last_played", 0);
            r.playMs = Math.max(0, o.optLong("play_ms", 0));
            return r;
        }
    }

    /** games.json in memory. Not thread-safe: the launcher's main thread owns it. */
    static final class Games {
        String selected = "";
        final LinkedHashMap<String, Record> games = new LinkedHashMap<>();

        Record get(String titleId) {
            return titleId == null ? null : games.get(titleId);
        }

        /** The selected record, else the most recently played one, else null. */
        Record selected() {
            Record r = games.get(selected);
            if (r != null)
                return r;
            for (Record x : games.values()) {
                if (r == null || x.lastPlayed > r.lastPlayed)
                    r = x;
            }
            return r;
        }

        void put(Record r) {
            games.put(r.titleId, r);
        }

        Record remove(String titleId) {
            if (titleId != null && titleId.equals(selected))
                selected = "";
            return games.remove(titleId);
        }

        /** Most recently played first, then by title. */
        List<Record> byLastPlayed() {
            List<Record> l = new ArrayList<>(games.values());
            l.sort((a, b) -> a.lastPlayed != b.lastPlayed ? Long.compare(b.lastPlayed, a.lastPlayed)
                                                          : a.title.compareToIgnoreCase(b.title));
            return l;
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("v", VERSION);
            o.put("selected", selected);
            JSONObject g = new JSONObject();
            for (Map.Entry<String, Record> e : games.entrySet())
                g.put(e.getKey(), e.getValue().toJson());
            o.put("games", g);
            return o;
        }

        static Games fromJson(JSONObject o) {
            Games g = new Games();
            g.selected = o.optString("selected", "");
            JSONObject m = o.optJSONObject("games");
            if (m != null) {
                for (Iterator<String> it = m.keys(); it.hasNext(); ) {
                    String k = it.next();
                    JSONObject r = m.optJSONObject(k);
                    if (r != null && !k.isEmpty())
                        g.games.put(k, Record.fromJson(k, r));
                }
            }
            return g;
        }
    }

    /**
     * games.json, or an empty library when it is missing. A file that cannot be parsed is kept aside as
     * games.json.bad (never silently lost) and the library starts empty; migrate() then rebuilds the current game.
     */
    static Games read(File gamesFile) {
        if (!gamesFile.isFile())
            return new Games();
        try {
            return Games.fromJson(new JSONObject(PrepareProtocol.readText(gamesFile, 4 << 20)));
        } catch (IOException | JSONException e) {
            File bad = new File(gamesFile.getPath() + ".bad");
            //noinspection ResultOfMethodCallIgnored
            bad.delete();
            //noinspection ResultOfMethodCallIgnored
            gamesFile.renameTo(bad);
            return new Games();
        }
    }

    static void write(File gamesFile, Games g) throws IOException {
        try {
            writeAtomically(gamesFile, g.toJson().toString(1));
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    static void writeAtomically(File file, String text) throws IOException {
        File dir = file.getParentFile();
        if (dir != null && !(dir.mkdirs() || dir.isDirectory()))
            throw new IOException("cannot create " + dir);
        File tmp = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        if (!tmp.renameTo(file))
            throw new IOException("cannot rename " + tmp + " to " + file);
    }

    /** « local-xxxxxxxx »: the key of a game whose XBE has no title ID, from a hash of its path. */
    static String localKey(String path) {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(path.getBytes(StandardCharsets.UTF_8));
            return String.format(Locale.ROOT, "local-%02x%02x%02x%02x", d[0] & 0xff, d[1] & 0xff, d[2] & 0xff,
                                 d[3] & 0xff);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static boolean isLocalKey(String key) {
        return key != null && key.startsWith("local-");
    }

    // ---- Play time --------------------------------------------------------------------------------------------------

    /** A session of titleId started at start: last_played = start. */
    static void sessionStart(Games g, String titleId, long start) {
        Record r = g.get(titleId);
        if (r != null)
            r.lastPlayed = start;
    }

    /** Closes a session of titleId: adds its length (at most GameStore.SESSION_MAX) to play_ms. */
    static void sessionEnd(Games g, String titleId, long start, long now) {
        Record r = g.get(titleId);
        if (r == null || start <= 0)
            return;
        r.playMs += Math.max(0, Math.min(GameStore.SESSION_MAX, now - start));
    }

    // ---- Migration (one game, no library yet) -----------------------------------------------------------------------

    /** What a launcher without the library knew: its preferences, recomp.cfg, the default import folder. */
    static final class Legacy {
        /** GameStore.chosenDir: the picked folder or the imported copy. */
        String chosenDir;
        String chosenTitle = "";
        /** RECOMP_GAME_DIR from recomp.cfg. */
        String cfgDir;
        /** <external files>/game, the only import target before the library. */
        File defaultGameDir;
        long lastPlayed, playMs;
    }

    /**
     * The record of the game a launcher without the library would run, resolved as it did (LauncherActivity.gameDir:
     * the chosen folder, else RECOMP_GAME_DIR, else <external files>/game when it holds a default.xbe); null when there
     * is none. Moves nothing.
     */
    static Record legacyRecord(Legacy l, long now) {
        String dir = null, kind = null;
        if (l.chosenDir != null && !l.chosenDir.isEmpty()) {
            dir = l.chosenDir;
            kind = sameFile(new File(dir), l.defaultGameDir) ? KIND_IMPORTED : KIND_IN_PLACE;
        } else if (l.cfgDir != null && !l.cfgDir.isEmpty()) {
            dir = l.cfgDir;
            kind = KIND_CFG;
        } else if (l.defaultGameDir != null && GameStore.findXbe(l.defaultGameDir) != null) {
            dir = l.defaultGameDir.getAbsolutePath();
            kind = KIND_IMPORTED;
        }
        if (dir == null)
            return null;
        File xbe = GameStore.findXbe(new File(dir));
        XbeInfo info = XbeInfo.parse(xbe);
        Record r = new Record();
        r.titleId = info != null ? info.titleId : localKey(new File(dir).getAbsolutePath());
        r.title = l.chosenTitle != null && !l.chosenTitle.isEmpty() ? l.chosenTitle
                  : info != null && !info.title.isEmpty() ? info.title : new File(dir).getName();
        r.dataDir = dir;
        r.dataKind = kind;
        r.source = KIND_IN_PLACE.equals(kind) ? LibraryIndex.fileLoc(new File(dir)) : "";
        if (xbe != null) {
            try {
                r.xbeSha256 = GameStore.sha256(xbe);
            } catch (IOException ignored) {
            }
        }
        r.addedAt = now;
        r.lastPlayed = l.lastPlayed;
        r.playMs = l.playMs;
        return r;
    }

    /**
     * Creates games.json from the one-game state when it is absent; idempotent (an existing games.json is returned
     * as it is). Writes nothing when there is no game (a later call, once one is chosen, migrates it). The record
     * becomes the selected game and the owner of the save caches (SaveCaches). Returns the library.
     */
    static Games migrate(File filesDir, Legacy l, long now) throws IOException {
        File f = gamesFile(filesDir);
        if (f.isFile()) {
            Games g = read(f);
            if (f.isFile())
                return g;   // (a corrupt file was moved aside by read(): migrate again)
        }
        Games g = new Games();
        Record r = legacyRecord(l, now);
        if (r == null)
            return g;
        g.put(r);
        g.selected = r.titleId;
        write(f, g);
        File owner = cacheOwnerFile(filesDir);
        if (!owner.isFile())
            SaveCaches.writeOwner(owner, r.titleId);
        return g;
    }

    /** migrate() with the app's state; also writes the launched game's title ID to the preferences. */
    static Games migrate(Context c) {
        SharedPreferences p = GameStore.prefs(c);
        boolean had = gamesFile(c.getFilesDir()).isFile();
        Legacy l = new Legacy();
        l.chosenDir = GameStore.chosenDir(c);
        l.chosenTitle = GameStore.chosenTitle(c);
        l.cfgDir = GameStore.cfgGameDir(c);
        l.defaultGameDir = GameStore.defaultGameDir(c);
        l.lastPlayed = GameStore.lastPlayed(c);
        l.playMs = GameStore.playTime(c);
        try {
            Games g = migrate(c.getFilesDir(), l, System.currentTimeMillis());
            Record r = g.selected();
            if (!had && r != null) {
                if (!isLocalKey(r.titleId))
                    p.edit().putString(PREF_TITLE_ID, r.titleId).commit();
                Log.i(TAG, "launcher: library: migrated " + r.titleId + " (" + r.dataDir + ")");
            }
            return g;
        } catch (IOException e) {
            Log.w(TAG, "launcher: library: migration failed", e);
            return read(gamesFile(c.getFilesDir()));
        }
    }

    // ---- Library folders (preferences) ------------------------------------------------------------------------------

    /**
     * One library folder (XBA-178: one per console, say): its tree URI (the picker's, whose read permission persists;
     * also the folder's identity, LibraryIndex.Item.folder) and, when it can be read as files with All files access,
     * its path (null: through the Storage Access Framework).
     */
    static final class Folder {
        final String tree, path;

        Folder(String tree, String path) {
            this.tree = tree == null ? "" : tree;
            this.path = path == null || path.isEmpty() ? null : path;
        }

        /** The folder's name for the Options sheet: the path's last part, else the tree's (« primary:gba » → gba). */
        String name() {
            if (path != null)
                return new File(path).getName();
            String t = tree;
            try {
                t = java.net.URLDecoder.decode(tree, "UTF-8");
            } catch (java.io.UnsupportedEncodingException | IllegalArgumentException ignored) {
            }
            int cut = Math.max(t.lastIndexOf('/'), t.lastIndexOf(':'));
            return cut >= 0 && cut + 1 < t.length() ? t.substring(cut + 1) : t;
        }
    }

    /** library_folders: a JSON array of {"tree", "path"} (path absent: SAF); unreadable or absent: null. */
    static List<Folder> parseFolders(String json) {
        if (json == null)
            return null;
        try {
            org.json.JSONArray a = new org.json.JSONArray(json);
            List<Folder> l = new ArrayList<>();
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.optJSONObject(i);
                if (o != null && !o.optString("tree", "").isEmpty())
                    l.add(new Folder(o.optString("tree", ""), o.optString("path", "")));
            }
            return l;
        } catch (JSONException e) {
            return null;
        }
    }

    static String foldersJson(List<Folder> l) {
        org.json.JSONArray a = new org.json.JSONArray();
        try {
            for (Folder f : l) {
                JSONObject o = new JSONObject();
                o.put("tree", f.tree);
                if (f.path != null)
                    o.put("path", f.path);
                a.put(o);
            }
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
        return a.toString();
    }

    /**
     * The library folders from the preferences' values: library_folders when set; else the one folder of a launcher
     * from before several folders (library_tree, library_path) as the only entry; else none.
     */
    static List<Folder> folders(String foldersJson, String oldTree, String oldPath) {
        List<Folder> l = parseFolders(foldersJson);
        if (l != null)
            return l;
        l = new ArrayList<>();
        if (oldTree != null && !oldTree.isEmpty())
            l.add(new Folder(oldTree, oldPath));
        return l;
    }

    /** The library folders, in the order they were added; the old single folder migrates (written once) as the first. */
    static List<Folder> folders(Context c) {
        SharedPreferences p = GameStore.prefs(c);
        String json = p.getString(PREF_FOLDERS, null);
        List<Folder> l = folders(json, p.getString(PREF_TREE, null), p.getString(PREF_PATH, null));
        if (json == null && !l.isEmpty()) {
            p.edit().putString(PREF_FOLDERS, foldersJson(l)).remove(PREF_TREE).remove(PREF_PATH).commit();
            Log.i(TAG, "launcher: library: folder " + l.get(0).tree + " migrated to library_folders");
        }
        return l;
    }

    static boolean setFolders(Context c, List<Folder> l) {
        return GameStore.prefs(c).edit().putString(PREF_FOLDERS, foldersJson(l)).remove(PREF_TREE).remove(PREF_PATH)
                .remove(PREF_SCANNED_AT).commit();
    }

    /** l with f added at the end; the same tree again replaces its entry in place (its path may have changed). */
    static List<Folder> withFolder(List<Folder> l, Folder f) {
        List<Folder> out = new ArrayList<>(l);
        for (int i = 0; i < out.size(); i++) {
            if (out.get(i).tree.equals(f.tree)) {
                out.set(i, f);
                return out;
            }
        }
        out.add(f);
        return out;
    }

    /** Adds a folder (or updates the one with this tree). Clears the scan time. */
    static boolean addFolder(Context c, String treeUri, String path) {
        return setFolders(c, withFolder(folders(c), new Folder(treeUri, path)));
    }

    /** Takes the folder with this tree out of the library (nothing on disk is touched). */
    static boolean removeFolder(Context c, String treeUri) {
        List<Folder> l = new ArrayList<>(folders(c));
        l.removeIf(f -> f.tree.equals(treeUri));
        return setFolders(c, l);
    }

    /**
     * The folder an index item was found in: its folder field's, else (an index from before several folders) the
     * first folder; null when that folder is no longer in the library.
     */
    static Folder folderOf(List<Folder> folders, LibraryIndex.Item i) {
        if (i == null || folders.isEmpty())
            return null;
        if (i.folder.isEmpty())
            return folders.get(0);
        for (Folder f : folders)
            if (f.tree.equals(i.folder))
                return f;
        return null;
    }

    /**
     * The library folders the app cannot read now (LibraryScreen.Env.noAccess): a path without All files access, a
     * tree whose read permission is gone. Their tree URIs, and "" when the first is one of them (it owns the items of
     * an index from before several folders).
     */
    static java.util.Set<String> unreadableFolders(Context c, boolean allFilesAccess) {
        java.util.Set<String> granted = new java.util.HashSet<>();
        for (android.content.UriPermission p : c.getContentResolver().getPersistedUriPermissions()) {
            if (p.isReadPermission())
                granted.add(p.getUri().toString());
        }
        java.util.Set<String> out = new java.util.HashSet<>();
        List<Folder> l = folders(c);
        for (int i = 0; i < l.size(); i++) {
            Folder f = l.get(i);
            if (f.path != null ? allFilesAccess : granted.contains(f.tree))
                continue;
            out.add(f.tree);
            if (i == 0)
                out.add("");
        }
        return out;
    }

    /** The first library folder's tree URI, or null (no folder): « a library folder is set ». */
    static String folderTree(Context c) {
        List<Folder> l = folders(c);
        return l.isEmpty() ? null : l.get(0).tree;
    }

    /** The first library folder as a path (All files access), or null (no folder, or the SAF fallback). */
    static String folderPath(Context c) {
        List<Folder> l = folders(c);
        return l.isEmpty() ? null : l.get(0).path;
    }

    /** Some library folder is read as files (a path): the library rescans each time it opens. */
    static boolean anyFolderPath(Context c) {
        for (Folder f : folders(c))
            if (f.path != null)
                return true;
        return false;
    }

    /** Makes this the only library folder (its tree URI and, when readable as files, its path). Clears the scan time. */
    static boolean setFolder(Context c, String treeUri, String path) {
        List<Folder> l = new ArrayList<>();
        if (treeUri != null && !treeUri.isEmpty())
            l.add(new Folder(treeUri, path));
        return setFolders(c, l);
    }

    static boolean clearFolder(Context c) {
        return setFolders(c, new ArrayList<>());
    }

    static long scannedAt(Context c) {
        return GameStore.prefs(c).getLong(PREF_SCANNED_AT, 0);
    }

    static boolean setScannedAt(Context c, long when) {
        return GameStore.prefs(c).edit().putLong(PREF_SCANNED_AT, when).commit();
    }

    private static boolean sameFile(File a, File b) {
        if (a == null || b == null)
            return false;
        try {
            return a.getCanonicalFile().equals(b.getCanonicalFile());
        } catch (IOException e) {
            return a.getAbsoluteFile().equals(b.getAbsoluteFile());
        }
    }
}
