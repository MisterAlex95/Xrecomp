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
        File work = Files.createTempDirectory("xba-net").toFile();
        File seed = new File(work, "seed"), lib = new File(seed, "Lib"), dl = new File(work, "dl");
        lib.mkdirs();
        Random r = new Random(42);
        byte[] a = new byte[3 << 20], b = new byte[(5 << 20) + 123], c = new byte[1 << 20];
        r.nextBytes(a);
        r.nextBytes(b);
        r.nextBytes(c);
        Files.write(new File(lib, "A.iso").toPath(), a);
        Files.write(new File(lib, "B.iso").toPath(), b);
        Files.write(new File(lib, "C.gba").toPath(), c);
        byte[] torrent = new TorrentBuilder().path(lib).pieceSize(256 << 10).setPrivate(true).generate().entry().bencode();

        TorrentMeta m = TorrentMeta.parse(torrent);
        check(m.isPrivate && m.multiFile && m.files.size() == 3, "TorrentMeta reads libtorrent's torrent: " + m.files.size()
              + " files, private");
        TorrentMeta.File fb = m.file("B.iso");
        TorrentInfo ti = new TorrentInfo(torrent);
        check(ti.hasV2() && ti.infoHashes().getV1().toHex().equals(m.infoHash),
              "hybrid torrent, same v1 info hash as libtorrent: " + m.infoHash + " (best: " + ti.infoHash().toHex() + ")");
        check(ti.files().filePath(fb.index).replace('\\', '/').endsWith("B.iso") && ti.files().fileSize(fb.index) == b.length,
              "B.iso is file " + fb.index + " for both (pad files counted: " + ti.numFiles() + " entries)");

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
        check(h1 != null && h1.isValid() && port > 0, "seeder up on 127.0.0.1:" + port + " (handle "
              + (h1 != null && h1.isValid()) + ", endpoints " + s1.listenEndpoints() + ")");

        // The app's engine downloads B only.
        LibtorrentEngine e = new LibtorrentEngine(new File(work, "meta"));
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
        e.want(m.infoHash, new int[] { fb.index }, new String[] { fb.path }, new long[] { fb.size });
        e.connectPeer(m.infoHash, "127.0.0.1", port);
        long done = 0;
        for (int i = 0; i < 600 && done < fb.size; i++) {
            Thread.sleep(100);
            long[] p = e.fileProgress(m.infoHash);
            done = p == null ? 0 : p[fb.index];
            if (i % 20 == 0)
                System.out.println("     " + done + "/" + fb.size + " peers " + e.peers(m.infoHash) + " err "
                                   + e.error(m.infoHash));
        }
        check(done == fb.size, "B.iso downloaded: " + done + " bytes");
        File got = new File(new File(save, m.name), fb.path);   // as NetDownloads.localFile
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
        check(new File(got.getParentFile(), "A.iso").length() == 0 && new File(got.getParentFile(), "C.gba").length() == 0,
              "A.iso and C.gba not written");
        e.remove(m.infoHash);
        check(!e.has(m.infoHash), "removed from the session");
        e.stop();
        s1.stop();
        System.out.println("net host check: all good");
        System.exit(0);
    }
}
