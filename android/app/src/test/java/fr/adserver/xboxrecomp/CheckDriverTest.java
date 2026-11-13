package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The library check's steps (CheckDriver) with a fake device and a fake clock: Prepare then boot then grade per
 * title, the pad script on the status clock, the watchdog, a crash, a Prepare that fails, a title not imported, the
 * player leaving during a boot, a severe thermal status, a low battery, and a restart of the check in the middle of a
 * boot (graded, then the next title: a title that takes the device down cannot loop).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class CheckDriverTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String A = "58420001", B = "58420002";

    /** The device: Prepares that finish when told, a game process that runs until killed or told to crash. */
    final class Fake implements CheckDriver.Platform {
        long now = 1_000_000;
        File files;
        final Set<String> prepared = new HashSet<>();
        final Map<String, PrepareProtocol.Status> status = new HashMap<>();
        final List<String> log = new ArrayList<>();
        final List<CheckDriver.Grade> grades = new ArrayList<>();
        boolean prepareAlive, canStart = true, inFront = true, window = true, charging = true;
        int thermal, battery = 80, pid, nextPid = 100;
        String launched;
        CheckDriver.Exit exit;
        int changes;

        @Override
        public long now() {
            return now;
        }

        @Override
        public File filesDir() {
            return files;
        }

        @Override
        public boolean needsPrepare(LibraryCheck.Title t) {
            return !prepared.contains(t.tid);
        }

        @Override
        public void startPrepare(LibraryCheck.Title t) throws Exception {
            if (!canStart)
                throw new IllegalStateException("not allowed from the background");
            log.add("prepare " + t.tid);
            PrepareProtocol.Status s = new PrepareProtocol.Status();
            s.titleId = t.tid;
            status.put(t.tid, s);
            prepareAlive = true;
        }

        @Override
        public void cancelPrepare(String tid) {
            log.add("cancel " + tid);
            PrepareProtocol.Status s = status.get(tid);
            if (s != null)
                s.state = PrepareProtocol.CANCELLED;
            prepareAlive = false;
        }

        @Override
        public PrepareProtocol.Status prepareStatus(String tid) {
            return status.get(tid);
        }

        @Override
        public boolean prepareAlive() {
            return prepareAlive;
        }

        @Override
        public boolean canStart() {
            return canStart;
        }

        @Override
        public void launchGame(LibraryCheck.Title t) {
            log.add("launch " + t.tid);
            launched = t.tid;
            pid = nextPid++;
            canStart = false;   // the game is in front now
            inFront = true;
            window = true;
        }

        @Override
        public int gamePid() {
            return pid;
        }

        @Override
        public boolean gameInFront() {
            return inFront;
        }

        @Override
        public boolean gameWindow() {
            return window;
        }

        @Override
        public void bringCheckToFront() {
            log.add("front");
            canStart = true;
        }

        @Override
        public void endGame() {
            if (pid != 0)
                log.add("end " + pid);
            pid = 0;
        }

        @Override
        public CheckDriver.Exit exitSince(long since) {
            return exit != null && exit.time >= since ? exit : null;
        }

        @Override
        public long peakKb(int p) {
            return 2_000_000;
        }

        @Override
        public void dumpLog(int p, File out) {
            log.add("log " + p);
        }

        @Override
        public JSONObject grade(CheckDriver.Grade g) throws Exception {
            grades.add(g);
            String stage = g.prepareFailed != null ? "none" : g.notBooted != null ? "prepares"
                           : g.exit.equals("none") ? "renders" : "boots";
            return new JSONObject().put("stage", stage).put("blocker", g.exit.equals("none") && g.prepareFailed == null
                                                                        && g.notBooted == null ? JSONObject.NULL
                                                                       : "something");
        }

        @Override
        public int thermalStatus() {
            return thermal;
        }

        @Override
        public int batteryPercent() {
            return battery;
        }

        @Override
        public boolean charging() {
            return charging;
        }

        @Override
        public void changed(LibraryCheck.State s) {
            changes++;
        }

        /** The runtime's status line at ms since its start (the game writes it). */
        void statusLine(String tid, long ms) throws IOException {
            File f = new File(LibraryCheck.runDir(files, tid), "status.jsonl");
            try (FileOutputStream o = new FileOutputStream(f, true)) {
                o.write(("{\"ms\":" + ms + ",\"flips\":" + ms / 16 + "}\n").getBytes(StandardCharsets.UTF_8));
            }
        }

        void finishPrepare(String tid, String state, String code) {
            PrepareProtocol.Status s = status.get(tid);
            s.state = state;
            s.code = code;
            prepareAlive = false;
            if (PrepareProtocol.DONE.equals(state))
                prepared.add(tid);
        }
    }

    Fake p;

    @Before
    public void setUp() throws Exception {
        p = new Fake();
        p.files = tmp.newFolder("files");
    }

    static LibraryCheck.Title title(String tid, String name, String dir) {
        LibraryCheck.Title t = new LibraryCheck.Title();
        t.tid = tid;
        t.title = name;
        t.gameDir = dir;
        t.source = dir.isEmpty() ? "/cache/" + tid : dir;
        return t;
    }

    CheckDriver driver(LibraryCheck.Title... titles) throws IOException {
        LibraryCheck.State s = LibraryCheck.start(Arrays.asList(titles), p.now);
        LibraryCheck.write(p.files, s);
        return new CheckDriver(p, s);
    }

    /** Ticks every 500 ms of fake time for ms. */
    void run(CheckDriver d, long ms) {
        for (long t = 0; t < ms; t += 500) {
            d.tick();
            p.now += 500;
        }
    }

    @Test
    public void preparesBootsGradesEachTitleAndWritesTheState() throws Exception {
        p.prepared.add(B);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"), title(B, "Tiny Tanks", "/games/b"));
        d.tick();
        assertEquals(Arrays.asList("prepare " + A), p.log);
        assertEquals(LibraryCheck.PREPARING, d.s.current().phase);
        run(d, 60_000);   // the Prepare runs: nothing else happens
        assertEquals(LibraryCheck.PREPARING, d.s.current().phase);
        p.finishPrepare(A, PrepareProtocol.DONE, "");
        d.tick();
        assertEquals(LibraryCheck.READY, d.s.current().phase);
        assertEquals(60_000, d.s.current().prepareMs, 1000);
        d.tick();
        assertEquals("launch " + A, p.log.get(p.log.size() - 1));
        assertEquals(LibraryCheck.BOOTING, d.s.current().phase);
        // current.json names the run for ShellActivity.
        assertNotNull(LibraryCheck.currentEnv(p.files, A, p.now, null));
        // The runtime starts 2 s after the launch; its status clock runs from there.
        run(d, 2000);
        for (long ms = 1000; ms <= 30_000; ms += 1000) {
            run(d, 1000);
            p.statusLine(A, ms);
        }
        String pad = new String(Files.readAllBytes(new File(LibraryCheck.runDir(p.files, A), "pad-live.txt").toPath()),
                                StandardCharsets.US_ASCII);
        assertTrue(pad, pad.startsWith("start\nstart\nstart\na\n"));
        run(d, 160_000);
        // 180 s after the launch: the check screen to the front, the game ended by PID, its log, the grade.
        assertTrue(p.log.toString(), p.log.containsAll(Arrays.asList("front", "end 100", "log 100")));
        CheckDriver.Grade g = p.grades.get(0);
        assertEquals("none", g.exit);
        assertEquals(180, g.runS, 1);
        assertEquals(2_000_000, g.peakKb);
        assertEquals(60_000, g.prepareMs, 1000);
        List<String> presses = Arrays.asList(g.presses.split(","));
        assertTrue(presses.size() > 10);
        // The first press 8 s into the status clock (found from its first line), within a tick.
        assertEquals(8000, Long.parseLong(presses.get(0)), 600);
        assertEquals(LibraryCheck.GRADED, d.s.titles.get(0).phase);
        assertEquals("renders", d.s.titles.get(0).stage);
        assertNull(LibraryCheck.currentEnv(p.files, A, p.now, null));
        // B: already prepared, booted at once (no Prepare), crashes after 30 s.
        p.canStart = true;
        run(d, 1000);
        assertEquals("launch " + B, p.log.get(p.log.size() - 1));
        assertEquals(-1, d.s.current().prepareMs);
        run(d, 30_000);
        p.pid = 0;
        p.exit = new CheckDriver.Exit("crash:11", p.now);
        run(d, 5000);
        assertEquals("crash:11", p.grades.get(1).exit);
        assertTrue(p.log.contains("log 101"));
        run(d, 1000);
        assertEquals(LibraryCheck.DONE, d.s.state);
        assertEquals(LibraryCheck.DONE, LibraryCheck.read(p.files).state);
        assertEquals(Integer.valueOf(1), d.s.counts().get("renders"));
        assertEquals(Integer.valueOf(1), d.s.counts().get("boots"));
    }

    @Test
    public void aPrepareThatFailsIsGradedNone() throws Exception {
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        d.tick();
        p.finishPrepare(A, PrepareProtocol.FAILED, "COMPILE_FAILED");
        p.status.get(A).msg = "gen_3.c: error";
        d.tick();
        assertEquals("COMPILE_FAILED: gen_3.c: error", p.grades.get(0).prepareFailed);
        assertEquals("none", d.s.titles.get(0).stage);
        assertFalse(p.log.toString().contains("launch"));
    }

    @Test
    public void anInterruptedPrepareStartsAgainThenIsReported() throws Exception {
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        for (int i = 0; i < CheckDriver.MAX_PREPARE_ATTEMPTS; i++) {
            d.tick();   // start
            p.finishPrepare(A, PrepareProtocol.CANCELLED, "KILLED");
            run(d, 6000);
        }
        assertEquals(CheckDriver.MAX_PREPARE_ATTEMPTS, p.log.stream().filter(l -> l.startsWith("prepare")).count());
        assertEquals("KILLED: interrupted 3 times", p.grades.get(0).prepareFailed);
    }

    @Test
    public void aTitleNotImportedIsPreparedNotBooted() throws Exception {
        CheckDriver d = driver(title(A, "Sample Racer", ""));
        d.tick();
        p.finishPrepare(A, PrepareProtocol.DONE, "");
        run(d, 2000);
        assertTrue(p.grades.get(0).notBooted.startsWith("the game's files are not in the app"));
        assertFalse(p.log.toString().contains("launch"));
    }

    @Test
    public void leavingDuringABootPausesAndBootsAgainOnReturn() throws Exception {
        p.prepared.add(A);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        run(d, 20_000);
        p.inFront = false;   // Home, or the screen off
        run(d, 4000);
        assertTrue(p.log.contains("end 100"));
        assertEquals(LibraryCheck.PAUSED, d.s.state);
        assertEquals(LibraryCheck.AWAY, d.s.reason);
        assertEquals(LibraryCheck.READY, d.s.current().phase);
        assertTrue(p.grades.isEmpty());
        run(d, 60_000);   // nothing while away
        assertEquals(1, p.log.stream().filter(l -> l.startsWith("launch")).count());
        p.canStart = true;   // the check screen is back
        run(d, 1000);
        assertEquals(LibraryCheck.RUNNING, d.s.state);
        assertEquals(2, p.log.stream().filter(l -> l.startsWith("launch")).count());
        assertEquals(2, d.s.current().bootAttempts);
    }

    @Test
    public void severeThermalStatusCancelsThePrepareAndResumesWhenCool() throws Exception {
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        d.tick();
        p.thermal = CheckDriver.THERMAL_SEVERE;
        d.tick();
        assertTrue(p.log.contains("cancel " + A));
        assertEquals(LibraryCheck.PAUSED, d.s.state);
        assertEquals(LibraryCheck.HOT, d.s.reason);
        run(d, 60_000);
        assertEquals(1, p.log.stream().filter(l -> l.startsWith("prepare")).count());
        p.thermal = CheckDriver.THERMAL_MODERATE;
        run(d, 1000);
        assertEquals(2, p.log.stream().filter(l -> l.startsWith("prepare")).count());
        assertEquals(1, d.s.current().prepareAttempts);   // a pause is not a failed attempt
    }

    @Test
    public void lowBatteryNotChargingPauses() throws Exception {
        p.prepared.add(A);
        p.charging = false;
        p.battery = 15;
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        run(d, 2000);
        assertEquals(LibraryCheck.BATTERY, d.s.reason);
        assertTrue(p.log.isEmpty());
        p.charging = true;
        run(d, 1000);
        assertEquals("launch " + A, p.log.get(0));
    }

    @Test
    public void aPauseDuringABootLetsTheBootFinish() throws Exception {
        p.prepared.add(A);
        p.prepared.add(B);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"), title(B, "Tiny Tanks", "/games/b"));
        run(d, 10_000);
        d.pause();
        run(d, 175_000);
        assertEquals(1, p.grades.size());
        assertEquals(LibraryCheck.PAUSED, d.s.state);
        assertEquals(1, p.log.stream().filter(l -> l.startsWith("launch")).count());   // B waits
        d.resume();
        run(d, 1000);
        assertEquals("launch " + B, p.log.get(p.log.size() - 1));
    }

    @Test
    public void theGameWindowClosingEndsTheRun() throws Exception {
        p.prepared.add(A);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"));
        run(d, 20_000);
        p.window = false;   // the shell refused the pack, or found no game data, and finished its window
        run(d, 5000);
        assertEquals("window", p.grades.get(0).exit);
    }

    @Test
    public void aRestartInTheMiddleOfABootGradesItAndGoesOn() throws Exception {
        p.prepared.add(A);
        p.prepared.add(B);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"), title(B, "Tiny Tanks", "/games/b"));
        run(d, 30_000);
        assertEquals(LibraryCheck.BOOTING, d.s.current().phase);
        // The app's process dies (or the device reboots): a new driver reads check.json.
        Fake q = new Fake();
        q.files = p.files;
        q.prepared.addAll(p.prepared);
        q.now = p.now + 60_000;
        q.canStart = true;
        CheckDriver e = new CheckDriver(q, LibraryCheck.read(p.files));
        e.recover();
        assertEquals("lost", q.grades.get(0).exit);
        assertEquals(A, q.grades.get(0).title.tid);
        e.tick();
        e.tick();
        assertEquals("launch " + B, q.log.get(q.log.size() - 1));
    }

    @Test
    public void stopEndsTheGameAndKeepsTheResults() throws Exception {
        p.prepared.add(A);
        CheckDriver d = driver(title(A, "Sample Racer", "/games/a"), title(B, "Tiny Tanks", "/games/b"));
        run(d, 10_000);
        d.stop();
        assertTrue(p.log.contains("end 100"));
        assertEquals(LibraryCheck.STOPPED, LibraryCheck.read(p.files).state);
        run(d, 10_000);
        assertEquals(1, p.log.stream().filter(l -> l.startsWith("launch")).count());
    }

    @Test
    public void theStatusClockIsFoundFromTheLastLine() throws Exception {
        File f = tmp.newFile("status.jsonl");
        assertEquals(-1, CheckDriver.lastStatusMs(f));
        TestSupport.write(f, "{\"ms\":1000}\n{\"ms\":2000}\n{\"ms\":30");
        assertEquals(2000, CheckDriver.lastStatusMs(f));
    }
}
