package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.content.Intent;
import android.os.BatteryManager;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.Arrays;

/**
 * « Check my library »'s screen (CheckActivity) over a synthetic library (our XBEs, fake packs): the intro (games,
 * how many to prepare and start, the time), Start writing check.json and starting CheckService, the screens of a
 * check under way, paused and done (rendered to build/screenshots/check-*.png, en and fr), Stop asking twice, and the
 * launcher handing over to the check while one runs.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class CheckActivityTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File files;
    static final String[] NAMES = { "Sample Racer", "Tiny Tanks", "Night Drive", "Puzzle Garden" };

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        files = app.getFilesDir();
        shadowOf(app.getSystemService(BatteryManager.class)).setIsCharging(true);
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "test");
        // Three games with their files (the first prepared), one image not imported.
        Library.Games g = new Library.Games();
        for (int i = 0; i < 3; i++) {
            File d = tmp.newFolder("game" + i);
            TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(0x58420001 + i, NAMES[i]));
            Library.Record r = new Library.Record();
            r.titleId = String.format("%08X", 0x58420001 + i);
            r.title = NAMES[i];
            r.dataDir = d.getAbsolutePath();
            r.dataKind = Library.KIND_IMPORTED;
            g.games.put(r.titleId, r);
        }
        g.selected = "58420002";
        Library.write(Library.gamesFile(files), g);
        TestSupport.writePack(files, "58420001", 64);
        File iso = new File(tmp.newFolder("xbox"), "garden.iso");
        TestSupport.write(iso, new byte[64]);
        LibraryIndex x = LibraryIndex.read(Library.indexFile(files));
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = LibraryIndex.fileLoc(iso);
        it.tid = "58420004";
        it.title = NAMES[3];
        it.size = iso.length();
        it.mtime = iso.lastModified();
        it.xbe = "xbe/58420004-00000000/default.xbe";
        TestSupport.write(it.xbeFile(files), TestSupport.xbe(0x58420004, NAMES[3]));
        x.items.add(it);
        LibraryIndex.write(Library.indexFile(files), x);
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        CheckActivity.setShownForTest(false);
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private CheckActivity open() {
        ActivityController<CheckActivity> c = Robolectric.buildActivity(CheckActivity.class).setup();
        idle(600);
        return c.get();
    }

    private void shot(CheckActivity a, String name) throws Exception {
        View root = a.getWindow().getDecorView();
        root.measure(View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY),
                     View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 1920, 1080);
        File f = TestSupport.screenshot(root, "check-" + name);
        assertTrue(f.length() > 0);
    }

    private static String labels(ViewGroup g) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < g.getChildCount(); i++)
            b.append(((Widgets.PillButton)g.getChildAt(i)).title).append('|');
        return b.toString();
    }

    @Test
    public void introSaysWhatACheckWouldDo() throws Exception {
        for (String loc : new String[] { "en", "fr" }) {
            RuntimeEnvironment.setQualifiers(loc + "-w1920dp-h1080dp-land-mdpi");
            CheckActivity a = open();
            String b = labels(a.buttonsForTest());
            assertTrue(b, b.startsWith(loc.equals("en") ? "Check all 4|Check Tiny Tanks only|"
                                                        : "Vérifier les 4|Vérifier Tiny Tanks seul|"));
            shot(a, "intro-" + loc);
        }
    }

    @Test
    public void startWritesTheCheckAndStartsTheService() throws Exception {
        CheckActivity a = open();
        a.buttonsForTest().getChildAt(0).performClick();
        idle(600);
        LibraryCheck.State s = LibraryCheck.read(files);
        assertNotNull(s);
        assertEquals(LibraryCheck.RUNNING, s.state);
        assertEquals(4, s.titles.size());
        assertEquals("Night Drive", s.titles.get(0).title);   // sorted by title, as the grid
        Intent svc = shadowOf(app).getNextStartedService();
        assertEquals(CheckService.class.getName(), svc.getComponent().getClassName());
        assertTrue(svc.getBooleanExtra(CheckService.EXTRA_FRESH, false));
        // The running screen's buttons.
        assertTrue(labels(a.buttonsForTest()), labels(a.buttonsForTest()).startsWith("Pause|Stop|Share report|"));
        // Stop asks twice.
        a.buttonsForTest().getChildAt(1).performClick();
        shadowOf(app).clearStartedServices();
        assertTrue(a.noteForTest().startsWith("Press Stop again"));
        a.buttonsForTest().getChildAt(1).performClick();
        Intent stop = shadowOf(app).getNextStartedService();
        assertEquals(CheckService.ACTION_STOP, stop.getAction());
    }

    /** check.json part way: two graded, one preparing. */
    private void midway(String state, String reason) throws Exception {
        LibraryCheck.Title a = CheckDriverTest.title("58420003", "Night Drive", "/g/3");
        a.phase = LibraryCheck.GRADED;
        a.stage = "sustained";
        LibraryCheck.Title b = CheckDriverTest.title("58420004", "Puzzle Garden", "");
        b.phase = LibraryCheck.GRADED;
        b.stage = "prepares";
        b.blocker = "not booted: the game's files are not in the app (import it to boot it)";
        LibraryCheck.Title c = CheckDriverTest.title("58420001", "Sample Racer", "/g/1");
        c.phase = LibraryCheck.GRADED;
        c.stage = "boots";
        c.blocker = "crash (signal 11): SIGSEGV";
        LibraryCheck.Title d = CheckDriverTest.title("58420002", "Tiny Tanks", "/g/2");
        d.phase = LibraryCheck.PREPARING;
        LibraryCheck.State s = LibraryCheck.start(Arrays.asList(a, b, c, d), System.currentTimeMillis());
        s.index = 3;
        s.state = state;
        s.reason = reason;
        LibraryCheck.write(files, s);
        PrepareProtocol.Status st = new PrepareProtocol.Status();
        st.titleId = "58420002";
        st.id = "compile";
        st.step = 5;
        st.pct = 42;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, "58420002"), st);
    }

    @Test
    public void runningPausedAndDoneScreens() throws Exception {
        midway(LibraryCheck.RUNNING, "");
        CheckActivity a = open();
        shot(a, "running-en");
        midway(LibraryCheck.PAUSED, LibraryCheck.HOT);
        a.refresh();
        assertTrue(labels(a.buttonsForTest()).startsWith("Continue|"));
        shot(a, "paused-en");
        LibraryCheck.State s = LibraryCheck.read(files);
        s.titles.get(3).phase = LibraryCheck.GRADED;
        s.titles.get(3).stage = "renders";
        s.titles.get(3).blocker = "no response to the pad";
        s.index = 4;
        s.state = LibraryCheck.DONE;
        LibraryCheck.write(files, s);
        a.refresh();
        assertTrue(labels(a.buttonsForTest()).startsWith("Share report|New check|Done|"));
        shot(a, "done-en");
        assertEquals("sustained 1 · renders 1 · boots 1 · prepares 1", CheckActivity.summaryLine(app, s));
        RuntimeEnvironment.setQualifiers("fr-w1920dp-h1080dp-land-mdpi");
        midway(LibraryCheck.RUNNING, "");
        shot(open(), "running-fr");
    }

    @Test
    public void theLauncherHandsOverToARunningCheck() throws Exception {
        midway(LibraryCheck.RUNNING, "");
        LauncherActivity l = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        assertTrue(l.isFinishing());
        Intent next = shadowOf(app).getNextStartedActivity();
        assertEquals(CheckActivity.class.getName(), next.getComponent().getClassName());
    }

    @Test
    public void theGameGetsTheRunEnvironmentOnlyWhenChecked() throws Exception {
        File run = LibraryCheck.runDir(files, "58420002");
        LibraryCheck.writeCurrent(files, "58420002", run, System.currentTimeMillis() + 60_000);
        assertEquals(new File(run, "pad-live.txt").getAbsolutePath(),
                     LibraryCheck.currentEnv(files, "58420002", System.currentTimeMillis(), null)
                             .get("RECOMP_PAD_LIVE"));
    }
}
