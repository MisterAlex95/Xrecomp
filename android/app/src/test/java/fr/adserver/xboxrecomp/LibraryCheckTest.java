package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.ApplicationExitInfo;
import android.app.Application;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import org.json.JSONObject;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * The library check's files and plan (LibraryCheck), the device side of the grade (CheckPlatform: exit reasons,
 * tools.compat grade's arguments, the line the app writes when the Python cannot run) and « Share report »'s URI.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class LibraryCheckTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files;

    @Before
    public void setUp() throws Exception {
        files = tmp.newFolder("files");
    }

    @Test
    public void theRunEnvironmentIsTheDesktopRunnersMinusHeadless() {
        // The same names and values as tools/compat/device.py check_env (test_e2e boots with that one).
        Map<String, String> e = LibraryCheck.runEnv(new File("/r"), null);
        assertEquals(Arrays.asList("RECOMP_STATUS", "RECOMP_STATUS_MS", "RECOMP_PAD_LIVE", "RECOMP_USB"),
                     new ArrayList<>(e.keySet()));
        assertEquals("/r/status.jsonl", e.get("RECOMP_STATUS"));
        assertEquals("1000", e.get("RECOMP_STATUS_MS"));
        assertEquals("/r/pad-live.txt", e.get("RECOMP_PAD_LIVE"));
        assertEquals("1", e.get("RECOMP_USB"));
        assertEquals("0", LibraryCheck.runEnv(new File("/r"), "0").get("RECOMP_USB"));   // recomp.cfg's wins
        assertFalse(e.containsKey("RECOMP_HEADLESS"));
    }

    @Test
    public void currentJsonAppliesToItsTitleUntilItsDeadline() throws Exception {
        File run = LibraryCheck.runDir(files, "58420001");
        LibraryCheck.writeCurrent(files, "58420001", run, 5000);
        assertEquals(new File(run, "status.jsonl").getAbsolutePath(),
                     LibraryCheck.currentEnv(files, "58420001", 4000, null).get("RECOMP_STATUS"));
        assertNull(LibraryCheck.currentEnv(files, "58420002", 4000, null));   // another game
        assertNull(LibraryCheck.currentEnv(files, "58420001", 6000, null));   // a later, normal start
        assertNull(LibraryCheck.currentEnv(files, null, 4000, null));
        LibraryCheck.clearCurrent(files);
        assertNull(LibraryCheck.currentEnv(files, "58420001", 4000, null));
    }

    @Test
    public void stateRoundTrips() throws Exception {
        LibraryCheck.Title t = CheckDriverTest.title("58420001", "Sample Racer", "/g");
        t.phase = LibraryCheck.BOOTING;
        t.prepareMs = 61_000;
        t.xbeSha256 = "ab";
        LibraryCheck.State s = LibraryCheck.start(Arrays.asList(t), 7);
        s.run = new LibraryCheck.Run();
        s.run.launchedAt = 9;
        s.run.presses.add(8000L);
        s.run.pid = 42;
        LibraryCheck.write(files, s);
        LibraryCheck.State r = LibraryCheck.read(files);
        assertEquals(s.toJson().toString(), r.toJson().toString());
        assertTrue(LibraryCheck.active(files));
        r.state = LibraryCheck.DONE;
        LibraryCheck.write(files, r);
        assertFalse(LibraryCheck.active(files));
        TestSupport.write(LibraryCheck.stateFile(files), "{not json");
        assertNull(LibraryCheck.read(files));
        assertFalse(LibraryCheck.active(files));
    }

    @Test
    public void aTitleNeedsPrepareWithoutAPackOrWithAStaleOne() throws Exception {
        // XBA-130 x XBA-127: the same verdict as Home's (PackFreshness), on the toolkit's content digest.
        String tid = "58420001";
        String a = new String(new char[64]).replace('\0', 'a'), b = a.replace('a', 'b');
        String t1 = a.replace('a', '1'), t2 = a.replace('a', '2');
        assertTrue("no pack", LibraryCheck.needsPrepare(files, tid, "", ""));
        TestSupport.writePack(files, tid, 64);   // xbe_sha256 "00" (unknown), no toolkit_sha256
        assertFalse("nothing known on either side", LibraryCheck.needsPrepare(files, tid, "", ""));
        assertTrue("a pack from before toolkit_sha256 under an APK with a digest: older toolkit",
                   LibraryCheck.needsPrepare(files, tid, "", t1));
        File pj = new File(Pack.packDir(files, tid), Pack.JSON);
        JSONObject o = new JSONObject(new String(Files.readAllBytes(pj.toPath()), StandardCharsets.UTF_8));
        TestSupport.write(pj, o.put("xbe_sha256", a).put("toolkit_sha256", t1).toString());
        assertFalse("same XBE, same toolkit: current", LibraryCheck.needsPrepare(files, tid, a, t1));
        assertTrue("another XBE (disc changed)", LibraryCheck.needsPrepare(files, tid, b, t1));
        assertTrue("an older toolkit: Home keeps Play, the check prepares",
                   LibraryCheck.needsPrepare(files, tid, a, t2));
        assertFalse("an APK without a digest judges no toolkit", LibraryCheck.needsPrepare(files, tid, a, ""));
        // A commit change that leaves the toolkit tree alone (pack.json's "toolkit" differs) is no reason.
        TestSupport.write(pj, o.put("toolkit", "0123abcd").toString());
        assertFalse("another commit, same toolkit digest: current", LibraryCheck.needsPrepare(files, tid, a, t1));
        assertEquals(PackFreshness.judge(a, b, t1, t1) != PackFreshness.Verdict.CURRENT,
                     LibraryCheck.needsPrepare(files, tid, b, t1));
    }

    @Test
    public void theInstalledToolkitIsReadFromItsVersionFile() throws Exception {
        assertEquals("", LibraryCheck.installedToolkit(files));
        File d = new File(files, "toolkit-0123456789ab");
        TestSupport.write(new File(d, "tools/VERSION"), "cafe1234\n");
        assertEquals("", LibraryCheck.installedToolkit(files));   // not complete yet
        TestSupport.write(new File(d, Toolchain.COMPLETE), "x");
        assertEquals("cafe1234", LibraryCheck.installedToolkit(files));
    }

    @Test
    public void thePlanTakesGamesWithFilesAndCachedXbes() throws Exception {
        Library.Games g = new Library.Games();
        File game = tmp.newFolder("game");
        TestSupport.write(new File(game, "default.xbe"), TestSupport.xbe(0x58420001, "Sample Racer"));
        Library.Record r = new Library.Record();
        r.titleId = "58420001";
        r.title = "Sample Racer";
        r.dataDir = game.getAbsolutePath();
        r.dataKind = Library.KIND_IMPORTED;
        r.xbeSha256 = "aa";
        g.games.put(r.titleId, r);
        LibraryIndex x = new LibraryIndex();
        LibraryIndex.Item it = new LibraryIndex.Item();
        File iso = new File(tmp.newFolder("xbox"), "tanks.iso");
        TestSupport.write(iso, new byte[64]);
        it.loc = LibraryIndex.fileLoc(iso);
        it.kind = LibraryIndex.KIND_ISO;
        it.tid = "58420002";
        it.title = "Tiny Tanks";
        it.xbe = "xbe/58420002-00000000/default.xbe";
        it.xbeSha256 = "bb";
        TestSupport.write(it.xbeFile(files), TestSupport.xbe(0x58420002, "Tiny Tanks"));
        x.items.add(it);
        LibraryScreen.Env env = new LibraryScreen.Env();
        env.toolchain = true;
        env.accessible = true;
        List<LibraryScreen.Entry> entries = LibraryScreen.entries(files, g, x, env);
        List<LibraryCheck.Title> plan = LibraryCheck.plan(entries, files, null);
        assertEquals(2, plan.size());
        LibraryCheck.Title a = plan.get(0), b = plan.get(1);
        assertEquals("Sample Racer", a.title);
        assertEquals(game.getAbsolutePath(), a.gameDir);
        assertEquals(game.getAbsolutePath(), a.source);
        assertEquals("aa", a.xbeSha256);
        assertTrue(a.bootable());
        assertEquals("Tiny Tanks", b.title);
        assertFalse(b.bootable());
        assertEquals(it.xbeFile(files).getParent(), b.source);
        assertEquals(1, LibraryCheck.plan(entries, files, "58420002").size());
        assertEquals(5 * 12 + (2 * (LibraryCheck.SECONDS + 15) + 59) / 60, LibraryCheck.estimateMinutes(5, 2, 12));
    }

    @Test
    public void exitReasonsInTheGradesWords() {
        assertEquals("exit:0", CheckPlatform.exitArg(ApplicationExitInfo.REASON_EXIT_SELF, 0));
        assertEquals("signal:9", CheckPlatform.exitArg(ApplicationExitInfo.REASON_SIGNALED, 9));
        assertEquals("crash:11", CheckPlatform.exitArg(ApplicationExitInfo.REASON_CRASH_NATIVE, 11));
        assertEquals("crash", CheckPlatform.exitArg(ApplicationExitInfo.REASON_CRASH_NATIVE, 0));
        assertEquals("java", CheckPlatform.exitArg(ApplicationExitInfo.REASON_CRASH, 0));
        assertEquals("anr", CheckPlatform.exitArg(ApplicationExitInfo.REASON_ANR, 0));
        assertEquals("lowmem", CheckPlatform.exitArg(ApplicationExitInfo.REASON_LOW_MEMORY, 0));
    }

    @Test
    public void gradeArgumentsFollowToolsCompatGrade() {
        CheckDriver.Grade g = new CheckDriver.Grade();
        g.title = CheckDriverTest.title("58420001", "Sample Racer", "/g");
        g.work = new File("/w");
        g.status = new File("/r/status.jsonl");
        g.log = new File("/r/run.log");
        g.presses = "8000,10000";
        g.exit = "crash:11";
        g.runS = 31.25;
        g.prepareMs = 61_500;
        List<String> a = CheckPlatform.gradeArgs(g, new File("/c/compat-results.jsonl"), Arrays.asList("device=X"));
        assertEquals(Arrays.asList("--out", "/c/compat-results.jsonl", "--source", "Sample Racer", "--title-id",
                                   "58420001", "--work", "/w", "--prepare-s", "61", "--status", "/r/status.jsonl",
                                   "--log", "/r/run.log", "--presses", "8000,10000", "--exit", "crash:11",
                                   "--run-s", "31.3", "--sustain", "60", "--field", "device=X"), a);
        g.prepareFailed = "COMPILE_FAILED: x";
        a = CheckPlatform.gradeArgs(g, new File("/c/r"), new ArrayList<>());
        assertTrue(a.contains("--prepare-failed") && !a.contains("--status"));
    }

    @Test
    public void withoutPythonTheAppWritesTheLineItself() throws Exception {
        CheckDriver.Grade g = new CheckDriver.Grade();
        g.title = CheckDriverTest.title("58420001", "Sample Racer", "/g");
        g.notBooted = "the game's files are not in the app";
        File out = LibraryCheck.resultsFile(files);
        JSONObject o = CheckPlatform.fallback(g, out, Arrays.asList("device=AYN Thor", "android=\"13\"", "api=33"),
                                              new java.io.IOException("no python"));
        assertEquals("prepares", o.getString("stage"));
        assertEquals("app", o.getString("graded_by"));
        assertEquals("13", o.getString("android"));
        assertEquals(33, o.getInt("api"));
        String line = new String(Files.readAllBytes(out.toPath()), StandardCharsets.UTF_8).trim();
        JSONObject back = new JSONObject(line);
        for (String k : new String[] { "source", "toolkit", "time", "stage", "blocker", "title_id", "device" })
            assertTrue(k, back.has(k));
    }

    @Test
    public void shareReportServesTheResultsFileOnly() throws Exception {
        Application app = RuntimeEnvironment.getApplication();
        LogProvider lp = Robolectric.setupContentProvider(LogProvider.class, LogProvider.authority(app));
        Uri u = LogProvider.reportUri(app);
        assertEquals("content://" + app.getPackageName() + ".logs/check/compat-results.jsonl", u.toString());
        TestSupport.write(LibraryCheck.resultsFile(app.getFilesDir()), "{\"stage\":\"boots\"}\n");
        try (Cursor c = lp.query(u, null, null, null, null)) {
            c.moveToFirst();
            assertEquals("compat-results.jsonl", c.getString(c.getColumnIndex(OpenableColumns.DISPLAY_NAME)));
        }
        assertTrue(lp.openFile(u, "r") != null);
        Uri other = Uri.parse("content://" + app.getPackageName() + ".logs/check/check.json");
        assertNull(lp.query(other, null, null, null, null));
    }
}
