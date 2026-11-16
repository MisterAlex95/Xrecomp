package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;

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
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * Storage › Cloud saves (CLOUD) against a Google of the test (CloudSavesTest.FakeGoogle): a build without the Google
 * client says so; Connect shows the code, the sign-in finishes on its own and the first sync sends the saves; a game
 * changed on both sides waits for the player, who keeps this device's.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherCloudTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private CloudSavesTest.FakeGoogle google;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        google = new CloudSavesTest.FakeGoogle();
        google.interval = 1;
        google.allow = false;   // SystemClock.sleep is Robolectric's: the polls do not wait
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        TestSupport.write(new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin"), "level 3");
    }

    @After
    public void tearDown() {
        LauncherActivity.cloudHttpFactory = null;
        LauncherActivity.cloudClientForTest = null;
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private LauncherActivity launch() {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        return a;
    }

    private static void waitCloud(LauncherActivity a) throws Exception {
        for (int i = 0; i < 400; i++) {
            Thread t = a.cloudThreadForTest();
            if (t == null || !t.isAlive())
                break;
            Thread.sleep(20);
            idle(50);
        }
        idle(200);
    }

    private static Widgets.MenuRow menuRow(LauncherActivity a, String title) {
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow && title.equals(((Widgets.MenuRow)v).title))
                return (Widgets.MenuRow)v;
        }
        return null;
    }

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    /** The row of a game under « Changed on both sides » (its badge). */
    private Widgets.MenuRow conflictRow(LauncherActivity a) {
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow
                && app.getString(R.string.CLOUD_CONFLICT_DETAIL).equals(((Widgets.MenuRow)v).description))
                return (Widgets.MenuRow)v;
        }
        return null;
    }

    @Test
    public void notInThisBuild() throws Exception {
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        Widgets.MenuRow r = menuRow(a, app.getString(R.string.CLOUD_TITLE));
        assertNotNull(r);
        assertEquals(app.getString(R.string.CLOUD_UNAVAILABLE), r.value);
        r.performClick();
        idle(300);
        assertEquals(LauncherActivity.Screen.CLOUD, a.screenForTest());
        assertNotNull(menuRow(a, app.getString(R.string.CLOUD_UNAVAILABLE)));
        assertNull(menuRow(a, app.getString(R.string.CLOUD_CONNECT)));
    }

    @Test
    public void connectSyncAndSettleAConflict() throws Exception {
        LauncherActivity.cloudClientForTest = new String[] { "client", "secret" };
        LauncherActivity.cloudHttpFactory = c -> google;
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        assertEquals(app.getString(R.string.VAL_OFF), menuRow(a, app.getString(R.string.CLOUD_TITLE)).value);
        a.goForTest(LauncherActivity.Screen.CLOUD);
        idle(300);
        menuRow(a, app.getString(R.string.CLOUD_CONNECT)).performClick();
        Widgets.MenuRow code = null;
        for (int i = 0; i < 300 && code == null; i++) {
            Thread.sleep(5);
            idle(10);
            code = menuRow(a, app.getString(R.string.CLOUD_CODE, "ABCD-EFGH"));
        }
        assertNotNull("the code to type; note: " + a.noteForTest() + " screen " + a.screenForTest(), code);
        a.forceFocusForTest();
        idle(300);
        TestSupport.screenshot(a.getWindow().getDecorView(), "cloud-code");
        google.allow = true;
        waitCloud(a);   // the sign-in
        waitCloud(a);   // the first sync
        assertEquals(1, google.meta.size());
        assertEquals("player@example.com", menuRow(a, app.getString(R.string.CLOUD_ACCOUNT)).value);
        assertNotNull(menuRow(a, app.getString(R.string.CLOUD_SYNC_NOW)));
        TestSupport.screenshot(a.getWindow().getDecorView(), "cloud-connected");

        // Both sides change: Sync now finds the conflict and waits.
        TestSupport.write(new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin"), "level 4 here");
        JSONObject m = google.meta.values().iterator().next();
        m.getJSONObject("appProperties").put(CloudSaves.P_HASH, "another-device");
        menuRow(a, app.getString(R.string.CLOUD_SYNC_NOW)).performClick();
        waitCloud(a);
        assertEquals(app.getResources().getQuantityString(R.plurals.CLOUD_CONFLICT_NOTE, 1, 1), a.noteForTest());
        Widgets.MenuRow conflict = conflictRow(a);
        assertNotNull("a row for the game", conflict);
        TestSupport.screenshot(a.getWindow().getDecorView(), "cloud-conflict");
        conflict.performClick();
        idle(300);
        Widgets.Sheet sheet = a.sheetForTest();
        assertNotNull(sheet);
        sheet.rowsForTest().get(0).performClick();   // Keep this device's saves
        waitCloud(a);
        assertNull(conflictRow(a));
        assertEquals(SaveArchive.hash(new Storage.Dirs(app.getFilesDir(), null), "4D530004"),
                     google.meta.values().iterator().next().getJSONObject("appProperties")
                           .getString(CloudSaves.P_HASH));
        assertTrue(CloudSaves.load(app.getFilesDir()).conflicts.isEmpty());
    }

    /**
     * Connected, Sync automatically on: the launcher's resume brings back what another device sent; a game started
     * later gets the cloud's newer saves first (« Checking cloud saves… »), then starts.
     */
    @Test
    public void newerSavesComeDownBeforeTheGameStarts() throws Exception {
        LauncherActivity.cloudClientForTest = new String[] { "client", "secret" };
        LauncherActivity.cloudHttpFactory = c -> google;
        GoogleDrive drive = new GoogleDrive(google, "client", "secret");
        GoogleDrive.Tokens t = new GoogleDrive.Tokens();
        t.refresh = "rt";
        t.email = "player@example.com";
        CloudSaves.connect(app.getFilesDir(), t);
        Storage.Dirs other = new Storage.Dirs(tmp.newFolder("other"), tmp.newFolder("othere"));
        CloudSaves.connect(other.filesDir, t);
        CloudSaves.Report r = new CloudSaves(drive, new Storage.Dirs(app.getFilesDir(), null), "here", null).syncAll(0);
        assertEquals(1, r.uploaded.size());
        assertEquals(1, new CloudSaves(drive, other, "other", null).syncAll(0).downloaded.size());

        TestSupport.write(new File(other.saveDir(), "UDATA/4d530004/save.bin"), "level 5 elsewhere");
        new CloudSaves(drive, other, "other", null).syncAll(0);
        LauncherActivity a = launch();
        waitCloud(a);   // the resume's sync
        File save = new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin");
        assertEquals("level 5 elsewhere", new String(java.nio.file.Files.readAllBytes(save.toPath()), "UTF-8"));

        TestSupport.write(new File(other.saveDir(), "UDATA/4d530004/save.bin"), "level 6 elsewhere");
        new CloudSaves(drive, other, "other", null).syncAll(0);
        a.forceFocusForTest();
        idle(300);
        a.dispatchKeyEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN,
                                                     android.view.KeyEvent.KEYCODE_BUTTON_START));
        a.dispatchKeyEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_UP,
                                                     android.view.KeyEvent.KEYCODE_BUTTON_START));
        idle(50);
        assertEquals(app.getString(R.string.CLOUD_CHECKING), a.noteForTest());
        waitCloud(a);
        assertEquals("level 6 elsewhere", new String(java.nio.file.Files.readAllBytes(save.toPath()), "UTF-8"));
        assertEquals(ShellActivity.class.getName(), shadowOf(a).getNextStartedActivity().getComponent().getClassName());
    }
}
