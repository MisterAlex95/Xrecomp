package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.Context;

import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * XBA-336: « Send reports automatically ». Nothing leaves with the option off, a game run is one session counted
 * once, its status comes from how the process ended, crash and Prepare reports wait for the job, the bubble offering
 * the option shows once per version.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AutoReportsTest {
    private Context c;
    private final List<byte[]> posted = new ArrayList<>();
    private int answer = 200;
    private final List<AutoReports.Exit> exits = new ArrayList<>();
    private int dumps;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        assertTrue("the test build carries a DSN", AutoReports.possible(c));
        CrashReport.transport = (url, h, body) -> {
            posted.add(body);
            if (answer < 0)
                throw new java.io.IOException("offline");
            return answer;
        };
        AutoReports.exits = ctx -> new ArrayList<>(exits);
        AutoReports.dumper = (ctx, f) -> {
            dumps++;
            try {
                TestSupport.write(f, "# XRecomp: the game's last run\n"
                                     + "# game process 4242 ended 2026-11-21 10:00:00.000: native crash, status 11\n"
                                     + "backtrace:\n  #00 pc 0001234  /data/app/x/lib/arm64/libmain.so (xbox_draw+12)\n");
            } catch (java.io.IOException e) {
                return false;
            }
            return true;
        };
        AutoReports.collect(c, false);   // the first look only starts counting
    }

    @After
    public void tearDown() {
        CrashReport.transport = CrashReport.HTTP;
        AutoReports.exits = AutoReports.SYSTEM_EXITS;
        AutoReports.dumper = AutoReports.LOGCAT;
        Telemetry.setAuto(c, false);
        wipe(Telemetry.dir(c));
        wipe(CrashReport.queueDir(c));
    }

    private static void wipe(File d) {
        File[] f = d.listFiles();
        if (f != null)
            for (File x : f) {
                if (x.isDirectory())
                    wipe(x);
                x.delete();
            }
        d.delete();
    }

    /** A game launched at start (GameStore's session), its process ended at end with this reason. */
    private void run(long start, long end, int pid, String reason, int status) {
        GameStore.sessionStart(c, start, "4D530004");
        exits.add(0, new AutoReports.Exit(pid, end, reason, status));   // newest first
    }

    private static long now() {
        return System.currentTimeMillis();
    }

    private static JSONObject session(byte[] body) throws Exception {
        List<Object[]> it = CrashReportTest.items(body);
        assertEquals("session", ((JSONObject)it.get(1)[0]).getString("type"));
        return new JSONObject(new String((byte[])it.get(1)[1], StandardCharsets.UTF_8));
    }

    private static String itemType(byte[] body) throws Exception {
        return ((JSONObject)CrashReportTest.items(body).get(1)[0]).getString("type");
    }

    @Test
    public void nothingLeavesWithTheOptionOff() {
        long t = now();
        run(t + 1000, t + 5000, 100, "native crash", 0);
        AutoReports.Run r = AutoReports.collect(c, false);
        assertNotNull(r);
        assertTrue(r.failed());   // the launcher may offer the option...
        assertEquals(0, AutoReports.pending(c));   // ...but keeps nothing to send
        assertEquals(0, dumps);
        assertFalse(AutoReports.flush(c));
        assertNull(Telemetry.installId(c));
        assertTrue(posted.isEmpty());
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.FAILED;
        s.code = "RECOMP_FAILED";
        s.updatedAt = t + 9000;
        assertTrue(AutoReports.prepareFailed(c, "4D530004", "Halo", s, null));
        assertEquals(0, AutoReports.pending(c));
        assertTrue(c.getSystemService(JobScheduler.class).getAllPendingJobs().isEmpty());
        assertEquals(0, CrashReport.queued(c).length);
    }

    @Test
    public void theBubbleShowsOncePerVersion() throws Exception {
        assertTrue(AutoReports.offerDue(c));
        AutoReports.offered(c);
        assertFalse(AutoReports.offerDue(c));
        TestSupport.write(new File(Telemetry.dir(c), "offered"), "0.0.1");   // an older version's
        assertTrue(AutoReports.offerDue(c));
        Telemetry.setAuto(c, true);
        assertFalse(AutoReports.offerDue(c));   // nothing to offer while it is on
    }

    @Test
    public void statusFromEachEnd() {
        assertEquals("crashed", AutoReports.status("native crash", 0, false));
        assertEquals("crashed", AutoReports.status("Java exception", 0, false));
        assertEquals("exited", AutoReports.status("exited", 0, false));
        assertEquals("abnormal", AutoReports.status("exited", 1, false));
        assertEquals("exited", AutoReports.status("stopped by the user", 0, false));
        assertEquals("exited", AutoReports.status("killed by a signal", 9, true));
        assertEquals("abnormal", AutoReports.status("killed by a signal", 9, false));
        assertEquals("abnormal", AutoReports.status("not responding", 0, false));
        assertEquals("abnormal", AutoReports.status("killed for memory", 0, false));
        assertEquals("abnormal", AutoReports.status(null, 0, false));
    }

    @Test
    public void aSessionPerRunFromHowItEnded() throws Exception {
        Telemetry.setAuto(c, true);
        String[][] cases = { { "exited", "0", "exited" }, { "native crash", "11", "crashed" },
                             { "killed for memory", "0", "abnormal" } };
        long t = now() + 1000;
        for (String[] k : cases) {
            posted.clear();
            run(t, t + 30_000, 7, k[0], Integer.parseInt(k[1]));
            assertNotNull(AutoReports.collect(c, false));
            assertFalse(AutoReports.flush(c));
            JSONObject s = null;
            int reports = 0;
            for (byte[] b : posted)
                if (itemType(b).equals("session"))
                    s = session(b);
                else
                    reports++;
            assertNotNull(s);
            assertEquals(k[2], s.getString("status"));
            assertTrue(s.getBoolean("init"));
            assertEquals(30.0, s.getDouble("duration"), 1e-9);
            assertEquals(Telemetry.installId(c), s.getString("did"));
            assertEquals(CrashReport.release(c), s.getJSONObject("attrs").getString("release"));
            assertEquals(k[2].equals("crashed") ? 1 : 0, s.getInt("errors"));
            assertEquals(k[2].equals("crashed") ? 1 : 0, reports);   // a crash's report leaves too
            // only sid, did, init, started, timestamp, status, errors, duration, attrs (release, environment)
            assertEquals(9, s.length());
            assertEquals(2, s.getJSONObject("attrs").length());
            t += 60_000;
        }
        assertEquals(0, AutoReports.pending(c));
        assertEquals(0, CrashReport.queued(c).length);   // never the tap path's queue
    }

    @Test
    public void aRunIsSentOnce() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        run(t, t + 5000, 8, "exited", 0);
        assertNotNull(AutoReports.collect(c, false));
        assertNull(AutoReports.collect(c, false));   // already counted
        AutoReports.flush(c);
        AutoReports.flush(c);
        assertEquals(1, posted.size());
        assertNull(AutoReports.collect(c, false));
        assertEquals(1, posted.size());
        // A newer end without a new launch (a library check's game, a process the system restarted): not a run.
        exits.add(0, new AutoReports.Exit(80, t + 50_000, "native crash", 0));
        assertNull(AutoReports.collect(c, false));
        assertEquals(0, AutoReports.pending(c));
        assertEquals(0, dumps);
    }

    @Test
    public void aRunWithoutAnEndIsNotOver() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        GameStore.sessionStart(c, t, "4D530004");
        exits.add(new AutoReports.Exit(9, t - 500, "exited", 0));   // the run before
        assertNull(AutoReports.collect(c, false));
        run(t, t + 4000, 10, "exited", 0);
        assertNull(AutoReports.collect(c, true));   // still playing
        assertNotNull(AutoReports.collect(c, false));
        assertEquals(1, AutoReports.pending(c));
    }

    @Test
    public void runsBeforeTheOptionWasOnStayHome() throws Exception {
        long t = now() - 60_000;
        TestSupport.write(new File(Telemetry.dir(c), "counted"), String.valueOf(t - 1));
        run(t, t + 5000, 11, "native crash", 0);   // ended while the option was off, not looked at yet
        Telemetry.setAuto(c, true);
        assertNull(AutoReports.collect(c, false));
        assertEquals(0, AutoReports.pending(c));
    }

    @Test
    public void theLaunchersOwnKillAndAQuickRebootAreOneCleanRun() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        GameStore.sessionStart(c, t, "4D530004");
        AutoReports.launcherEnded(c, 12, t);   // a quick reboot ends the first process...
        exits.add(0, new AutoReports.Exit(12, t + 2000, "killed by a signal", 9));
        assertNull(AutoReports.collect(c, true));   // ...the second one plays
        exits.add(0, new AutoReports.Exit(13, t + 9000, "exited", 0));
        AutoReports.Run r = AutoReports.collect(c, false);
        assertNotNull(r);
        assertEquals(1, AutoReports.pending(c));
        assertEquals(9000, r.end - r.start);
        // A game left in the background, ended by the launcher to start another one (lastPlayed moves on)
        long t2 = t + 20_000;
        AutoReports.launcherEnded(c, 14, t2);
        GameStore.sessionStart(c, t2 + 10_000, "4D530005");
        exits.add(0, new AutoReports.Exit(14, t2 + 9_000, "killed by a signal", 9));
        r = AutoReports.collect(c, true);
        assertNotNull(r);
        assertEquals("exited", r.status);
        assertEquals(2, AutoReports.pending(c));
    }

    @Test
    public void turnedOffBeforeTheJobNothingLeaves() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        run(t, t + 5000, 15, "native crash", 0);
        AutoReports.collect(c, false);
        assertEquals(2, AutoReports.pending(c));   // the session and the crash report
        assertFalse(c.getSystemService(JobScheduler.class).getAllPendingJobs().isEmpty());
        Telemetry.setAuto(c, false);
        assertEquals(0, AutoReports.pending(c));
        assertTrue(c.getSystemService(JobScheduler.class).getAllPendingJobs().isEmpty());
        assertFalse(AutoReports.flush(c));
        assertTrue(posted.isEmpty());
    }

    @Test
    public void theSessionCarriesTheInstallIdOfWhenItLeaves() throws Exception {
        Telemetry.setAuto(c, true);
        String before = Telemetry.installId(c);
        long t = now() + 1000;
        run(t, t + 5000, 16, "exited", 0);
        AutoReports.collect(c, false);
        Telemetry.resetInstallId(c);
        String after = Telemetry.installId(c);
        assertNotEquals(before, after);
        AutoReports.flush(c);
        assertEquals(1, posted.size());
        assertEquals(after, session(posted.get(0)).getString("did"));
        for (File f : Telemetry.dir(c).listFiles())
            if (f.isFile() && !f.getName().equals("auto"))
                assertFalse(f.getName(), new String(java.nio.file.Files.readAllBytes(f.toPath()),
                                                    StandardCharsets.UTF_8).contains(after));
    }

    @Test
    public void whatCouldNotLeaveWaitsAndTheJobTriesAgain() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        run(t, t + 5000, 17, "exited", 0);
        AutoReports.collect(c, false);
        answer = -1;   // offline
        assertTrue(AutoReports.flush(c));
        assertEquals(1, AutoReports.pending(c));
        assertEquals(0, CrashReport.queued(c).length);
        answer = 429;   // the gate closes: waits, no retry loop
        assertFalse(AutoReports.flush(c));
        assertEquals(1, AutoReports.pending(c));
    }

    @Test
    public void aFailedPrepareIsReportedOnceWithTheOptionOn() throws Exception {
        Telemetry.setAuto(c, true);
        File log = File.createTempFile("prepare", ".log");
        TestSupport.write(log, "error in /storage/emulated/0/Games/Halo/default.xbe\n");
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.FAILED;
        s.code = "RECOMP_FAILED";
        s.msg = "clang failed";
        s.updatedAt = now() + 1000;
        assertTrue(AutoReports.prepareFailed(c, "4D530004", "Halo", s, log));
        assertFalse(AutoReports.prepareFailed(c, "4D530004", "Halo", s, log));   // the screen rebuilt
        assertEquals(1, AutoReports.pending(c));
        PrepareProtocol.Status other = new PrepareProtocol.Status();
        other.state = PrepareProtocol.FAILED;
        other.updatedAt = s.updatedAt + 1;
        assertTrue(AutoReports.prepareFailed(c, "n64-mario", "Mario", other, null));
        assertFalse(AutoReports.prepareFailed(c, "4D530004", "Halo", s, log));   // two titles seen in turn
        assertFalse(AutoReports.prepareFailed(c, "n64-mario", "Mario", other, null));
        assertEquals(2, AutoReports.pending(c));
        assertTrue(new File(new File(Telemetry.dir(c), "outbox"), other.updatedAt + "-prepare.envelope").delete());
        assertEquals(1, AutoReports.pending(c));
        AutoReports.flush(c);
        assertEquals(1, posted.size());
        String body = new String(posted.get(0), StandardCharsets.UTF_8);
        assertTrue(body.contains("PrepareFailed"));
        assertFalse(body.contains("/storage/"));   // scrubbed as « Send report » is
        log.delete();
    }

    @Test
    public void aQueuedOrDonePrepareIsNotAFailure() {
        Telemetry.setAuto(c, true);
        for (String state : new String[] { PrepareProtocol.QUEUED, PrepareProtocol.DONE, PrepareProtocol.RUNNING,
                                           PrepareProtocol.CANCELLED }) {
            PrepareProtocol.Status s = new PrepareProtocol.Status();
            s.state = state;
            s.updatedAt = now() + 1000;
            assertFalse(state, AutoReports.prepareFailed(c, "4D530004", "Halo", s, null));
        }
        assertEquals(0, AutoReports.pending(c));
    }

    @Test
    public void theJobWaitsForWifiUnlessMobileDataIsAllowed() {
        Telemetry.setAuto(c, true);
        long t = now() + 1000;
        run(t, t + 5000, 18, "exited", 0);
        AutoReports.collect(c, false);
        JobScheduler js = c.getSystemService(JobScheduler.class);
        JobInfo j = js.getPendingJob(AutoReports.JOB_ID);
        assertNotNull(j);
        assertEquals(JobInfo.NETWORK_TYPE_UNMETERED, j.getNetworkType());
        AutoReports.setMobileData(c, true);
        assertEquals(JobInfo.NETWORK_TYPE_ANY, js.getPendingJob(AutoReports.JOB_ID).getNetworkType());
    }
}
