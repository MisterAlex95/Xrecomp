package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * What NetService does at each tick (XBA-321), in plain Java over a NetEngine, so the JVM tests drive it with a fake
 * engine: the first queued or running download runs (one at a time), every other torrent waits paused; its progress,
 * rate and peers go into its job (NetDownloads); once all its files are whole it is DONE and its torrent pauses (no
 * seeding before NET-4 says how). The launcher sees DONE, rescans the download folder and goes on with the game.
 *
 * Errors a job carries while it waits or runs (the launcher words them): {@link #E_WIFI} (only on Wi-Fi, and the
 * network is metered), {@link #E_NO_PEERS} (no peer for {@link #NO_PEERS_MS}: nobody shares the game now; it keeps
 * trying), {@link #E_NO_META} (a magnet whose metadata is not here yet), {@link #E_SPACE} (not enough free space for
 * what is left), or the engine's own error (fails the job).
 */
final class NetRunner {
    static final String E_WIFI = "wifi", E_NO_PEERS = "no_peers", E_NO_META = "no_meta", E_SPACE = "space";
    static final long NO_PEERS_MS = 3 * 60_000L;

    final NetEngine engine;
    final File filesDir, downloadDir;
    /** The key of the job that runs now, or null. */
    String current;
    private final Map<String, Long> lastPeer = new HashMap<>();

    NetRunner(NetEngine engine, File filesDir, File downloadDir) {
        this.engine = engine;
        this.filesDir = filesDir;
        this.downloadDir = downloadDir;
    }

    /** What the device allows now. */
    static final class Conditions {
        /** Downloading is allowed on this network (Wi-Fi, or mobile data allowed). */
        boolean network = true;
        /** Free bytes in the download folder. */
        long free = Long.MAX_VALUE;
        long now;
    }

    /**
     * One tick. Returns true while a download is queued or runs (the service stays), false when there is nothing
     * left to do now (the service may end; jobs waiting for Wi-Fi stay queued for the next start).
     */
    boolean step(Conditions c) throws IOException {
        List<NetDownloads.Job> pending = NetDownloads.pending(NetDownloads.read(filesDir));
        if (pending.isEmpty()) {
            pauseCurrent();
            return false;
        }
        if (!c.network) {
            pauseCurrent();
            markWaiting(pending, E_WIFI);
            return false;
        }
        NetDownloads.Job j = pending.get(0);
        TorrentMeta m = RemoteLibrary.meta(filesDir, j.source);
        if (m == null) {   // a magnet's metadata not fetched yet (NetService fetches it), or the source is gone
            RemoteLibrary.Source s = RemoteLibrary.find(RemoteLibrary.sources(filesDir), j.source);
            if (s == null || s.magnet.isEmpty()) {
                fail(j.key, E_NO_META);
                return pending.size() > 1;
            }
            markWaiting(java.util.Collections.singletonList(j), E_NO_META);
            return true;
        }
        if (current != null && !current.equals(j.key))
            pauseCurrent();
        int n = j.files.size();
        int[] idx = new int[n];
        String[] paths = new String[n];
        long[] sizes = new long[n];
        long total = 0;
        for (int i = 0; i < n; i++) {
            NetDownloads.FileRef f = j.files.get(i);
            idx[i] = f.index;
            paths[i] = f.path;
            sizes[i] = f.size;
            total += f.size;
        }
        if (!j.key.equals(current)) {
            try {
                if (!engine.has(j.source))
                    engine.add(j.source, java.nio.file.Files.readAllBytes(RemoteLibrary.torrentFile(filesDir,
                                                                                                    j.source).toPath()),
                               NetDownloads.sourceDir(downloadDir, j.source));
                engine.want(j.source, idx, paths, sizes);
            } catch (IOException e) {
                fail(j.key, e.getMessage());
                return pending.size() > 1;
            }
            current = j.key;
            lastPeer.put(j.key, c.now);
        }
        long[] prog = engine.fileProgress(j.source);
        long done = 0;
        for (int i = 0; i < n; i++)
            if (prog != null && idx[i] >= 0 && idx[i] < prog.length)
                done += Math.min(prog[idx[i]], sizes[i]);
        String err = engine.error(j.source);
        if (err != null) {
            fail(j.key, err);
            current = null;
            engine.want(j.source, new int[0], new String[0], new long[0]);
            return pending.size() > 1;
        }
        int peers = engine.peers(j.source);
        if (peers > 0)
            lastPeer.put(j.key, c.now);
        boolean whole = done >= total;
        String waiting = !whole && total - done > c.free ? E_SPACE
                         : !whole && c.now - lastPeer.getOrDefault(j.key, c.now) > NO_PEERS_MS ? E_NO_PEERS : "";
        long d = done, t = total;
        int rate = engine.downloadRate(j.source);
        NetDownloads.edit(filesDir, l -> {
            NetDownloads.Job x = NetDownloads.find(l, j.key);
            if (x == null)
                return;   // cancelled meanwhile
            x.done = d;
            x.total = t;
            x.rate = whole ? 0 : rate;
            x.peers = peers;
            x.error = waiting;
            x.state = whole ? NetDownloads.DONE : NetDownloads.RUNNING;
        });
        if (whole) {
            LibraryScanner.warn("net: " + j.key + " (" + j.title + ") downloaded, " + total + " bytes");
            engine.want(j.source, new int[0], new String[0], new long[0]);
            current = null;
            return pending.size() > 1;
        }
        return true;
    }

    private void pauseCurrent() {
        if (current == null)
            return;
        String src = null;
        for (NetDownloads.Job x : NetDownloads.read(filesDir))
            if (x.key.equals(current))
                src = x.source;
        if (src != null && engine.has(src)) {
            try {
                engine.want(src, new int[0], new String[0], new long[0]);
            } catch (IOException ignored) {
                // pausing never checks files
            }
        }
        current = null;
    }

    private void markWaiting(List<NetDownloads.Job> jobs, String why) throws IOException {
        NetDownloads.edit(filesDir, l -> {
            for (NetDownloads.Job j : jobs) {
                NetDownloads.Job x = NetDownloads.find(l, j.key);
                if (x != null && x.active()) {
                    x.state = NetDownloads.QUEUED;
                    x.error = why;
                    x.rate = 0;
                    x.peers = 0;
                }
            }
        });
    }

    private void fail(String key, String why) throws IOException {
        LibraryScanner.warn("net: " + key + " failed: " + why);
        NetDownloads.edit(filesDir, l -> {
            NetDownloads.Job x = NetDownloads.find(l, key);
            if (x != null) {
                x.state = NetDownloads.FAILED;
                x.error = why == null ? "" : why;
                x.rate = 0;
            }
        });
    }

    /**
     * Cancels key's download: out of the queue, its torrent's files at priority 0 when it runs, and its files deleted
     * unless another job (a game sharing a track, say) is done with them. Paths come from the checked torrent only.
     */
    void cancel(String key) throws IOException {
        if (key.equals(current))
            pauseCurrent();
        cancelFiles(filesDir, downloadDir, key);
    }

    /** The same without an engine (the service is not running): the queue and the files. */
    static void cancelFiles(File filesDir, File downloadDir, String key) throws IOException {
        List<NetDownloads.Job> l = NetDownloads.read(filesDir);
        NetDownloads.Job j = NetDownloads.find(l, key);
        if (j == null)
            return;
        NetDownloads.cancel(filesDir, key);
        TorrentMeta m = RemoteLibrary.meta(filesDir, j.source);
        if (m == null || NetDownloads.DONE.equals(j.state))
            return;
        java.util.Set<String> kept = new java.util.HashSet<>();
        for (NetDownloads.Job o : l)
            if (o != j && NetDownloads.DONE.equals(o.state) && o.source.equals(j.source))
                for (NetDownloads.FileRef f : o.files)
                    kept.add(f.path);
        for (NetDownloads.FileRef f : j.files) {
            File lf = NetDownloads.localFile(downloadDir, j.source, m, f.path);
            if (lf != null && !kept.contains(f.path) && lf.isFile() && !lf.delete())
                LibraryScanner.warn("net: cannot delete " + lf);
        }
    }

    /**
     * Takes a source out (RemoteLibrary.remove): its torrent leaves the session, its downloads not done are cancelled
     * (their partial files deleted); downloaded games stay.
     */
    static void removeSource(NetEngine engine, File filesDir, File downloadDir, String id) throws IOException {
        if (engine != null && engine.has(id))
            engine.remove(id);
        for (NetDownloads.Job j : NetDownloads.read(filesDir))
            if (j.source.equals(id) && !NetDownloads.DONE.equals(j.state))
                cancelFiles(filesDir, downloadDir, j.key);
        RemoteLibrary.remove(filesDir, id);
    }
}
