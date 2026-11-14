package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.app.Service;
import android.content.Intent;
import android.os.Looper;

import org.json.JSONObject;
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

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/**
 * XBA-128: a Prepare kind to the device and resumable. The thermal policy shared with the library check, the device
 * log PrepareMonitor writes for tools.prepare, a Prepare whose process died started again (and given up after
 * MAX_RESUMES), the compilers a dead process left behind ended, the quick first build's -O2 rebuild, and what the
 * pack's « prepare » object says in Manage and in the check's report line.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareDeviceTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String TID = "12345678";
    /** A valid pack, then ok; the command line's -O and jobs go to the log (the factory below passes them). */
    private static final String PACK_SCRIPT = "echo \"opt $3 jobs $1\"\n"
            + "echo 'XBA_PROGRESS step=4/5 id=compile pct=50 msg=x'\n"
            + "mkdir -p \"$2\" && printf 'ELF!' > \"$2/libtitle.so\"\n"
            + "echo '{\"format\":1,\"title_id\":\"" + TID + "\",\"shell_abi\":1,\"lib\":\"libtitle.so\",\"lib_size\":4,"
            + "\"abi\":\"arm64-v8a\",\"entry\":\"recomp_title_main\"}' > \"$2/pack.json\"\n"
            + "echo \"XBA_RESULT ok pack=$2\"\n";

    static final class FakeProbe implements PrepareMonitor.Probe {
        volatile int thermal;
        volatile boolean game;

        @Override
        public int thermal() {
            return thermal;
        }

        @Override
        public float tempC() {
            return 38.5f;
        }

        @Override
        public boolean gameRunning() {
            return game;
        }
    }

    private final FakeProbe probe = new FakeProbe();

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
        PrepareService.probeOverride = probe;
    }

    @After
    public void tearDown() throws Exception {
        PrepareService.environmentOverride = null;
        PrepareService.probeOverride = null;
        PrepareService.forgetQueue();
        Thread w = PrepareService.workerThread();
        if (w != null)
            w.join(20000);
    }

    private void useScript(String body) throws Exception {
        File script = tmp.newFile("fake_prepare.sh");
        TestSupport.write(script, body);
        File files = app.getFilesDir();
        PrepareService.environmentOverride = (c, gameDir, titleId, runner) -> new PrepareRunner.CommandFactory() {
            @Override
            public List<String> command(int jobs) {
                return TestSupport.sh(script, String.valueOf(jobs), Pack.packDir(files, titleId).getAbsolutePath(),
                                      String.valueOf(runner.opt()));
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
    }

    private ServiceController<PrepareService> start(int flags) {
        Intent i = PrepareService.startIntent(app, tmp.getRoot().getAbsolutePath(), TID, "Test title");
        ServiceController<PrepareService> sc = Robolectric.buildService(PrepareService.class, i).create();
        sc.startCommand(flags, 1);
        return sc;
    }

    private void waitWorker() throws Exception {
        for (int i = 0; i < 3; i++) {   // a queued job starts from the main looper after the first one ends
            Thread w = PrepareService.workerThread();
            assertNotNull(w);
            w.join(30000);
            assertFalse(w.isAlive());
            shadowOf(Looper.getMainLooper()).idle();
            if (PrepareService.workerThread() == w || !PrepareService.busy())
                break;
        }
    }

    private String log() throws Exception {
        return PrepareProtocol.readText(PrepareService.logFile(app, TID), 1 << 20);
    }

    // ---- The policy and the device log ---------------------------------------------------------------------------

    @Test
    public void jobsDropWhenHotAndComeBack() {
        assertEquals(4, DeviceGuard.jobsFor(0, 4));
        assertEquals(4, DeviceGuard.jobsFor(DeviceGuard.THERMAL_MODERATE, 4));
        assertEquals(1, DeviceGuard.jobsFor(DeviceGuard.THERMAL_SEVERE, 4));
        assertEquals(0, DeviceGuard.jobsFor(DeviceGuard.THERMAL_CRITICAL, 4));
        assertEquals(0, DeviceGuard.jobsFor(6, 4));
        assertTrue(DeviceGuard.hot(3) && !DeviceGuard.hot(2) && DeviceGuard.cooled(2) && !DeviceGuard.cooled(3));
        assertTrue(DeviceGuard.batteryLow(15, false) && !DeviceGuard.batteryLow(15, true)
                   && !DeviceGuard.batteryLow(-1, false));
        assertTrue(DeviceGuard.batteryBack(30, false) && DeviceGuard.batteryBack(5, true));
        // The check uses the same thresholds.
        assertEquals(DeviceGuard.THERMAL_SEVERE, CheckDriver.THERMAL_SEVERE);
    }

    @Test
    public void theMonitorWritesTheDeviceLog() throws Exception {
        File work = tmp.newFolder("work");
        PrepareMonitor m = new PrepareMonitor(work, 4, false, probe);
        assertEquals(4, m.sample(1_000_000));
        probe.thermal = 3;
        assertEquals(1, m.sample(1_005_000));
        probe.thermal = 4;
        assertEquals(0, m.sample(1_010_000));
        String[] lines = PrepareProtocol.readText(PrepareMonitor.logFile(work), 1 << 16).trim().split("\n");
        assertEquals(3, lines.length);
        JSONObject o = new JSONObject(lines[1]);
        assertEquals(1005, o.getLong("t"));
        assertEquals(3, o.getInt("thermal"));
        assertEquals(1, o.getInt("jobs"));
        assertEquals(38.5, o.getDouble("temp_c"), 0.01);
        // A background Prepare holds while the game runs.
        PrepareMonitor bg = new PrepareMonitor(work, 4, true, probe);
        probe.thermal = 0;
        probe.game = true;
        assertEquals(0, bg.sample(1_015_000));
        probe.game = false;
        assertEquals(4, bg.sample(1_020_000));
    }

    @Test
    public void aPrepareRunsWithItsDeviceLog() throws Exception {
        useScript(PACK_SCRIPT);
        probe.thermal = 3;
        start(0);
        waitWorker();
        File dl = PrepareMonitor.logFile(PrepareService.workDir(app, TID));
        assertTrue("the monitor sampled", dl.isFile());
        assertTrue(PrepareProtocol.readText(dl, 1 << 16).contains("\"jobs\":1"));
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.DONE, s.state);
        assertEquals(2, s.opt);
        assertTrue(log(), log().contains("opt 2 jobs"));
    }

    // ---- Resumable ---------------------------------------------------------------------------------------------------

    private void runningStatus(int resumes) throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = TID;
        s.resumes = resumes;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        TestSupport.write(PrepareService.logFile(app, TID), "the first attempt's log\n");
    }

    @Test
    public void aPrepareWhoseProcessDiedIsStartedAgain() throws Exception {
        useScript(PACK_SCRIPT);
        runningStatus(0);
        start(Service.START_FLAG_REDELIVERY);
        waitWorker();
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.DONE, s.state);
        assertEquals(1, s.resumes);
        String log = log();
        assertTrue("the first attempt's log is kept", log.startsWith("the first attempt's log"));
        assertTrue(log, log.contains("resumed after the Prepare's process died (1)"));
    }

    @Test
    public void aPrepareKilledTooOftenIsReported() throws Exception {
        useScript(PACK_SCRIPT);
        runningStatus(PrepareService.MAX_RESUMES);
        ServiceController<PrepareService> sc = start(Service.START_FLAG_REDELIVERY);
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
        assertEquals(PrepareProtocol.FAILED, s.state);
        assertEquals(PrepareRunner.KILLED, s.code);
        assertFalse(PrepareService.busy());
        assertTrue(shadowOf(sc.get()).isStoppedBySelf());
    }

    @Test
    public void aRedeliveredStartOfAFinishedPrepareDoesNothing() throws Exception {
        useScript(PACK_SCRIPT);
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = TID;
        s.state = PrepareProtocol.DONE;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        start(Service.START_FLAG_REDELIVERY);
        assertFalse(PrepareService.busy());
        assertEquals(PrepareProtocol.DONE, PrepareProtocol.readStatus(PrepareService.statusFile(app, TID)).state);
    }

    @Test
    public void theCompilersADeadProcessLeftAreEnded() throws Exception {
        org.junit.Assume.assumeTrue(new File("/usr/bin/setsid").canExecute() || new File("/bin/setsid").canExecute());
        // A process group of its own, as tools.prepare's under setsid, still running.
        Process orphan = new ProcessBuilder("setsid", "sh", "-c", "echo $$; exec sleep 60").start();
        String pgid = new BufferedReader(new InputStreamReader(orphan.getInputStream())).readLine().trim();
        File dir = tmp.newFolder("status");
        TestSupport.write(new File(dir, "pgid"), pgid);
        PrepareRunner r = new PrepareRunner(dir, TID, 1, null, null, null);
        r.killOrphans();
        assertTrue("ended", orphan.waitFor(10, TimeUnit.SECONDS));
        assertFalse(r.pgidFile().exists());
    }

    // ---- The quick first build (a switch, off) -----------------------------------------------------------------------

    @Test
    public void quickFirstBuildsAtO1ThenO2InTheBackground() throws Exception {
        useScript(PACK_SCRIPT);
        TestSupport.write(new File(new File(app.getFilesDir(), "prepare"), PrepareService.QUICK_FIRST), "");
        try {
            start(0);
            waitWorker();
            String log = log();
            // The -O2 rebuild ran after the -O1 build (its log replaced the first: a new Prepare).
            assertTrue(log, log.contains("opt 2 jobs"));
            PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, TID));
            assertEquals(PrepareProtocol.DONE, s.state);
            assertEquals(2, s.opt);
            assertTrue("the rebuild runs in the background", s.background);
        } finally {
            //noinspection ResultOfMethodCallIgnored
            new File(new File(app.getFilesDir(), "prepare"), PrepareService.QUICK_FIRST).delete();
        }
    }

    // ---- What the pack says ------------------------------------------------------------------------------------------

    private void packWithMetrics() throws Exception {
        File dir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        File json = new File(dir, Pack.JSON);
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("prepare", new JSONObject("{\"total_s\": 372.4, \"steps_s\": {\"identify\": 2.1, \"analyse\": 61.0,"
                + " \"lift\": 48.2, \"compile\": 255.0, \"pack\": 6.1}, \"skipped\": [], \"jobs\": 4, \"opt\": \"2\","
                + " \"files\": 163, \"compiled\": 163, \"c_mb\": 256.0, \"obj_mb\": 149.0, \"pack_mb\": 61.2,"
                + " \"cpus\": 8, \"peak_rss_mb\": {\"max\": 589, \"p90\": 302, \"median\": 163},"
                + " \"thermal\": {\"samples\": 74, \"max\": 3, \"severe_s\": 95, \"jobs_min\": 1,"
                + " \"temp_c_max\": 41.5}}"));
        TestSupport.write(json, o.toString(1));
    }

    @Test
    public void manageAndTheReportSayWhatThePrepareCost() throws Exception {
        packWithMetrics();
        List<String> lines = PrepareFacts.lines(app, app.getFilesDir(), TID);
        assertEquals(3, lines.size());
        assertTrue(lines.get(0), lines.get(0).contains("6 min 12 s · 4 jobs · -O2 (163 files, 163 compiled)"));
        assertTrue(lines.get(1), lines.get(1).contains("589 MB at most, 163 MB typical"));
        assertTrue(lines.get(2), lines.get(2).contains("1 min 35 s at severe or above"));
        List<String> f = PrepareFacts.reportFields(app.getFilesDir(), TID, false);
        assertTrue(f.toString(), f.contains("prepare_s=372") && f.contains("prepare_peak_mb=589")
                                 && f.contains("pack_mb=61.2") && f.contains("prepare_thermal_max=3")
                                 && f.contains("prepare_temp_c_max=41.5") && f.contains("prepare_opt=\"2\""));
        assertFalse("the check's own timing wins", PrepareFacts.reportFields(app.getFilesDir(), TID, true).toString()
                .contains("prepare_s="));
        // A pack from before XBA-128 says nothing.
        TestSupport.writePack(app.getFilesDir(), "4541005B", 64);
        assertTrue(PrepareFacts.lines(app, app.getFilesDir(), "4541005B").isEmpty());
        assertTrue(PrepareFacts.reportFields(app.getFilesDir(), "4541005B", false).isEmpty());
        assertEquals(new ArrayList<String>(), PrepareFacts.reportFields(app.getFilesDir(), "00000000", false));
    }
}
