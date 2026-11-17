package fr.adserver.xrecomp;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;
import java.util.function.Supplier;

/**
 * Cloud saves (docs/android/app-shell.md « Cloud saves »): each game's saves as one zip (SaveArchive, one title) in
 * the app's hidden Google Drive folder, <key>.zip, with its content hash in the file's appProperties.
 *
 * Per game the device keeps the hash both sides agreed on at the last sync (the base). A sync compares this device's
 * hash, the cloud's and the base: only this device changed → upload; only the cloud changed → download (the device's
 * saves kept in save/.backup/<key>/ as an import does); both changed → a conflict, nothing moves until the player
 * picks a side. Saves deleted on this device stay in the cloud (no deletion goes up). A running game's saves are
 * never downloaded (pending until it ends; asked at each download, not once per sync); they may go up.
 *
 * State in <files>/cloud/state.json (the account's refresh token and email, « sync automatically », per game its base
 * and conflict), private to the app. One sync at a time (a static lock). Plain Java.
 */
final class CloudSaves {
    static final String DIR = "cloud", STATE = "state.json";
    static final String P_KEY = "xrecompKey", P_HASH = "hash", P_DEVICE = "device", P_AT = "at";
    static final String SUFFIX = ".zip";
    private static final Object LOCK = new Object();

    enum Action { NONE, UPLOAD, DOWNLOAD, CONFLICT }

    /** What to do with one game: local, remote and base are content hashes (null: none). */
    static Action decide(String local, String remote, String base) {
        if (local == null ? remote == null : local.equals(remote))
            return Action.NONE;
        if (remote == null)
            return Action.UPLOAD;
        if (local == null)
            return remote.equals(base) ? Action.NONE : Action.DOWNLOAD;   // deleted here: the cloud keeps its copy
        if (local.equals(base))
            return Action.DOWNLOAD;
        if (remote.equals(base))
            return Action.UPLOAD;
        return Action.CONFLICT;   // both changed, or both have saves and never synced
    }

    /** A conflict settled by the player. */
    enum Side { KEEP_DEVICE, KEEP_CLOUD }

    // ---- State -------------------------------------------------------------------------------------------------

    static final class State {
        String refresh = "", email = "";
        boolean auto = true;
        long lastSync;
        String lastError = "";
        final Map<String, String> base = new HashMap<>();
        final TreeSet<String> conflicts = new TreeSet<>();

        boolean connected() {
            return !refresh.isEmpty();
        }
    }

    static File stateFile(File filesDir) {
        return new File(new File(filesDir, DIR), STATE);
    }

    static State load(File filesDir) {
        State s = new State();
        File f = stateFile(filesDir);
        if (!f.isFile())
            return s;
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(f, 4 << 20));
            s.refresh = o.optString("refresh");
            s.email = o.optString("email");
            s.auto = o.optBoolean("auto", true);
            s.lastSync = o.optLong("lastSync");
            s.lastError = o.optString("lastError");
            JSONObject b = o.optJSONObject("base");
            if (b != null) {
                for (Iterator<String> k = b.keys(); k.hasNext();) {
                    String key = k.next();
                    s.base.put(key, b.optString(key));
                }
            }
            org.json.JSONArray c = o.optJSONArray("conflicts");
            for (int i = 0; c != null && i < c.length(); i++)
                s.conflicts.add(c.optString(i));
        } catch (IOException | JSONException e) {
            // unreadable: as if never connected (the cloud's files stay; connecting again syncs them)
        }
        return s;
    }

    static void save(File filesDir, State s) throws IOException {
        JSONObject o = new JSONObject();
        try {
            o.put("refresh", s.refresh);
            o.put("email", s.email);
            o.put("auto", s.auto);
            o.put("lastSync", s.lastSync);
            o.put("lastError", s.lastError);
            o.put("base", new JSONObject(s.base));
            o.put("conflicts", new org.json.JSONArray(s.conflicts));
        } catch (JSONException e) {
            throw new IOException(e);
        }
        File f = stateFile(filesDir);
        File p = f.getParentFile();
        if (p != null && !p.isDirectory() && !p.mkdirs())
            throw new IOException("cannot create " + p);
        Library.writeAtomically(f, o.toString());
    }

    // ---- Sync --------------------------------------------------------------------------------------------------

    /** One sync's outcome, per game. */
    static final class Report {
        final List<String> uploaded = new ArrayList<>(), downloaded = new ArrayList<>();
        final List<String> conflicts = new ArrayList<>(), pending = new ArrayList<>();
        final Map<String, String> failed = new LinkedHashMap<>();
        /** The account must connect again (Google refused the refresh token). */
        boolean signedOut;

        int moved() {
            return uploaded.size() + downloaded.size();
        }
    }

    final GoogleDrive drive;
    final Storage.Dirs dirs;
    final File filesDir;
    final String device;
    /**
     * Keys whose saves must not be replaced now (the running game's), asked again at each download: a sync lasts,
     * and a game can start meanwhile, so a list taken when the sync began is not enough.
     */
    final Supplier<Collection<String>> busy;
    private String access;
    private long accessUntil;

    CloudSaves(GoogleDrive drive, Storage.Dirs dirs, String device, Collection<String> busy) {
        this(drive, dirs, busy == null ? null : () -> busy, device);
    }

    /** busy asked again at each download. */
    static CloudSaves live(GoogleDrive drive, Storage.Dirs dirs, String device, Supplier<Collection<String>> busy) {
        return new CloudSaves(drive, dirs, busy, device);
    }

    private CloudSaves(GoogleDrive drive, Storage.Dirs dirs, Supplier<Collection<String>> busy, String device) {
        this.drive = drive;
        this.dirs = dirs;
        this.filesDir = dirs.filesDir;
        this.device = device == null ? "" : device;
        this.busy = busy == null ? Collections::emptySet : busy;
    }

    private boolean busy(String key) {
        Collection<String> b = busy.get();
        return b != null && b.contains(key);
    }

    /** Connected: the tokens of a finished sign-in are kept (any earlier account's bases are dropped). */
    static void connect(File filesDir, GoogleDrive.Tokens t) throws IOException {
        synchronized (LOCK) {
            State s = load(filesDir);
            boolean same = t.email != null && !t.email.isEmpty() && t.email.equals(s.email);
            if (!same) {
                s.base.clear();
                s.conflicts.clear();
                s.lastSync = 0;
            }
            s.refresh = t.refresh;
            s.email = t.email == null ? "" : t.email;
            s.lastError = "";
            save(filesDir, s);
        }
    }

    /** Disconnected: the grant revoked (best effort) and forgotten here; the cloud's files and local saves stay. */
    static void disconnect(File filesDir, GoogleDrive drive) throws IOException {
        String token;
        synchronized (LOCK) {
            State s = load(filesDir);
            token = s.refresh;
            s.refresh = "";
            s.lastError = "";
            save(filesDir, s);
        }
        if (drive != null && !token.isEmpty())
            drive.revoke(token);
    }

    static void setAuto(File filesDir, boolean on) throws IOException {
        synchronized (LOCK) {
            State s = load(filesDir);
            s.auto = on;
            save(filesDir, s);
        }
    }

    /** Syncs every game: this device's with saves, the cloud's, and those synced before. */
    Report syncAll(long nowMs) {
        return sync(null, null, nowMs);
    }

    /** Syncs one game (before it starts); side settles its conflict when not null. */
    Report syncOne(String key, Side side, long nowMs) {
        return sync(Collections.singletonList(key), side, nowMs);
    }

    private Report sync(List<String> only, Side side, long nowMs) {
        Report rep = new Report();
        synchronized (LOCK) {
            State s = load(filesDir);
            if (!s.connected() || !drive.configured())
                return rep;
            try {
                Map<String, GoogleDrive.RemoteFile> remote = new HashMap<>();
                for (GoogleDrive.RemoteFile f : withToken(s, a -> drive.list(a))) {
                    String k = keyOf(f);
                    if (k != null && !remote.containsKey(k))
                        remote.put(k, f);
                }
                TreeSet<String> keys = new TreeSet<>();
                if (only != null) {
                    keys.addAll(only);
                } else {
                    keys.addAll(SaveArchive.titlesWithSaves(dirs, new Library.Games()));
                    keys.addAll(remote.keySet());
                    keys.addAll(s.base.keySet());
                }
                for (String k : keys) {
                    if (!SaveCaches.safeName(k))
                        continue;
                    try {
                        syncKey(s, k, remote.get(k), only != null ? side : null, rep);
                    } catch (GoogleDrive.AuthException e) {
                        throw e;
                    } catch (IOException | RuntimeException e) {
                        rep.failed.put(k, String.valueOf(e.getMessage()));
                    }
                }
                s.lastSync = nowMs;
                s.lastError = rep.failed.isEmpty() ? "" : rep.failed.values().iterator().next();
            } catch (GoogleDrive.AuthException e) {
                rep.signedOut = true;
                s.refresh = "";
                s.lastError = "signed out: " + e.getMessage();
            } catch (IOException | RuntimeException e) {
                rep.failed.put("", String.valueOf(e.getMessage()));
                s.lastError = String.valueOf(e.getMessage());
            }
            try {
                save(filesDir, s);
            } catch (IOException e) {
                rep.failed.put("", "state: " + e.getMessage());
            }
        }
        return rep;
    }

    /** The key of a file of ours: its appProperties' key, else its name without .zip; null when not a save. */
    static String keyOf(GoogleDrive.RemoteFile f) {
        String k = f.prop(P_KEY);
        if (k == null && f.name != null && f.name.endsWith(SUFFIX))
            k = f.name.substring(0, f.name.length() - SUFFIX.length());
        return k != null && SaveCaches.safeName(k) && f.prop(P_HASH) != null ? k : null;
    }

    private void syncKey(State s, String key, GoogleDrive.RemoteFile rf, Side side, Report rep) throws IOException {
        String local = SaveArchive.hash(dirs, key);
        String remote = rf == null ? null : rf.prop(P_HASH);
        String base = s.base.get(key);
        Action a = decide(local, remote, base);
        if (a == Action.CONFLICT && side != null)
            a = side == Side.KEEP_DEVICE ? Action.UPLOAD : Action.DOWNLOAD;
        if (a == Action.UPLOAD && local == null)   // keep this device's « nothing »: the cloud's copy stays
            a = Action.NONE;
        switch (a) {
        case NONE:
            if (local != null && local.equals(remote))
                s.base.put(key, local);
            s.conflicts.remove(key);
            break;
        case CONFLICT:
            s.conflicts.add(key);
            rep.conflicts.add(key);
            break;
        case UPLOAD: {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            SaveArchive.export(dirs, Collections.singletonList(key), null, "", out);
            Map<String, String> props = new HashMap<>();
            props.put(P_KEY, key);
            props.put(P_HASH, local);
            props.put(P_DEVICE, device);
            props.put(P_AT, Long.toString(System.currentTimeMillis()));
            byte[] zip = out.toByteArray();
            String id = rf == null ? null : rf.id;
            withToken(s, t -> drive.upload(t, id, key + SUFFIX, props, zip));
            s.base.put(key, local);
            s.conflicts.remove(key);
            rep.uploaded.add(key);
            break;
        }
        case DOWNLOAD: {
            if (busy(key)) {
                rep.pending.add(key);
                break;
            }
            byte[] zip = withToken(s, t -> drive.download(t, rf.id));
            if (busy(key)) {   // its game started during the download: its folder is not renamed under it
                rep.pending.add(key);
                break;
            }
            SaveArchive.Unpacked u = SaveArchive.unpack(dirs, new ByteArrayInputStream(zip));
            SaveArchive.install(dirs, u, Collections.singletonList(key));
            // The base is the cloud's hash: should this device's differ after all, its next sync uploads it.
            s.base.put(key, remote);
            s.conflicts.remove(key);
            rep.downloaded.add(key);
            break;
        }
        }
    }

    private interface Call<T> {
        T run(String access) throws IOException;
    }

    /** Runs a Drive call with a valid access token: refreshed when old, and once more after a 401. */
    private <T> T withToken(State s, Call<T> c) throws IOException {
        long now = System.currentTimeMillis();
        if (access == null || now > accessUntil - 60_000)
            refresh(s, now);
        try {
            return c.run(access);
        } catch (GoogleDrive.TokenExpired e) {
            refresh(s, System.currentTimeMillis());
            return c.run(access);
        }
    }

    private void refresh(State s, long now) throws IOException {
        GoogleDrive.Tokens t = drive.refresh(s.refresh, now);
        access = t.access;
        accessUntil = t.expiresAtMs;
    }
}
