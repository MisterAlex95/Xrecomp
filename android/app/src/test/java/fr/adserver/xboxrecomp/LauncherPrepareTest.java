package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;

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

/**
 * T6: the launcher's Prepare states (Prepare / Preparing / Failed / Play, and no toolchain = as before), each rendered
 * to build/screenshots/prepare-*.png for review.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherPrepareTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String TID = "4D530004";

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        File game = GameStore.defaultGameDir(app);
        assertNotNull(game);
        TestSupport.write(new File(game, "default.xbe"), TestSupport.xbe(0x4D530004, "Sample Game"));
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
    }

    private void withToolchain() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
    }

    private LauncherActivity launch(String name) throws Exception {
        ActivityController<LauncherActivity> c = Robolectric.buildActivity(LauncherActivity.class).setup();
        LauncherActivity a = c.get();
        shadowOf(Looper.getMainLooper()).idle();
        if (name != null)
            shot(a, name);
        return a;
    }

    private File shot(LauncherActivity a, String name) throws Exception {
        View root = a.getWindow().getDecorView();
        shadowOf(Looper.getMainLooper()).idle();
        File f = TestSupport.screenshot(root, "prepare-" + name);
        assertTrue(f.length() > 0);
        return f;
    }

    private void status(String state, String id, int step, int pct, long eta, String msg, String code)
            throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = state;
        s.id = id;
        s.step = step;
        s.pct = pct;
        s.eta = eta;
        s.msg = msg;
        s.code = code;
        s.titleId = TID;
        s.startedAt = System.currentTimeMillis() - 600_000;
        s.updatedAt = System.currentTimeMillis();
        s.log = PrepareService.logFile(app, TID).getAbsolutePath();
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
    }

    @Test
    public void withoutToolchainHomeIsAsBefore() throws Exception {
        LauncherActivity a = launch("no-toolchain");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(a.homePrepForTest());
    }

    @Test
    public void prepare() throws Exception {
        withToolchain();
        LauncherActivity a = launch("1-home-prepare");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());

        // A on Prepare: the service starts, the Preparing screen shows.
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_START));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_START));
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertEquals(TID, started.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertEquals(PrepareProtocol.RUNNING,
                     PrepareProtocol.readStatus(PrepareService.statusFile(app, TID)).state);
    }

    @Test
    public void preparing() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, "compile", 4, 42, 760, "func_00012a40.c (1 893 of 4 512)", "");
        LauncherActivity a = launch("2-preparing");
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());

        // B once arms the cancel, twice sends it.
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B));
        assertNull(shadowOf(app).getNextStartedService());
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B));
        Intent cancel = shadowOf(app).getNextStartedService();
        assertNotNull(cancel);
        assertEquals(PrepareService.ACTION_CANCEL, cancel.getAction());
    }

    @Test
    public void installPhase() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, PrepareProtocol.STEP_INSTALL, 0, 63, -1, "sysroot", "");
        LauncherActivity a = launch("2b-installing-tools");
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
    }

    @Test
    public void preparingThenDoneGoesToPlay() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, "pack", 5, 90, 5, "libtitle.so", "");
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(600));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(a.homePrepForTest());
        assertTrue(a.noteForTest(), a.noteForTest().contains("Sample Game"));
        shot(a, "4b-done-note");
    }

    @Test
    public void failed() throws Exception {
        withToolchain();
        TestSupport.write(PrepareService.logFile(app, TID), "XBA_RESULT error code=CC_FAILED msg=x\n");
        status(PrepareProtocol.FAILED, "compile", 4, 37, -1,
               "clang: error: 3 files failed to compile (func_00012a40.c, func_00012b00.c, …)", "CC_FAILED");
        LauncherActivity a = launch("3-failed");
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        // Y: Share log -> a chooser with the log's content URI.
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_Y));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_Y));
        Intent chooser = shadowOf(a).getNextStartedActivity();
        assertNotNull(chooser);
        assertEquals(Intent.ACTION_CHOOSER, chooser.getAction());
        Intent send = chooser.getParcelableExtra(Intent.EXTRA_INTENT);
        assertEquals(LogProvider.uriFor(app, TID), send.getParcelableExtra(Intent.EXTRA_STREAM));
        // B: back to Home, whose row says it failed.
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.FAILED, a.homePrepForTest());
        shot(a, "3b-home-failed");
    }

    @Test
    public void killedTwice() throws Exception {
        withToolchain();
        status(PrepareProtocol.FAILED, "compile", 4, 12, -1, "killed by the system (signal 9) twice", "KILLED");
        launch("3c-killed");
    }

    @Test
    public void interrupted() throws Exception {
        withToolchain();
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.RUNNING;
        s.id = "lift";
        s.step = 3;
        s.titleId = TID;
        s.updatedAt = System.currentTimeMillis() - 3_600_000;   // an hour ago, and no ":prepare" process
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        LauncherActivity a = launch("5-interrupted");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.INTERRUPTED, a.homePrepForTest());
    }

    @Test
    public void play() throws Exception {
        withToolchain();
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch("4-play");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(a.homePrepForTest());
    }

    @Test
    public void staleShellAbiPrepareAgain() throws Exception {
        withToolchain();
        File dir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        TestSupport.write(new File(dir, "pack.json"),
                          PrepareProtocol.readText(new File(dir, "pack.json"), 1 << 16)
                                  .replace("\"shell_abi\": 1", "\"shell_abi\": 0"));
        assertEquals(Pack.Check.WRONG_SHELL_ABI, Pack.check(app.getFilesDir(), TID));
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
    }

    @Test
    public void autoStartDoesNotSkipPrepare() throws Exception {
        withToolchain();
        GameStore.setAutoStart(app, true);
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(shadowOf(a).getNextStartedActivity());
    }
}
