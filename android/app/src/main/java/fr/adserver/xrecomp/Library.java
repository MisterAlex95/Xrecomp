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

    /** Preferences (GameStore.PREFS): the launched game's title ID (next to game_dir and title), the library folder. */
    static final String PREF_TITLE_ID = "title_id", PREF_TREE = "library_tree", PREF_PATH = "library_path",
            PREF_SCANNED_AT = "library_scanned_at";

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
        /** The key: 8 upper-case hex digits, or « local-xxxxxxxx » for an XBE without a title ID. */
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

    // ---- Library folder (preferences) -------------------------------------------------------------------------------

    /** The picked library folder's tree URI, or null. */
    static String folderTree(Context c) {
        String s = GameStore.prefs(c).getString(PREF_TREE, null);
        return s == null || s.isEmpty() ? null : s;
    }

    /** The library folder as a path (All files access), or null (no folder, or the SAF fallback). */
    static String folderPath(Context c) {
        String s = GameStore.prefs(c).getString(PREF_PATH, null);
        return s == null || s.isEmpty() ? null : s;
    }

    /** Remembers the folder: its tree URI and, when readable as files, its path (null: SAF). Clears the scan time. */
    static boolean setFolder(Context c, String treeUri, String path) {
        SharedPreferences.Editor e = GameStore.prefs(c).edit().putString(PREF_TREE, treeUri == null ? "" : treeUri)
                .remove(PREF_SCANNED_AT);
        if (path == null || path.isEmpty())
            e.remove(PREF_PATH);
        else
            e.putString(PREF_PATH, path);
        return e.commit();
    }

    static boolean clearFolder(Context c) {
        return GameStore.prefs(c).edit().remove(PREF_TREE).remove(PREF_PATH).remove(PREF_SCANNED_AT).commit();
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
