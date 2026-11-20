package fr.adserver.xrecomp;

import static fr.adserver.xrecomp.TorrentMetaTest.file;
import static fr.adserver.xrecomp.TorrentMetaTest.files;
import static fr.adserver.xrecomp.TorrentMetaTest.torrent;
import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * NetDownloads and NetRunner over a fake NetEngine (Robolectric for org.json): the queue, one download at a time,
 * exactly the game's files wanted, progress, DONE and the pause after it, Wi-Fi, no peers, space, an engine error,
 * cancel (files deleted, a file another done job keeps stays), a source removed, the incomplete files the scan hides,
 * paths read back from disk checked again.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class NetRunnerTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files, downloads;

    /** A fake session: what was added and wanted, and the progress the test sets. */
    static final class FakeEngine implements NetEngine {
        final Map<String, long[]> progress = new HashMap<>();
        final Map<String, int[]> wanted = new HashMap<>();
        final Map<String, File> saveDirs = new HashMap<>();
        int peers = 3, rate = 1000;
        String error;
        boolean started;

        @Override
        public void start() {
            started = true;
        }

        @Override
        public void stop() {
            started = false;
        }

        @Override
        public byte[] fetchMetadata(String magnet, int timeoutSec) {
            return null;
        }

        @Override
        public boolean has(String id) {
            return saveDirs.containsKey(id);
        }

        @Override
        public void add(String id, byte[] torrent, File saveDir) throws IOException {
            TorrentMeta m = TorrentMeta.parse(torrent);
            assertEquals(id, m.infoHash);
            saveDirs.put(id, saveDir);
            int n = 0;
            for (TorrentMeta.File f : m.files)
                n = Math.max(n, f.index + 1);
            progress.put(id, new long[n]);
        }

        @Override
        public void want(String id, int[] fileIndexes, String[] paths, long[] sizes) throws IOException {
            if (!has(id))
                throw new IOException("not in");
            wanted.put(id, fileIndexes.clone());
        }

        @Override
        public long[] fileProgress(String id) {
            return progress.get(id);
        }

        @Override
        public int downloadRate(String id) {
            return rate;
        }

        @Override
        public int peers(String id) {
            return peers;
        }

        @Override
        public String error(String id) {
            return error;
        }

        @Override
        public void remove(String id) {
            saveDirs.remove(id);
        }
    }

    private FakeEngine engine;
    private NetRunner runner;
    private RemoteLibrary.Source src;
    private List<RemoteLibrary.Game> games;

    @Before
    public void setUp() throws IOException {
        files = new File(tmp.getRoot(), "files");
        downloads = new File(tmp.getRoot(), "net");
        engine = new FakeEngine();
        runner = new NetRunner(engine, files, downloads);
        // Halo (file 0), a pad file (1), Big as a split set (2, 3), Fable (4)
        Map<String, Object> pad = file(5, ".pad", "5");
        pad.put("attr", "p");
        src = RemoteLibrary.addTorrent(files, torrent("Xbox", true, files(
                file(100, "Halo.iso"), pad, file(10, "Big.1.iso"), file(20, "Big.2.iso"), file(50, "Fable.iso"))),
                null, 1);
        games = RemoteLibrary.games(src, RemoteLibrary.meta(files, src.id));
        assertEquals(3, games.size());
    }

    private NetRunner.Conditions ok(long now) {
        NetRunner.Conditions c = new NetRunner.Conditions();
        c.now = now;
        return c;
    }

    private NetDownloads.Job job(String key) {
        return NetDownloads.find(NetDownloads.read(files), key);
    }

    @Test
    public void oneGameDownloadsItsFilesOnlyThenIsDoneAndPaused() throws IOException {
        RemoteLibrary.Game big = games.get(1);
        assertEquals("Big", big.title);
        NetDownloads.enqueue(files, big, 10);
        assertTrue(runner.step(ok(0)));
        assertArrayEquals(new int[] { 2, 3 }, engine.wanted.get(src.id));
        assertEquals(NetDownloads.sourceDir(downloads, src.id), engine.saveDirs.get(src.id));
        NetDownloads.Job j = job(big.key);
        assertEquals(NetDownloads.RUNNING, j.state);
        assertEquals(0, j.done);
        assertEquals(30, j.total);
        assertEquals(3, j.peers);

        engine.progress.get(src.id)[2] = 10;
        engine.progress.get(src.id)[3] = 5;
        engine.progress.get(src.id)[0] = 100;   // another file's progress is never counted
        assertTrue(runner.step(ok(1000)));
        assertEquals(15, job(big.key).done);
        assertEquals(50, job(big.key).percent());

        engine.progress.get(src.id)[3] = 20;
        assertFalse(runner.step(ok(2000)));   // nothing more queued
        j = job(big.key);
        assertEquals(NetDownloads.DONE, j.state);
        assertEquals(0, j.rate);
        assertTrue(j.auto);
        assertEquals(0, engine.wanted.get(src.id).length);   // paused: no seeding yet
    }

    @Test
    public void downloadsRunOneAfterTheOther() throws IOException {
        NetDownloads.enqueue(files, games.get(0), 1);
        NetDownloads.enqueue(files, games.get(2), 2);
        assertTrue(runner.step(ok(0)));
        assertArrayEquals(new int[] { 0 }, engine.wanted.get(src.id));
        assertEquals(NetDownloads.QUEUED, job(games.get(2).key).state);
        engine.progress.get(src.id)[0] = 100;
        assertTrue(runner.step(ok(1)));   // Halo done, Fable still queued
        assertEquals(NetDownloads.DONE, job(games.get(0).key).state);
        assertTrue(runner.step(ok(2)));
        assertArrayEquals(new int[] { 4 }, engine.wanted.get(src.id));
        // A running one queued again stays as it is; a done one (its copy removed since) is queued anew.
        NetDownloads.enqueue(files, games.get(2), 3);
        assertEquals(NetDownloads.RUNNING, job(games.get(2).key).state);
        NetDownloads.enqueue(files, games.get(0), 3);
        assertEquals(NetDownloads.QUEUED, job(games.get(0).key).state);
        assertEquals(0, job(games.get(0).key).done);
    }

    @Test
    public void onlyOnWifiAndWaitingReasons() throws IOException {
        NetDownloads.enqueue(files, games.get(0), 1);
        NetRunner.Conditions c = ok(0);
        c.network = false;
        assertFalse(runner.step(c));   // the service may end; the job stays queued
        assertEquals(NetDownloads.QUEUED, job(games.get(0).key).state);
        assertEquals(NetRunner.E_WIFI, job(games.get(0).key).error);

        assertTrue(runner.step(ok(0)));
        assertEquals("", job(games.get(0).key).error);
        engine.peers = 0;
        runner.step(ok(NetRunner.NO_PEERS_MS - 1));
        assertEquals("", job(games.get(0).key).error);
        runner.step(ok(NetRunner.NO_PEERS_MS + 1));
        assertEquals(NetRunner.E_NO_PEERS, job(games.get(0).key).error);
        assertEquals(NetDownloads.RUNNING, job(games.get(0).key).state);   // it keeps trying
        engine.peers = 2;
        NetRunner.Conditions small = ok(NetRunner.NO_PEERS_MS + 2);
        small.free = 99;
        runner.step(small);
        assertEquals(NetRunner.E_SPACE, job(games.get(0).key).error);
    }

    @Test
    public void anEngineErrorFailsTheJobAndTheNextOneRuns() throws IOException {
        NetDownloads.enqueue(files, games.get(0), 1);
        NetDownloads.enqueue(files, games.get(2), 2);
        runner.step(ok(0));
        engine.error = "disk full";
        assertTrue(runner.step(ok(1)));
        assertEquals(NetDownloads.FAILED, job(games.get(0).key).state);
        assertEquals("disk full", job(games.get(0).key).error);
        engine.error = null;
        runner.step(ok(2));
        assertArrayEquals(new int[] { 4 }, engine.wanted.get(src.id));
        // Try again: a failed job is queued anew.
        NetDownloads.enqueue(files, games.get(0), 3);
        assertEquals(NetDownloads.QUEUED, job(games.get(0).key).state);
        assertEquals("", job(games.get(0).key).error);
    }

    @Test
    public void cancelDeletesThePartialFilesButNotADoneGamesFiles() throws IOException {
        TorrentMeta m = RemoteLibrary.meta(files, src.id);
        RemoteLibrary.Game halo = games.get(0), fable = games.get(2);
        NetDownloads.enqueue(files, halo, 1);
        runner.step(ok(0));
        File haloFile = NetDownloads.localFile(downloads, src.id, m, "Halo.iso");
        assertEquals(new File(new File(NetDownloads.sourceDir(downloads, src.id), "Xbox"), "Halo.iso"), haloFile);
        haloFile.getParentFile().mkdirs();
        Files.write(haloFile.toPath(), new byte[40]);
        Set<String> hidden = NetDownloads.incomplete(files, downloads);
        assertTrue(hidden.contains(haloFile.getAbsolutePath()));

        runner.cancel(halo.key);
        assertNull(job(halo.key));
        assertFalse(haloFile.exists());
        assertEquals(0, engine.wanted.get(src.id).length);

        // A done game keeps its file and is not hidden from the scan.
        NetDownloads.enqueue(files, fable, 2);
        runner.step(ok(1));
        engine.progress.get(src.id)[4] = 50;
        runner.step(ok(2));
        File fableFile = NetDownloads.localFile(downloads, src.id, m, "Fable.iso");
        fableFile.getParentFile().mkdirs();
        Files.write(fableFile.toPath(), new byte[50]);
        assertFalse(NetDownloads.incomplete(files, downloads).contains(fableFile.getAbsolutePath()));
        NetRunner.cancelFiles(files, downloads, fable.key);   // a done job: out of the list, its file stays
        assertTrue(fableFile.exists());
    }

    @Test
    public void removingASourceCancelsItsDownloadsAndKeepsDoneGames() throws IOException {
        NetDownloads.enqueue(files, games.get(0), 1);
        runner.step(ok(0));
        assertTrue(engine.has(src.id));
        NetRunner.removeSource(engine, files, downloads, src.id);
        assertFalse(engine.has(src.id));
        assertTrue(RemoteLibrary.sources(files).isEmpty());
        assertTrue(NetDownloads.read(files).isEmpty());
    }

    @Test
    public void aMagnetWithoutMetadataWaitsAndAGoneSourceFails() throws IOException {
        String other = "fedcba9876543210fedcba9876543210fedcba98";
        RemoteLibrary.Source mag = RemoteLibrary.addMagnet(files, "magnet:?xt=urn:btih:" + other, null, 1);
        assertNotNull(mag);
        NetDownloads.Job j = new NetDownloads.Job();
        j.key = "net-fedcba987654-00000001";
        j.source = other;
        j.files.add(new NetDownloads.FileRef(0, "A.iso", 5));
        NetDownloads.write(files, java.util.Collections.singletonList(j));
        assertTrue(runner.step(ok(0)));
        assertEquals(NetRunner.E_NO_META, job(j.key).error);
        assertEquals(NetDownloads.QUEUED, job(j.key).state);
        RemoteLibrary.remove(files, other);
        runner.step(ok(1));
        assertEquals(NetDownloads.FAILED, job(j.key).state);
    }

    @Test
    public void pathsReadBackAreCheckedAgain() throws IOException {
        Files.createDirectories(NetDownloads.file(files).getParentFile().toPath());
        Files.write(NetDownloads.file(files).toPath(), ("{\"v\":1,\"jobs\":[{\"key\":\"k\",\"source\":\"" + src.id
                + "\",\"state\":\"queued\",\"files\":[{\"index\":0,\"path\":\"../../etc/x\",\"size\":1},"
                + "{\"index\":0,\"path\":\"/abs\",\"size\":1},{\"index\":0,\"path\":\"Halo.iso\",\"size\":100}]}]}")
                .getBytes());
        NetDownloads.Job j = job("k");
        assertEquals(1, j.files.size());
        assertEquals("Halo.iso", j.files.get(0).path);
        assertFalse(NetDownloads.safePath("a/../b"));
        assertFalse(NetDownloads.safePath(""));
        assertTrue(NetDownloads.safePath("a/b.iso"));
        assertNull(NetDownloads.localFile(downloads, src.id, RemoteLibrary.meta(files, src.id), "../x"));
    }
}
