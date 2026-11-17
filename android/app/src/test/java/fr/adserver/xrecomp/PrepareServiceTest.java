package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.app.Notification;
import android.app.NotificationManager;
import android.content.Intent;
import android.os.Looper;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ServiceController;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * PrepareService with a scripted command (PrepareService.environmentOverride): foreground, status, cancel, one at a
 * time, and the queue (another title waits, runs next, can be taken out; a dead process's queue is dropped).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareServiceTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String TID = "12345678", TID2 = "4541000D";

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
    }

    @After
    public void tearDown() throws Exception {
        // A gated script still waiting (a failed check above) must not outlive its test.
        go(TID);
        go(TID2);
        PrepareService.environmentOverride = null;
        PrepareService.forgetQueue();
        Thread w = PrepareService.workerThread();
        if (w != null)
            w.join(20000);
    }

    private void useScript(String body) throws Exception {
        File script = tmp.newFile("fake_prepare.sh");
        TestSupport.write(script, body);
        File files = app.getFilesDir();
        PrepareService.environmentOverride = (c, gameDir, titleId, runner) -> {
            runner.installProgress(50, "toolkit");
            return new PrepareRunner.CommandFactory() {
                @Override
                public List<String> command(int jobs) {
                    return TestSupport.sh(script, String.valueOf(jobs), Pack.packDir(files, titleId).getAbsolutePath());
                }

                @Override
                public Map<String, String> environment() {
                    return Collections.emptyMap();
                }

                @Override
                public File workingDir() {
                    return tmp.getRoot();
                }
            };
        };
    }

    private ServiceController<PrepareService> start() {
        Intent i = PrepareService.startIntent(app, tmp.getRoot().getAbsolutePath(), TID, "Test title");
        ServiceController<PrepareService> sc = Robolectric.buildService(PrepareService.class, i).create();
        sc.startCommand(0, 1);
        return sc;
    }

    private void waitWorker() throws Exception {
        Thread w = PrepareService.workerThread();
        assertNotNull(w);
        w.join(30000);
        assertFalse(w.isAlive());
        shadowOf(Looper.getMainLooper()).idle();
    }

    @Test
    public void progressThenDone() throws Exception {
        // The script writes a valid pack where the service will check it (like tools.prepare's pack step).
        String packJson = "{\"format\":1,\"title_id\":\"" + TID + "\",\"shell_abi\":1,\"lib\":\"libtitle.so\","
                          + "\"lib_size\":4,\"abi\":\"arm64-v8a\",\"entry\":\"recomp_title_main\"}";
        useScript("echo 'XBA_PROGRESS step=1/5 id=identify pct=100 msg=ok'\n"
                  + "echo 'XBA_PROGRESS step=4/5 id=compile pct=50 eta=120 msg=jobs '$1\n"
                  + "mkdir -p \"$2\" && printf 'ELF!' > \"$2/libtitle.so\"\n"
                  + "echo '" + packJson + "' > \"$2/pack.json\"\n"
                  + "echo \"XBA_RESULT ok pack=$2\"\n");
        ServiceController<PrepareService> sc = start();
        assertTrue(PrepareService.busy() || PrepareService.workerThread() != null);
        Notification fg = shadowOf(sc.get()).getLastForegroundNotification();
        assertNotNull("the service went foreground", fg);
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertNotNull(s);
        assertEquals(PrepareProtocol.DONE, s.state);
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertTrue(PrepareService.logFile(app, TID).isFile());
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
        NotificationManager nm = app.getSystemService(NotificationManager.class);
        assertNotNull(shadowOf(nm).getNotification(TID, PrepareService.NOTIF_DONE_ID));
    }

    @Test
    public void okWithoutAPackFails() throws Exception {
        useScript("echo 'XBA_RESULT ok pack=/nowhere'\n");
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals(PrepareRunner.PACK_INVALID, s.code);
    }

    @Test
    public void errorThenStatusFailed() throws Exception {
        useScript("echo 'XBA_RESULT error code=NOT_AN_XBE msg=default.xbe is not an XBE'\nexit 1\n");
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals("NOT_AN_XBE", s.code);
        assertEquals("default.xbe is not an XBE", s.msg);
    }

    @Test
    public void cancelAHang() throws Exception {
        useScript("echo 'XBA_PROGRESS step=3/5 id=lift pct=1 msg=forever'\nwhile :; do sleep 1; done\n");
        ServiceController<PrepareService> sc = start();
        File status = PrepareService.statusFile(app, TID);
        long until = System.currentTimeMillis() + 15000;
        while (System.currentTimeMillis() < until) {
            PrepareProtocol.Status s = PrepareProtocol.readStatus(status);
            if (s != null && "lift".equals(s.id))
                break;
            Thread.sleep(50);
        }
        assertEquals("lift", PrepareProtocol.readStatus(status).id);
        // A second start of the running title is ignored (one Prepare at a time).
        Thread w = PrepareService.workerThread();
        sc.withIntent(PrepareService.startIntent(app, "/elsewhere", TID, "x")).startCommand(0, 2);
        assertEquals(w, PrepareService.workerThread());
        assertTrue(PrepareService.queuedTitleIds().isEmpty());
        sc.withIntent(PrepareService.cancelIntent(app, TID)).startCommand(0, 3);
        waitWorker();
        assertEquals(PrepareProtocol.CANCELLED, PrepareProtocol.readStatus(status).state);
    }

    @Test
    public void installFailureIsReported() throws Exception {
        PrepareService.environmentOverride = (c, gameDir, titleId, runner) -> {
            throw new Toolchain.MissingAssetException("no toolkit.zip in this APK");
        };
        start();
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals(PrepareRunner.NO_TOOLS, s.code);
    }

    @Test
    public void cancelWithNothingRunningFixesAStaleStatus() throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = TID;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        ServiceController<PrepareService> sc =
                Robolectric.buildService(PrepareService.class, PrepareService.cancelIntent(app, TID)).create();
        sc.startCommand(0, 1);
        assertEquals(PrepareProtocol.CANCELLED,
                     PrepareProtocol.readStatus(PrepareService.statusFile(app, TID)).state);
    }

    // ---- Queue ----------------------------------------------------------------------------------------------------

    /** Each title: progress, a second's work, a valid pack under its title ID, ok. */
    private static final String PACK_SCRIPT = "echo 'XBA_PROGRESS step=1/5 id=identify pct=100 msg=ok'\n"
            + "sleep 1\n"
            + "mkdir -p \"$2\" && printf 'ELF!' > \"$2/libtitle.so\"\n"
            + "tid=$(basename \"$2\")\n"
            + "echo '{\"format\":1,\"title_id\":\"'$tid'\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":4}'"
            + " > \"$2/pack.json\"\n"
            + "echo \"XBA_RESULT ok pack=$2\"\n";

    /**
     * PACK_SCRIPT that waits for a go file next to its pack dir ("<pack>.go") before it starts: a test that looks at
     * the queue while the first title runs must not depend on how fast a loaded CI host runs a 1 s sleep (the first
     * title used to finish before the queue file was read, now and then).
     */
    private static final String GATED_PACK_SCRIPT = "i=0; while [ ! -e \"$2.go\" ] && [ $i -lt 1200 ]; do sleep 0.1;"
            + " i=$((i+1)); done\n" + PACK_SCRIPT;

    private void go(String tid) throws Exception {
        File f = new File(Pack.packDir(app.getFilesDir(), tid).getAbsolutePath() + ".go");
        f.getParentFile().mkdirs();
        TestSupport.write(f, "");
    }

    /** For a failure message: the title's state, code and the end of its log. */
    private String describe(String tid) {
        PrepareProtocol.Status st = status(tid);
        if (st == null)
            return "no status";
        String log = "";
        try {
            if (st.log != null) {
                String all = new String(java.nio.file.Files.readAllBytes(new File(st.log).toPath()));
                log = all.substring(Math.max(0, all.length() - 400));
            }
        } catch (Exception e) {
            log = "(log unreadable: " + e + ")";
        }
        return st.state + " " + st.code + " " + st.msg + " | log: " + log;
    }

    private PrepareProtocol.Status status(String tid) {
        return PrepareProtocol.readStatus(PrepareService.statusFile(app, tid));
    }

    private void waitState(String tid, String id) throws Exception {
        long until = System.currentTimeMillis() + 15000;
        while (System.currentTimeMillis() < until) {
            PrepareProtocol.Status s = status(tid);
            if (s != null && id.equals(s.id))
                return;
            Thread.sleep(50);
        }
        throw new AssertionError(tid + " never reached " + id);
    }

    @Test
    public void anotherTitleWaitsAndRunsNext() throws Exception {
        useScript(GATED_PACK_SCRIPT);
        ServiceController<PrepareService> sc = start();
        Thread first = PrepareService.workerThread();
        sc.withIntent(PrepareService.startIntent(app, "/games/b", TID2, "Second")).startCommand(0, 2);
        assertEquals("the running one goes on", first, PrepareService.workerThread());
        assertEquals(Collections.singletonList(TID2), PrepareService.queuedTitleIds());
        PrepareProtocol.Status q = status(TID2);
        assertNotNull(q);
        assertTrue(q.queued());
        PrepareProtocol.Queue file = PrepareProtocol.readQueue(PrepareProtocol.queueFile(app.getFilesDir()));
        assertEquals("the queue file while the first runs (first: " + describe(TID) + ")", 1, file.jobs.size());
        assertEquals("/games/b", file.jobs.get(0).gameDir);
        assertEquals("Second", file.jobs.get(0).title);
        // Starting a waiting title again, or the running one, changes nothing.
        sc.withIntent(PrepareService.startIntent(app, "/games/b", TID2, "Second")).startCommand(0, 3);
        sc.withIntent(PrepareService.startIntent(app, "/games/a", TID, "First")).startCommand(0, 4);
        assertEquals(Collections.singletonList(TID2), PrepareService.queuedTitleIds());

        go(TID);        // the second stays gated until its checks below are done
        waitWorker();   // the first ends; its end starts the next
        Thread second = PrepareService.workerThread();
        assertTrue("the next one started", second != first);
        assertEquals(PrepareProtocol.DONE, status(TID).state);
        assertTrue(PrepareService.queuedTitleIds().isEmpty());
        assertTrue(PrepareProtocol.readQueue(PrepareProtocol.queueFile(app.getFilesDir())).jobs.isEmpty());
        assertFalse("still running: not stopped", shadowOf(sc.get()).isStoppedBySelf());
        go(TID2);
        waitWorker();
        assertEquals(PrepareProtocol.DONE, status(TID2).state);
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID2));
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
        NotificationManager nm = app.getSystemService(NotificationManager.class);
        assertNotNull(shadowOf(nm).getNotification(TID, PrepareService.NOTIF_DONE_ID));
        assertNotNull(shadowOf(nm).getNotification(TID2, PrepareService.NOTIF_DONE_ID));
    }

    @Test
    public void nextRunsAfterAFailureToo() throws Exception {
        useScript("if [ \"$(basename \"$2\")\" = " + TID + " ]; then echo 'XBA_RESULT error code=BOOM msg=x'; exit 1; fi\n"
                  + PACK_SCRIPT);
        ServiceController<PrepareService> sc = start();
        sc.withIntent(PrepareService.startIntent(app, "/games/b", TID2, "Second")).startCommand(0, 2);
        waitWorker();
        waitWorker();
        assertEquals(PrepareProtocol.FAILED, status(TID).state);
        assertEquals(PrepareProtocol.DONE, status(TID2).state);
    }

    @Test
    public void cancelAWaitingTitle() throws Exception {
        useScript("echo 'XBA_PROGRESS step=3/5 id=lift pct=1 msg=forever'\nwhile :; do sleep 1; done\n");
        ServiceController<PrepareService> sc = start();
        waitState(TID, "lift");
        sc.withIntent(PrepareService.startIntent(app, "/games/b", TID2, "Second")).startCommand(0, 2);
        assertTrue(status(TID2).queued());
        sc.withIntent(PrepareService.cancelIntent(app, TID2)).startCommand(0, 3);
        assertTrue(PrepareService.queuedTitleIds().isEmpty());
        assertEquals(PrepareProtocol.CANCELLED, status(TID2).state);
        assertTrue("the running one goes on", PrepareService.busy());
        assertEquals("lift", status(TID).id);
        assertFalse(shadowOf(sc.get()).isStoppedBySelf());
        // A cancel for a title that neither runs nor waits does not stop the running one.
        sc.withIntent(PrepareService.cancelIntent(app, "99999999")).startCommand(0, 4);
        assertTrue(PrepareService.busy());
        sc.withIntent(PrepareService.cancelIntent(app, TID)).startCommand(0, 5);
        waitWorker();
        assertEquals(PrepareProtocol.CANCELLED, status(TID).state);
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
    }

    @Test
    public void destroyCancelsTheWaitingTitles() throws Exception {
        useScript("echo 'XBA_PROGRESS step=3/5 id=lift pct=1 msg=forever'\nwhile :; do sleep 1; done\n");
        ServiceController<PrepareService> sc = start();
        waitState(TID, "lift");
        sc.withIntent(PrepareService.startIntent(app, "/games/b", TID2, "Second")).startCommand(0, 2);
        sc.destroy();
        assertEquals(PrepareProtocol.CANCELLED, status(TID2).state);
        assertTrue(PrepareProtocol.readQueue(PrepareProtocol.queueFile(app.getFilesDir())).jobs.isEmpty());
        waitWorker();
        assertEquals(PrepareProtocol.CANCELLED, status(TID).state);
    }

    @Test
    public void aDeadProcessQueueIsDropped() throws Exception {
        // Left by a process that died: TID2 waited behind another Prepare.
        PrepareProtocol.Queue q = new PrepareProtocol.Queue();
        q.add(new PrepareProtocol.Job(TID2, "/games/b", "Second"));
        PrepareProtocol.writeQueue(PrepareProtocol.queueFile(app.getFilesDir()), q);
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.QUEUED;
        s.titleId = TID2;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID2), s);

        useScript(PACK_SCRIPT);
        start();
        assertEquals(PrepareProtocol.CANCELLED, status(TID2).state);
        assertTrue(PrepareService.queuedTitleIds().isEmpty());
        assertTrue(PrepareProtocol.readQueue(PrepareProtocol.queueFile(app.getFilesDir())).jobs.isEmpty());
        waitWorker();
        assertEquals(PrepareProtocol.DONE, status(TID).state);
        assertEquals("not run", PrepareProtocol.CANCELLED, status(TID2).state);
    }

    @Test
    public void cancelWithNothingRunningFixesAStaleQueuedStatus() throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.QUEUED;
        s.titleId = TID2;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID2), s);
        ServiceController<PrepareService> sc =
                Robolectric.buildService(PrepareService.class, PrepareService.cancelIntent(app, TID2)).create();
        sc.startCommand(0, 1);
        assertEquals(PrepareProtocol.CANCELLED, status(TID2).state);
    }

    /**
     * XBA-174: the ROM an N64 Prepare reads. A path goes as it is (tools.prepare reads any byte order in place); a
     * document is copied into the work folder as rom.z64 (its first word says the order), 64 MB at most.
     */
    @Test
    public void n64Source() throws Exception {
        File work = tmp.newFolder("work64");
        assertEquals("/sdcard/n64/Game.v64", PrepareService.romSource(app, "/sdcard/n64/Game.v64", work, null,
                                                                      PrepareService.N64_ROM_COPY,
                                                                      PrepareService.N64_ROM_MAX));
        assertEquals(64L << 20, PrepareService.N64_ROM_MAX);
        android.net.Uri u = android.net.Uri.parse(
            "content://com.android.externalstorage.documents/tree/primary%3An64/document/primary%3An64%2FGame.v64");
        byte[] rom = ConsoleTest.n64(1);
        org.robolectric.Shadows.shadowOf(app.getContentResolver())
            .registerInputStream(u, new java.io.ByteArrayInputStream(rom));
        String out = PrepareService.romSource(app, u.toString(), work, null, PrepareService.N64_ROM_COPY,
                                              PrepareService.N64_ROM_MAX);
        assertEquals(new File(work, "rom.z64").getAbsolutePath(), out);
        org.junit.Assert.assertArrayEquals(rom, java.nio.file.Files.readAllBytes(new File(out).toPath()));
        android.net.Uri big = android.net.Uri.parse(u + "2");
        org.robolectric.Shadows.shadowOf(app.getContentResolver())
            .registerInputStream(big, new java.io.ByteArrayInputStream(new byte[64]));
        try {
            PrepareService.romSource(app, big.toString(), work, null, PrepareService.N64_ROM_COPY, 32);
            org.junit.Assert.fail("over the limit");
        } catch (java.io.IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("N64"));
        }
    }

    /**
     * XBA-264: what a GameCube Prepare reads. A DOL or a plain ISO / GCM / CISO / GCZ path goes as it is; an RVZ / WIA
     * path, or any disc document (by its descriptor), goes through the native reader into &lt;work&gt;/disc.gcm; a DOL
     * document is copied (16 MB at most); without the native reader an RVZ fails with why.
     */
    @Test
    public void gcSource() throws Exception {
        File work = tmp.newFolder("work-gc");
        assertEquals("/sdcard/gc/Game.iso", PrepareService.gcSource(app, "/sdcard/gc/Game.iso", work, null));
        assertEquals("/sdcard/gc/Game.gcz", PrepareService.gcSource(app, "/sdcard/gc/Game.gcz", work, null));
        assertEquals("/sdcard/gc/hb.dol", PrepareService.gcSource(app, "/sdcard/gc/hb.dol", work, null));
        GcNative.Extractor saved = GcNative.extractor;
        final String[] seen = new String[2];
        try {
            GcNative.extractor = (path, out) -> {
                seen[0] = path;
                seen[1] = out;
                return "OK GALE01-0 3 4096000";
            };
            String got = PrepareService.gcSource(app, "/sdcard/gc/Game.rvz", work, null);
            assertEquals(new File(work, PrepareService.GC_SPARSE).getAbsolutePath(), got);
            assertEquals("/sdcard/gc/Game.rvz", seen[0]);
            assertEquals(got, seen[1]);
            GcNative.extractor = (path, out) -> "ERR RVZ: bad group table";
            try {
                PrepareService.gcSource(app, "/sdcard/gc/Bad.rvz", work, null);
                org.junit.Assert.fail("a bad RVZ");
            } catch (java.io.IOException e) {
                assertTrue(e.getMessage(), e.getMessage().contains("bad group table"));
            }
            GcNative.extractor = (path, out) -> null;
            try {
                PrepareService.gcSource(app, "/sdcard/gc/Game.WIA", work, null);
                org.junit.Assert.fail("no native reader");
            } catch (java.io.IOException e) {
                assertTrue(e.getMessage(), e.getMessage().contains("native disc reader"));
            }
        } finally {
            GcNative.extractor = saved;
        }
        android.net.Uri u = android.net.Uri.parse(
            "content://com.android.externalstorage.documents/tree/primary%3Agc/document/primary%3Agc%2Fhb.dol");
        byte[] dol = new byte[0x200];
        org.robolectric.Shadows.shadowOf(app.getContentResolver())
            .registerInputStream(u, new java.io.ByteArrayInputStream(dol));
        String out = PrepareService.romSource(app, u.toString(), work, null, PrepareService.GC_DOL_COPY,
                                              PrepareService.GC_DOL_MAX);
        assertEquals(new File(work, "game.dol").getAbsolutePath(), out);
        assertTrue(Console.GC.prepares());
    }

    /**
     * XBA-168: the disc a PS1 Prepare reads. A path goes as it is (tools.prepare opens a .cue and its tracks itself);
     * a .cue document's track documents come from the library index (its item's parts, under the same tree).
     */
    /**
     * XBA-168: a .chd goes through the native disc reader into &lt;work&gt;/disc.iso (its boot EXE's sectors), a path
     * as well as a document; without the reader a path is handed to tools.prepare as it is.
     */
    @Test
    public void ps1SourceChd() throws Exception {
        File work = tmp.newFolder("work-chd");
        byte[] exe = Ps1DiscTest.exe(6000);
        byte[] raw = Ps1DiscTest.disc(true, "BOOT=cdrom:\\GAME.EXE;1\n", "GAME.EXE;1", false, exe);
        Ps1Native.DiscOpener saved = Ps1Native.discOpener;
        final String[] opened = new String[1];
        final boolean[] closed = new boolean[1];
        try {
            Ps1Native.discOpener = path -> {
                opened[0] = path;
                return Ps1DiscTest.nativeDisc(raw, closed);
            };
            String got = PrepareService.ps1Source(app, "/sdcard/ps1/Game.CHD", "ps1-SLUS-00594", work, null);
            assertEquals(new File(work, "disc.iso").getAbsolutePath(), got);
            assertEquals("/sdcard/ps1/Game.CHD", opened[0]);
            assertTrue("the native disc is closed", closed[0]);
            byte[] img = java.nio.file.Files.readAllBytes(new File(got).toPath());
            byte[] boot = new byte[6000];
            System.arraycopy(img, 30 * 2048, boot, 0, 6000);
            org.junit.Assert.assertArrayEquals(exe, boot);
            // No native library (a JVM, an APK without it): the path, for tools.prepare's own reader.
            Ps1Native.discOpener = path -> null;
            assertEquals("/sdcard/ps1/Game.chd", PrepareService.ps1Source(app, "/sdcard/ps1/Game.chd",
                                                                          "ps1-SLUS-00594", work, null));
            // The reader refuses it: the Prepare fails with why.
            Ps1Native.discOpener = path -> {
                throw new java.io.IOException(path + ": not a disc image");
            };
            try {
                PrepareService.ps1Source(app, "/sdcard/ps1/Bad.chd", "ps1-X", work, null);
                org.junit.Assert.fail("a bad .chd");
            } catch (java.io.IOException e) {
                assertTrue(e.getMessage(), e.getMessage().contains("Bad.chd"));
            }
        } finally {
            Ps1Native.discOpener = saved;
        }
    }

    @Test
    public void ps1Source() throws Exception {
        File work = tmp.newFolder("work");
        assertEquals("/sdcard/ps1/Game.cue", PrepareService.ps1Source(app, "/sdcard/ps1/Game.cue", "ps1-SLUS-00594",
                                                                      work, null));
        String tree = "content://com.android.externalstorage.documents/tree/primary%3Aps1";
        LibraryIndex x = new LibraryIndex();
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = "doc:primary:ps1/Game.cue";
        it.name = "Game.cue";
        it.parts.add("doc:primary:ps1/Game (Track 1).bin");
        it.parts.add("doc:primary:ps1/Game (Track 2).bin");
        x.items.add(it);
        LibraryIndex.write(Library.indexFile(PrepareService.filesDir(app)), x);
        String cue = android.provider.DocumentsContract.buildDocumentUriUsingTree(android.net.Uri.parse(tree),
                                                                                  "primary:ps1/Game.cue").toString();
        assertEquals(java.util.Arrays.asList(
                         tree + "/document/primary%3Aps1%2FGame%20(Track%201).bin",
                         tree + "/document/primary%3Aps1%2FGame%20(Track%202).bin"),
                     PrepareService.cueParts(PrepareService.filesDir(app), cue));
        String other = android.provider.DocumentsContract.buildDocumentUriUsingTree(android.net.Uri.parse(tree),
                                                                                    "primary:ps1/Other.cue").toString();
        assertTrue(PrepareService.cueParts(PrepareService.filesDir(app), other).isEmpty());
        assertTrue(PrepareService.cueParts(PrepareService.filesDir(app), "/sdcard/x.cue").isEmpty());
    }
}
