package fr.adserver.xrecomp;

import org.libtorrent4j.SessionManager;
import org.libtorrent4j.SessionParams;
import org.libtorrent4j.SettingsPack;
import org.libtorrent4j.TorrentBuilder;
import org.libtorrent4j.TorrentFlags;
import org.libtorrent4j.TorrentHandle;
import org.libtorrent4j.TorrentInfo;
import org.libtorrent4j.Priority;
import org.libtorrent4j.swig.settings_pack;

import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * The network library's engine against real libtorrent (XBA-321), on a Linux host (libtorrent4j-linux, x86-64): no
 * device can be reached by an agent, so this is the one check of LibtorrentEngine itself. tools/android/net_host_check.sh
 * builds and runs it in the app's Docker image.
 *
 * A seeder session shares a folder of three synthetic files (random bytes, nothing of a game) whose pieces straddle
 * the files; LibtorrentEngine adds the torrent (made by libtorrent: hybrid v1/v2, so with pad files), wants only the
 * middle file, connects to the seeder on localhost, and must end with exactly that file, byte for byte, and no other
 * visible file. Then want() of nothing pauses it, and a file index that does not match is refused. Exit 0 when all
 * holds.
 */
public final class NetHostCheck {
    private static void check(boolean ok, String what) {
        System.out.println((ok ? "ok   " : "FAIL ") + what);
        if (!ok)
            System.exit(1);
    }

    private static List<String> listFiles(File d, File base, List<String> out) {
        File[] l = d.listFiles();
        if (l != null)
            for (File f : l) {
                if (f.isDirectory())
                    listFiles(f, base, out);
                else
                    out.add(base.toPath().relativize(f.toPath()) + " " + f.length());
            }
        return out;
    }

    public static void main(String[] args) throws Exception {
        run(false);   // hybrid v1/v2: piece-aligned files, pad files
        run(true);    // v1 only: pieces straddle the files
        System.out.println("net host check: all good");
        System.exit(0);
    }

    private static long progress(LibtorrentEngine e, String id, int index) {
        long[] p = e.fileProgress(id);
        return p == null ? 0 : p[index];
    }

    static int seederPort;

    private static void waitFor(LibtorrentEngine e, String id, int index, long until) throws Exception {
        for (int i = 0; i < 600 && progress(e, id, index) < until; i++) {
            Thread.sleep(50);
            if (i % 20 == 0 && e.peers(id) == 0)   // (a connection asked while the torrent was still checking is lost)
                e.connectPeer(id, "127.0.0.1", seederPort);
            if (i % 40 == 0)
                System.out.println("     " + progress(e, id, index) + "/" + until + " peers " + e.peers(id) + " rate "
                                   + e.downloadRate(id) + " err " + e.error(id));
        }
    }

    private static void run(boolean v1Only) throws Exception {
        System.out.println("== " + (v1Only ? "v1-only torrent, pieces across files" : "hybrid torrent"));
        File work = Files.createTempDirectory("xba-net").toFile();
        File seed = new File(work, "seed"), lib = new File(seed, "Lib"), dl = new File(work, "dl");
        lib.mkdirs();
        Random r = new Random(42);
        // Sizes that are no multiple of the piece size: B's first and last pieces are shared with A and C.
        byte[] a = new byte[(3 << 20) + 77777], b = new byte[(5 << 20) + 123], c = new byte[(1 << 20) + 4321];
        r.nextBytes(a);
        r.nextBytes(b);
        r.nextBytes(c);
        Files.write(new File(lib, "A.iso").toPath(), a);
        Files.write(new File(lib, "B.iso").toPath(), b);
        Files.write(new File(lib, "C.gba").toPath(), c);
        TorrentBuilder tb = new TorrentBuilder().path(lib).pieceSize(256 << 10).setPrivate(true);
        if (v1Only)
            tb.flags(TorrentBuilder.V1_ONLY);
        byte[] torrent = tb.generate().entry().bencode();

        TorrentMeta m = TorrentMeta.parse(torrent);
        check(m.isPrivate && m.multiFile && m.files.size() == 3, "TorrentMeta reads libtorrent's torrent: " + m.files.size()
              + " files, private");
        TorrentMeta.File fb = m.file("B.iso");
        TorrentInfo ti = new TorrentInfo(torrent);
        check(ti.hasV2() != v1Only && ti.infoHashes().getV1().toHex().equals(m.infoHash),
              "same v1 info hash as libtorrent: " + m.infoHash + " (best: " + ti.infoHash().toHex() + ")");
        check(ti.files().filePath(fb.index).replace('\\', '/').endsWith("B.iso") && ti.files().fileSize(fb.index) == b.length,
              "B.iso is file " + fb.index + " for both (" + ti.numFiles() + " entries)");
        if (v1Only)
            check(ti.files().fileOffset(fb.index) % ti.pieceLength() != 0
                  || (ti.files().fileOffset(fb.index) + fb.size) % ti.pieceLength() != 0,
                  "B.iso shares a piece with a neighbour");
        // The seeder: localhost only, no DHT / LSD.
        SettingsPack sp = new SettingsPack();
        sp.setBoolean(settings_pack.bool_types.enable_dht.swigValue(), false);
        sp.setBoolean(settings_pack.bool_types.enable_lsd.swigValue(), false);
        sp.setBoolean(settings_pack.bool_types.enable_upnp.swigValue(), false);
        sp.setBoolean(settings_pack.bool_types.enable_natpmp.swigValue(), false);
        sp.listenInterfaces("127.0.0.1:0");
        SessionManager s1 = new SessionManager(false);
        s1.start(new SessionParams(sp));
        s1.download(ti, seed, null, Priority.array(Priority.DEFAULT, ti.numFiles()), null, TorrentFlags.SEED_MODE);
        TorrentHandle h1 = null;
        for (int i = 0; i < 200 && (h1 == null || !h1.isValid()); i++) {
            Thread.sleep(50);
            h1 = s1.find(ti.infoHash());
        }
        int port = 0;
        for (int i = 0; i < 100 && port == 0; i++) {
            int lp = s1.swig().listen_port();
            if (lp > 0)
                port = lp;
            for (String ep : s1.listenEndpoints())
                if (ep.startsWith("127.0.0.1:"))
                    port = Integer.parseInt(ep.substring(ep.lastIndexOf(':') + 1));
            if (port == 0)
                Thread.sleep(50);
        }
        seederPort = port;
        check(h1 != null && h1.isValid() && port > 0, "seeder up on 127.0.0.1:" + port + " (handle "
              + (h1 != null && h1.isValid()) + ", endpoints " + s1.listenEndpoints() + ")");

        // The app's engine downloads B only, stopped halfway (resume data written), started again, finished.
        File resume = new File(work, "resume");
        LibtorrentEngine e = new LibtorrentEngine(new File(work, "meta"), resume);
        e.start();
        File save = new File(dl, m.infoHash.substring(0, 12));   // as NetDownloads.sourceDir
        e.add(m.infoHash, torrent, save);
        check(e.has(m.infoHash), "engine added the torrent");
        try {
            e.want(m.infoHash, new int[] { fb.index }, new String[] { "A.iso" }, new long[] { fb.size });
            check(false, "a mismatched path is refused");
        } catch (java.io.IOException expected) {
            check(true, "a mismatched path is refused: " + expected.getMessage());
        }
        File got = new File(new File(save, m.name), fb.path);   // as NetDownloads.localFile
        int[] wantB = { fb.index };
        String[] pathB = { fb.path };
        long[] sizeB = { fb.size };
        h1.setUploadLimit(1 << 20);   // 1 MB/s: there is time to stop it halfway
        e.want(m.infoHash, wantB, pathB, sizeB);
        e.connectPeer(m.infoHash, "127.0.0.1", port);
        waitFor(e, m.infoHash, fb.index, fb.size * 2 / 5);
        long half = progress(e, m.infoHash, fb.index);
        check(half > 0 && half < fb.size, "B.iso partly downloaded: " + half);
        e.stop();
        check(new File(resume, m.infoHash + ".resume").isFile(), "resume data written at stop");

        e = new LibtorrentEngine(new File(work, "meta"), resume);
        e.start();
        e.add(m.infoHash, torrent, save);
        for (int i = 0; i < 100 && progress(e, m.infoHash, fb.index) == 0; i++)
            Thread.sleep(50);
        long back = progress(e, m.infoHash, fb.index);
        check(back >= half * 9 / 10, "a new session starts from what came: " + back + " of " + half);
        h1.setUploadLimit(0);
        e.want(m.infoHash, wantB, pathB, sizeB);
        e.connectPeer(m.infoHash, "127.0.0.1", port);
        waitFor(e, m.infoHash, fb.index, fb.size);
        check(progress(e, m.infoHash, fb.index) == fb.size, "B.iso downloaded after the restart");
        e.want(m.infoHash, new int[0], new String[0], new long[0]);   // pause (flushes the files)
        Thread.sleep(1500);
        check(got.isFile() && Arrays.equals(Files.readAllBytes(got.toPath()), b), "B.iso is byte for byte at " + got);
        List<String> present = listFiles(dl, dl, new ArrayList<>());
        System.out.println("     files: " + present);
        boolean others = false;
        for (String f : present) {
            String name = f.substring(f.lastIndexOf('/') + 1);
            if (!name.startsWith(".") && !name.startsWith("B.iso"))
                others = true;
        }
        check(!others, "no other visible file (neighbours' pieces stay in the hidden part file)");
        check(!new File(got.getParentFile(), "A.iso").exists() && !new File(got.getParentFile(), "C.gba").exists(),
              "A.iso and C.gba not created");

        // Cancel as NetRunner does (pause, out of the session, files deleted), then the same game again: whole.
        e.want(m.infoHash, new int[0], new String[0], new long[0]);
        e.remove(m.infoHash);
        check(!e.has(m.infoHash) && !new File(resume, m.infoHash + ".resume").exists(),
              "removed from the session, resume data forgotten");
        check(got.delete(), "the downloaded file deleted");
        h1.setUploadLimit(1 << 20);
        e.add(m.infoHash, torrent, save);
        e.want(m.infoHash, wantB, pathB, sizeB);
        e.connectPeer(m.infoHash, "127.0.0.1", port);
        waitFor(e, m.infoHash, fb.index, fb.size * 2 / 5);
        // cancelled at about 40 %
        e.want(m.infoHash, new int[0], new String[0], new long[0]);
        e.remove(m.infoHash);
        Thread.sleep(500);
        got.delete();
        h1.setUploadLimit(0);
        e.add(m.infoHash, torrent, save);
        e.want(m.infoHash, wantB, pathB, sizeB);
        e.connectPeer(m.infoHash, "127.0.0.1", port);
        waitFor(e, m.infoHash, fb.index, fb.size);
        e.want(m.infoHash, new int[0], new String[0], new long[0]);
        Thread.sleep(1500);
        check(got.isFile() && Arrays.equals(Files.readAllBytes(got.toPath()), b),
              "cancelled at 40 %, asked again: B.iso whole, no holes");
        e.remove(m.infoHash);
        check(!e.has(m.infoHash), "removed from the session");
        e.stop();
        s1.stop();
    }
}
