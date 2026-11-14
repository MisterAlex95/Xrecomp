package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.View;

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

/**
 * XBA-82: a disc's other image prepared on its first launch (PendingImage). The first Prepare names it with library
 * ""; when the game launches it, Home offers « Prepare IMAGE » (Play starts default.xbe again, without the launch data
 * meant for the image), the Prepare asks for it in images.want, and the next start goes on into it (image.next).
 * Rendered to build/screenshots/prepare-17-image-pending.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherImagePrepareTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File work, packDir, launchData;
    private static final String TID = "4D530004";

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        File game = GameStore.defaultGameDir(app);
        TestSupport.write(new File(game, "default.xbe"), TestSupport.xbe(0x4D530004, "Sample Game"));
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());   // the game's process already ended
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        packDir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        images("");
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.DONE;
        s.id = "pack";
        s.step = 5;
        s.pct = 100;
        s.titleId = TID;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        work = new File(new File(app.getFilesDir(), "work"), TID);
        // What the shell and the kernel leave when the game launches Sub/Game.xbe (shell_main.c image_via_launcher).
        TestSupport.write(new File(work, PendingImage.PENDING), "Sub/Game.xbe\n");
        launchData = new File(GameTheme.saveDir(app), SaveCaches.LAUNCH_DATA);
        TestSupport.write(launchData, "page");
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        PackFreshness.forget();
    }

    /** pack.json "images": {"Sub/Game.xbe": lib} (lib "" = named, not prepared), lib written when it is not "". */
    private void images(String lib) throws Exception {
        File f = new File(packDir, Pack.JSON);
        JSONObject o = new JSONObject(PrepareProtocol.readText(f, 1 << 20));
        o.put("images", new JSONObject().put("Sub/Game.xbe", lib).put("intro.xbe", ""));
        TestSupport.write(f, o.toString(1));
        if (!lib.isEmpty())
            TestSupport.write(new File(packDir, lib), new byte[16]);
    }

    private LauncherActivity launch(Intent in) {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class, in).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        return a;
    }

    private static Widgets.PillButton pillNamed(View v, String title) {
        if (v instanceof Widgets.PillButton && title.equals(((Widgets.PillButton)v).title))
            return (Widgets.PillButton)v;
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup)v;
            for (int i = 0; i < g.getChildCount(); i++) {
                Widgets.PillButton p = pillNamed(g.getChildAt(i), title);
                if (p != null)
                    return p;
            }
        }
        return null;
    }

    private static boolean startedShell(Intent i) {
        return i != null && i.getComponent() != null
               && ShellActivity.class.getName().equals(i.getComponent().getClassName());
    }

    @Test
    public void homeOffersToPrepareTheImage() throws Exception {
        Intent in = ShellActivity.prepareImageIntent(app, "Sub/Game.xbe");
        assertTrue((in.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0);
        LauncherActivity a = launch(in);
        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), "prepare-17-image-pending").length() > 0);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(app.getString(R.string.PREPARE_IMAGE, "Sub/Game.xbe"), a.homePrimaryForTest().title);
        assertTrue(a.noteForTest(), a.noteForTest().contains("Sub/Game.xbe"));
        assertNull("nothing starts by itself", shadowOf(a).getNextStartedActivity());

        a.homePrimaryForTest().performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(PendingImage.wanted(work).contains("sub/game.xbe"));
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("the title's Prepare, through the same queue", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertEquals(TID, started.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertEquals("still pending until the image is in the pack", "Sub/Game.xbe", PendingImage.pending(work));
    }

    @Test
    public void playStartsTheGameFromTheBeginning() throws Exception {
        LauncherActivity a = launch(ShellActivity.prepareImageIntent(app, "Sub/Game.xbe"));
        Widgets.PillButton play = pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PLAY));
        assertNotNull(play);
        play.performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(startedShell(shadowOf(a).getNextStartedActivity()));
        assertNull(PendingImage.pending(work));
        assertFalse("the launch data was meant for the image", launchData.exists());
        assertFalse(new File(work, PendingImage.NEXT).exists());
    }

    @Test
    public void onceInThePackTheGameGoesOnIntoTheImage() throws Exception {
        images("libimage-1.so");
        LauncherActivity a = launch(new Intent(app, LauncherActivity.class));
        assertEquals("prepared: Home plays", app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        a.homePrimaryForTest().performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(startedShell(shadowOf(a).getNextStartedActivity()));
        assertEquals("Sub/Game.xbe", PrepareProtocol.readText(new File(work, PendingImage.NEXT), 256).trim());
        assertNull(PendingImage.pending(work));
        assertTrue("kept for the image", launchData.exists());
    }

    @Test
    public void pendingImageFiles() throws Exception {
        assertEquals("", PendingImage.lib(packDir, "\\INTRO.xbe"));
        assertNull(PendingImage.lib(packDir, "other.xbe"));
        assertFalse(PendingImage.built(packDir, "sub/game.xbe"));
        PendingImage.want(work, "Sub\\Game.xbe");
        PendingImage.want(work, "/sub/GAME.xbe");
        PendingImage.want(work, "intro.xbe");
        assertEquals("sub/game.xbe\nintro.xbe\n", PrepareProtocol.readText(new File(work, PendingImage.WANT), 256));
        images("../x.so");
        assertFalse("not a plain name", PendingImage.built(packDir, "Sub/Game.xbe"));
        images("libimage-1.so");
        assertTrue(PendingImage.built(packDir, "SUB/GAME.XBE"));
        assertNull(PendingImage.beforeLaunch(new File(tmp.getRoot(), "none"), packDir, GameTheme.saveDir(app)));
        assertTrue("nothing pending: the launch data stays (a quick reboot's)", launchData.exists());
    }
}
