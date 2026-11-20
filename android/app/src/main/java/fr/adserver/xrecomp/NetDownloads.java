package fr.adserver.xrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The network library's downloads (XBA-321, docs « Network library »): one job per game the player asked for, in the
 * order asked, kept in <files>/net/downloads.json so a download survives the app being closed or killed (NetService
 * picks the queue up again). Plain Java over one file; one process writes it (the app's main process, where NetService
 * runs), under this class's lock.
 *
 * Where a game lands: <download dir>/<12 hex of the info hash>/<the torrent's path> (a multi-file torrent's files
 * below its name), the download dir being the app's own external files folder (<external files>/net: readable as a
 * path with no permission, scanned as one more library root, LibraryScanner.netRoot). A job's files stay invisible to
 * that scan until the job is done ({@link #incomplete}): a half-written image is never identified, and the game stays
 * « On the network » (Downloading) until it is whole.
 *
 * {"v":1, "jobs":[{"key", "source", "title", "files":[{"index", "path", "size"}], "state":"queued|running|failed|done",
 *   "done":0, "total":0, "rate":0, "peers":0, "error":"", "auto":true, "added_at":0}]}
 */
final class NetDownloads {
    static final String QUEUED = "queued", RUNNING = "running", FAILED = "failed", DONE = "done";

    static final class FileRef {
        /** The file's number in the torrent (TorrentMeta.File.index). */
        final int index;
        /** Its path inside the torrent (TorrentMeta.File.path, already checked). */
        final String path;
        final long size;

        FileRef(int index, String path, long size) {
            this.index = index;
            this.path = path;
            this.size = size;
        }
    }

    static final class Job {
        String key = "", source = "", title = "";
        /**
         * Where its paths start, relative to the download folder: « <12 hex> » or, for a multi-file torrent,
         * « <12 hex>/<torrent name> » (kept so the files stay hidden and deletable without the torrent).
         */
        String base = "";
        final List<FileRef> files = new ArrayList<>();
        String state = QUEUED;
        long done, total;
        int rate, peers;
        String error = "";
        /** Once downloaded, import it and Prepare it (Alex: « ça le télécharge puis ça fait tout comme il faut »). */
        boolean auto = true;
        long addedAt;

        boolean active() {
            return QUEUED.equals(state) || RUNNING.equals(state);
        }

        /** 0..100. */
        int percent() {
            return total <= 0 ? 0 : (int)Math.min(100, done * 100 / total);
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("key", key);
            o.put("source", source);
            o.put("title", title);
            o.put("base", base);
            JSONArray a = new JSONArray();
            for (FileRef f : files) {
                JSONObject j = new JSONObject();
                j.put("index", f.index);
                j.put("path", f.path);
                j.put("size", f.size);
                a.put(j);
            }
            o.put("files", a);
            o.put("state", state);
            o.put("done", done);
            o.put("total", total);
            o.put("rate", rate);
            o.put("peers", peers);
            o.put("error", error);
            o.put("auto", auto);
            o.put("added_at", addedAt);
            return o;
        }

        static Job fromJson(JSONObject o) {
            Job j = new Job();
            j.key = o.optString("key", "");
            j.source = o.optString("source", "");
            j.title = o.optString("title", "");
            String b = o.optString("base", "");
            j.base = safePath(b) ? b : "";
            JSONArray a = o.optJSONArray("files");
            for (int i = 0; a != null && i < a.length(); i++) {
                JSONObject f = a.optJSONObject(i);
                if (f == null)
                    continue;
                String p = f.optString("path", "");
                if (!safePath(p))
                    continue;   // never trust a path read back from disk more than the torrent's
                j.files.add(new FileRef(f.optInt("index", -1), p, f.optLong("size", 0)));
            }
            j.state = o.optString("state", FAILED);
            j.done = o.optLong("done", 0);
            j.total = o.optLong("total", 0);
            j.rate = o.optInt("rate", 0);
            j.peers = o.optInt("peers", 0);
            j.error = o.optString("error", "");
            j.auto = o.optBoolean("auto", true);
            j.addedAt = o.optLong("added_at", 0);
            return j;
        }
    }

    /** A torrent path made of components TorrentMeta takes (no « .. », no absolute path). */
    static boolean safePath(String p) {
        if (p == null || p.isEmpty() || p.startsWith("/"))
            return false;
        for (String c : p.split("/", -1))
            if (!TorrentMeta.goodComponent(c))
                return false;
        return true;
    }

    // ---- Where files land -------------------------------------------------------------------------------------------

    /** The folder a source's games land in: <download dir>/<12 hex of its info hash>. */
    static File sourceDir(File downloadDir, String sourceId) {
        return new File(downloadDir, sourceId.length() > 12 ? sourceId.substring(0, 12) : sourceId);
    }

    /**
     * Where a torrent file lands (libtorrent's save path is sourceDir; a multi-file torrent's files are below its
     * name). Null for a path that is not safe (never joined).
     */
    static File localFile(File downloadDir, String sourceId, TorrentMeta m, String path) {
        if (!safePath(path) || !TorrentMeta.goodComponent(m.name))
            return null;
        File base = sourceDir(downloadDir, sourceId);
        return m.multiFile ? new File(new File(base, m.name), path) : new File(base, path);
    }

    /** The base of a job of source id whose torrent is m (Job.base). */
    static String base(String id, TorrentMeta m) {
        String s = id.length() > 12 ? id.substring(0, 12) : id;
        return m.multiFile ? s + "/" + m.name : s;
    }

    /** Where a job's file lands, from the job alone; null without a base or for an unsafe path. */
    static File jobFile(File downloadDir, Job j, String path) {
        if (j.base.isEmpty() || !safePath(j.base) || !safePath(path))
            return null;
        return new File(new File(downloadDir, j.base), path);
    }

    // ---- The file ---------------------------------------------------------------------------------------------------

    static File file(File filesDir) {
        return new File(RemoteLibrary.dir(filesDir), "downloads.json");
    }

    private static final Object LOCK = new Object();

    static List<Job> read(File filesDir) {
        synchronized (LOCK) {
            List<Job> l = new ArrayList<>();
            File f = file(filesDir);
            if (!f.isFile())
                return l;
            try {
                JSONArray a = new JSONObject(PrepareProtocol.readText(f, 4 << 20)).optJSONArray("jobs");
                for (int i = 0; a != null && i < a.length(); i++) {
                    JSONObject o = a.optJSONObject(i);
                    if (o != null && !o.optString("key", "").isEmpty())
                        l.add(Job.fromJson(o));
                }
            } catch (IOException | JSONException e) {
                LibraryScanner.warn("net: unreadable " + f + ": " + e);
            }
            return l;
        }
    }

    static void write(File filesDir, List<Job> jobs) throws IOException {
        synchronized (LOCK) {
            try {
                JSONArray a = new JSONArray();
                for (Job j : jobs)
                    a.put(j.toJson());
                JSONObject o = new JSONObject();
                o.put("v", 1);
                o.put("jobs", a);
                Library.writeAtomically(file(filesDir), o.toString());
            } catch (JSONException e) {
                throw new IOException(e);
            }
        }
    }

    /** Changes the jobs under the lock: read, edit, write. */
    interface Edit {
        void apply(List<Job> jobs);
    }

    static List<Job> edit(File filesDir, Edit e) throws IOException {
        synchronized (LOCK) {
            List<Job> l = read(filesDir);
            e.apply(l);
            write(filesDir, l);
            return l;
        }
    }

    static Job find(List<Job> l, String key) {
        for (Job j : l)
            if (j.key.equals(key))
                return j;
        return null;
    }

    /**
     * Queues game g: a queued or running job of it is left as it is; a failed one, or a done one (the game is on the
     * network again: its local copy was removed), starts again. Returns the job.
     */
    static Job enqueue(File filesDir, RemoteLibrary.Game g, long now) throws IOException {
        Job[] out = new Job[1];
        edit(filesDir, l -> {
            Job j = find(l, g.key);
            if (j == null) {
                j = new Job();
                j.key = g.key;
                j.addedAt = now;
                l.add(j);
            } else if (!FAILED.equals(j.state) && !DONE.equals(j.state)) {
                out[0] = j;
                return;
            }
            j.source = g.source.id;
            j.title = g.title;
            TorrentMeta m = RemoteLibrary.meta(filesDir, g.source.id);
            j.base = m == null ? "" : base(g.source.id, m);
            j.files.clear();
            for (TorrentMeta.File f : g.files)
                j.files.add(new FileRef(f.index, f.path, f.size));
            j.state = QUEUED;
            j.done = 0;
            j.total = g.size();
            j.error = "";
            j.auto = true;
            out[0] = j;
        });
        return out[0];
    }

    /** Takes key's job out (cancelled by the player; its partial files are the caller's to delete). */
    static void cancel(File filesDir, String key) throws IOException {
        edit(filesDir, l -> l.removeIf(j -> j.key.equals(key)));
    }

    /** The jobs that still need the network: queued or running, in order. */
    static List<Job> pending(List<Job> l) {
        List<Job> out = new ArrayList<>();
        for (Job j : l)
            if (j.active())
                out.add(j);
        return out;
    }

    /** The keys whose download runs or waits (the library's Downloading chip). */
    static Set<String> activeKeys(List<Job> l) {
        Set<String> s = new HashSet<>();
        for (Job j : l)
            if (j.active())
                s.add(j.key);
        return s;
    }

    /**
     * The local paths the download folder's scan must not list yet: every file of a job that is not done (queued,
     * running, failed: a failed one may be half written). Absolute paths.
     */
    static Set<String> incomplete(File filesDir, File downloadDir) {
        Set<String> s = new HashSet<>();
        for (Job j : read(filesDir)) {
            if (DONE.equals(j.state))
                continue;
            for (FileRef f : j.files) {
                File lf = jobFile(downloadDir, j, f.path);
                if (lf != null)
                    s.add(lf.getAbsolutePath());
            }
        }
        return s;
    }
}
