package fr.adserver.xrecomp;

import org.libtorrent4j.ErrorCode;
import org.libtorrent4j.FileStorage;
import org.libtorrent4j.Priority;
import org.libtorrent4j.SessionManager;
import org.libtorrent4j.SessionParams;
import org.libtorrent4j.SettingsPack;
import org.libtorrent4j.TorrentFlags;
import org.libtorrent4j.TorrentHandle;
import org.libtorrent4j.TorrentInfo;
import org.libtorrent4j.TorrentStatus;
import org.libtorrent4j.swig.settings_pack;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

/**
 * NetEngine over libtorrent4j (libtorrent 2.x; MIT, NOTICE): the only class that touches libtorrent, so nothing the
 * JVM tests load needs its native library (libtorrent4j.so, from the libtorrent4j-android-arm64 jar).
 *
 * Session defaults (docs « Network library »): a random listening port, no UPnP and no NAT-PMP (nothing opened on the
 * player's router), DHT and local peer discovery on (a magnet needs the DHT; LSD finds the player's own NAS on the
 * same network); a private torrent (the private flag) gets neither DHT nor PEX nor LSD, libtorrent's own rule. A
 * torrent is added with every file at priority 0 (TorrentFlags.DEFAULT_DONT_DOWNLOAD: nothing is created, pieces
 * shared with a wanted file go to libtorrent's hidden part file), not auto-managed, paused until {@link #want}.
 */
final class LibtorrentEngine implements NetEngine {
    private SessionManager s;
    private final Map<String, TorrentInfo> infos = new HashMap<>();
    private final File metaTmp, resumeDir;

    /** metaTmp: a scratch folder for magnet fetches; resumeDir: where <id>.resume files are kept. */
    LibtorrentEngine(File metaTmp, File resumeDir) {
        this.metaTmp = metaTmp;
        this.resumeDir = resumeDir;
    }

    File resumeFile(String id) {
        return new File(resumeDir, id + ".resume");
    }

    /** libtorrent hands resume data over in an alert: written beside the torrent, atomically. */
    private final org.libtorrent4j.AlertListener resumeWriter = new org.libtorrent4j.AlertListener() {
        @Override
        public int[] types() {
            return new int[] { org.libtorrent4j.alerts.AlertType.SAVE_RESUME_DATA.swig() };
        }

        @Override
        public void alert(org.libtorrent4j.alerts.Alert<?> a) {
            if (!(a instanceof org.libtorrent4j.alerts.SaveResumeDataAlert))
                return;
            org.libtorrent4j.alerts.SaveResumeDataAlert sa = (org.libtorrent4j.alerts.SaveResumeDataAlert)a;
            String id = idOf(sa.handle());
            if (id == null)
                return;
            try {
                byte[] b = org.libtorrent4j.AddTorrentParams.writeResumeDataBuf(sa.params());
                resumeDir.mkdirs();
                File f = resumeFile(id), tmp = new File(resumeDir, "." + id + ".resume.tmp");
                java.nio.file.Files.write(tmp.toPath(), b);
                if (!tmp.renameTo(f))
                    tmp.delete();
            } catch (IOException | RuntimeException e) {
                System.err.println("net: cannot write the resume data of " + id + ": " + e);   // (no Android here)
            } finally {
                synchronized (pendingSaves) {
                    pendingSaves.remove(id);
                    pendingSaves.notifyAll();
                }
            }
        }
    };
    private final java.util.Set<String> pendingSaves = new java.util.HashSet<>();

    private synchronized String idOf(TorrentHandle h) {
        if (h == null || !h.isValid())
            return null;
        org.libtorrent4j.Sha1Hash best = h.infoHash();
        for (Map.Entry<String, TorrentInfo> e : infos.entrySet())
            if (e.getValue().infoHash().equals(best))
                return e.getKey();
        return null;
    }

    @Override
    public synchronized void start() throws IOException {
        if (s != null && s.isRunning())
            return;
        try {
            SettingsPack sp = new SettingsPack();
            sp.setBoolean(settings_pack.bool_types.enable_upnp.swigValue(), false);
            sp.setBoolean(settings_pack.bool_types.enable_natpmp.swigValue(), false);
            sp.setBoolean(settings_pack.bool_types.enable_dht.swigValue(), true);
            sp.setBoolean(settings_pack.bool_types.enable_lsd.swigValue(), true);
            sp.listenInterfaces("0.0.0.0:0,[::]:0");
            s = new SessionManager(false);
            s.addListener(resumeWriter);
            s.start(new SessionParams(sp));
        } catch (RuntimeException | LinkageError e) {
            s = null;
            throw new IOException("libtorrent: " + e, e);
        }
    }

    @Override
    public void stop() {
        synchronized (this) {   // what came is saved first: the next session goes on without checking it again
            for (String id : infos.keySet())
                saveResume(id);
        }
        long until = System.currentTimeMillis() + 3000;
        synchronized (pendingSaves) {
            while (!pendingSaves.isEmpty() && System.currentTimeMillis() < until) {
                try {
                    pendingSaves.wait(100);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        stopSession();
    }

    private synchronized void stopSession() {
        if (s != null) {
            try {
                s.stop();
            } catch (RuntimeException ignored) {
                // ending anyway
            }
        }
        s = null;
        infos.clear();
    }

    @Override
    public byte[] fetchMetadata(String magnet, int timeoutSec) throws IOException {
        SessionManager m;
        synchronized (this) {
            start();
            m = s;
        }
        metaTmp.mkdirs();
        try {
            return m.fetchMagnet(magnet, timeoutSec, metaTmp);
        } catch (RuntimeException e) {
            throw new IOException("libtorrent: " + e.getMessage(), e);
        }
    }

    private TorrentHandle handle(String id) {
        TorrentInfo ti = infos.get(id);
        if (s == null || ti == null)
            return null;
        TorrentHandle h = s.find(ti.infoHash());
        return h != null && h.isValid() ? h : null;
    }

    @Override
    public synchronized boolean has(String id) {
        return handle(id) != null;
    }

    @Override
    public synchronized void add(String id, byte[] torrent, File saveDir) throws IOException {
        start();
        if (handle(id) != null)
            return;
        TorrentInfo ti;
        try {
            ti = new TorrentInfo(torrent);
        } catch (RuntimeException e) {
            throw new IOException("libtorrent: not a torrent: " + e.getMessage(), e);
        }
        // The v1 hash (TorrentMeta's): infoHash() is libtorrent's « best » one, a hybrid torrent's truncated v2 hash.
        if (!ti.hasV1() || !ti.infoHashes().getV1().toHex().equalsIgnoreCase(id))
            throw new IOException("libtorrent: the torrent's v1 hash is not " + id);
        if (!saveDir.isDirectory() && !saveDir.mkdirs())
            throw new IOException("cannot create " + saveDir);
        infos.put(id, ti);
        File resume = resumeFile(id);
        s.download(ti, saveDir, resume.isFile() ? resume : null, Priority.array(Priority.IGNORE, ti.numFiles()), null,
                   TorrentFlags.DEFAULT_DONT_DOWNLOAD.or_(TorrentFlags.PAUSED));
        for (int i = 0; i < 100 && handle(id) == null; i++) {   // the add is asynchronous: at most 5 s
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        TorrentHandle h = handle(id);
        if (h == null) {
            infos.remove(id);
            throw new IOException("libtorrent: the torrent was not added");
        }
        h.unsetFlags(TorrentFlags.AUTO_MANAGED);
    }

    @Override
    public synchronized void want(String id, int[] fileIndexes, String[] paths, long[] sizes) throws IOException {
        TorrentHandle h = handle(id);
        if (h == null)
            throw new IOException("libtorrent: " + id + " is not in the session");
        TorrentInfo ti = infos.get(id);
        FileStorage fs = ti.files();
        Priority[] p = Priority.array(Priority.IGNORE, ti.numFiles());
        for (int k = 0; k < fileIndexes.length; k++) {
            int i = fileIndexes[k];
            String fp = i < 0 || i >= fs.numFiles() ? "" : fs.filePath(i).replace('\\', '/');
            if (i < 0 || i >= fs.numFiles() || fs.padFileAt(i) || fs.fileSize(i) != sizes[k]
                || !(fp.equals(paths[k]) || fp.equals(ti.name() + "/" + paths[k])))
                throw new IOException("libtorrent: file " + i + " is not " + paths[k]);
            p[i] = Priority.DEFAULT;
        }
        h.prioritizeFiles(p);
        if (fileIndexes.length == 0)
            h.pause();
        else
            h.resume();
    }

    @Override
    public synchronized long[] fileProgress(String id) {
        TorrentHandle h = handle(id);
        return h == null ? null : h.fileProgress(TorrentHandle.PIECE_GRANULARITY);
    }

    private TorrentStatus status(String id) {
        TorrentHandle h = handle(id);
        return h == null ? null : h.status();
    }

    @Override
    public synchronized int downloadRate(String id) {
        TorrentStatus st = status(id);
        return st == null ? 0 : st.downloadPayloadRate();
    }

    @Override
    public synchronized int peers(String id) {
        TorrentStatus st = status(id);
        return st == null ? 0 : st.numPeers();
    }

    @Override
    public synchronized String error(String id) {
        TorrentStatus st = status(id);
        if (st == null)
            return null;
        ErrorCode e = st.errorCode();
        return e != null && e.isError() ? e.getMessage() : null;
    }

    @Override
    public synchronized void connectPeer(String id, String host, int port) throws IOException {
        TorrentHandle h = handle(id);
        if (h == null)
            throw new IOException("libtorrent: " + id + " is not in the session");
        try {
            h.swig().connect_peer(new org.libtorrent4j.TcpEndpoint(host, port).swig());
        } catch (RuntimeException e) {
            throw new IOException("libtorrent: cannot connect to " + host + ":" + port + ": " + e.getMessage(), e);
        }
    }

    @Override
    public synchronized void saveResume(String id) {
        TorrentHandle h = handle(id);
        if (h == null)
            return;
        synchronized (pendingSaves) {
            pendingSaves.add(id);
        }
        h.saveResumeData(TorrentHandle.FLUSH_DISK_CACHE);
    }

    @Override
    public void forgetResume(String id) {
        resumeFile(id).delete();
    }

    @Override
    public synchronized void remove(String id) {
        TorrentHandle h = handle(id);
        if (h != null)
            s.remove(h);
        infos.remove(id);
        forgetResume(id);
    }
}
