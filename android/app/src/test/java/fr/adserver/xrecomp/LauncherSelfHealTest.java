package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.app.ApplicationExitInfo;
import android.content.Intent;
import android.os.Looper;
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
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowActivityManager;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;

/**
 * XBA-129: self-healing with no question asked. The launcher starts the rounds itself (a note, never a dialog), a
 * background round leaves Play and offers Stop, a round without progress stops the loop, the setting off brings the
 * buttons back. Rendered to build/screenshots/prepare-1x-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherSelfHealTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File work;
    private static final String TID = "4D530004";
    private static final String ICALL = "{\"t\":\"run\"}\n{\"t\":\"icall\",\"va\":\"0x0012ABCD\",\"ret\":\"0x00123456\"}\n";

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
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, false);
        work = new File(new File(app.getFilesDir(), "work"), TID);
        SelfHeal.prepareStarted(work);
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        PackFreshness.forget();
    }

    private void status(String state, boolean background) throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = state;
        s.id = PrepareProtocol.RUNNING.equals(state) ? "compile" : "pack";
        s.step = PrepareProtocol.RUNNING.equals(state) ? 4 : 5;
        s.pct = PrepareProtocol.RUNNING.equals(state) ? 40 : 100;
        s.titleId = TID;
        s.background = background;
        s.startedAt = System.currentTimeMillis() - 60_000;
        s.updatedAt = System.currentTimeMillis();
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
    }

    /** The last run of TID ended (reason: ApplicationExitInfo's), with discovered.jsonl gaining {@code found}. */
    private void lastRun(int reason, String found) throws Exception {
        GameStore.rememberLaunch(app, GameStore.defaultGameDir(app).getAbsolutePath(), "Sample Game", TID);
        long now = System.currentTimeMillis();
        SelfHeal.launched(work, now - 60_000);
        if (!found.isEmpty())
            TestSupport.write(SelfHeal.discoveryFile(work), found);
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());   // no game process now
        shadowOf(am).addApplicationExitInfo(ShadowActivityManager.ApplicationExitInfoBuilder.newBuilder()
                .setProcessName(app.getPackageName()).setReason(reason).setTimestamp(now - 1000).build());
    }

    private LauncherActivity launch(String shot) throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        if (shot != null)
            assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), "prepare-" + shot).length() > 0);
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

    @Test
    public void aCrashWithNewCodeIsPreparedAgainAtOnce() throws Exception {
        lastRun(ApplicationExitInfo.REASON_CRASH_NATIVE, ICALL);
        LauncherActivity a = launch("11-self-heal-round");
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("the round's Prepare started with no question asked", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertFalse(started.getBooleanExtra(PrepareService.EXTRA_BACKGROUND, true));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertTrue(a.noteForTest(), a.noteForTest().contains("(round 1)"));
        SelfHeal.State s = SelfHeal.read(work);
        assertTrue(s.roundOpen && s.relaunch);
    }

    @Test
    public void aRoundWithProgressRelaunchesTheGame() throws Exception {
        SelfHeal.startRound(work, false);
        TestSupport.write(new File(work, SelfHeal.SEEDS), "[{\"start\":\"0x0012ABCD\"}]");   // the round took it
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());
        LauncherActivity a = launch(null);
        Intent next = shadowOf(a).getNextStartedActivity();
        assertNotNull("the game starts again", next);
        assertEquals(ShellActivity.class.getName(), next.getComponent().getClassName());
        assertEquals(0, SelfHeal.read(work).idle);
    }

    @Test
    public void aRoundWithoutProgressStopsTheLoop() throws Exception {
        SelfHeal.startRound(work, false);   // and the Prepare took nothing: no discovered-seeds.json
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());
        GameStore.rememberLaunch(app, GameStore.defaultGameDir(app).getAbsolutePath(), "Sample Game", TID);
        LauncherActivity a = launch("12-self-heal-no-progress");
        Intent next = shadowOf(a).getNextStartedActivity();
        assertTrue("no relaunch", next == null || next.getComponent() == null
                                  || !ShellActivity.class.getName().equals(next.getComponent().getClassName()));
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertTrue(a.noteForTest(), a.noteForTest().contains("none of them code"));
        assertEquals(SelfHeal.Offer.NEEDS_FIX, SelfHeal.read(work).offer);
    }

    @Test
    public void aGoodRunWithNewCodeImprovesInTheBackground() throws Exception {
        lastRun(ApplicationExitInfo.REASON_EXIT_SELF, ICALL);
        LauncherActivity a = launch(null);
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("a background round started", started);
        assertTrue(started.getBooleanExtra(PrepareService.EXTRA_BACKGROUND, false));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertTrue(PrepareProtocol.readStatus(PrepareService.statusFile(app, TID)).background);
        SelfHeal.State s = SelfHeal.read(work);
        assertTrue(s.roundOpen && s.background && !s.relaunch);
    }

    @Test
    public void aBackgroundRoundLeavesPlayAndOffersStop() throws Exception {
        SelfHeal.startRound(work, true);
        status(PrepareProtocol.RUNNING, true);
        LauncherActivity a = launch("13-self-heal-improving");
        assertNull("playable", a.homePrepForTest());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.HOME_IMPROVING_NOTE), a.noteForTest());
        Widgets.PillButton stop = pillNamed(a.getWindow().getDecorView(), app.getString(R.string.SELF_HEAL_STOP));
        assertNotNull("Stop is offered", stop);
        stop.performClick();
        shadowOf(Looper.getMainLooper()).idle();
        Intent cancel = shadowOf(app).getNextStartedService();
        assertNotNull(cancel);
        assertEquals(PrepareService.ACTION_CANCEL, cancel.getAction());
        assertEquals(TID, cancel.getStringExtra(PrepareService.EXTRA_TITLE_ID));
    }

    @Test
    public void theSettingOffOffersInsteadOfStarting() throws Exception {
        SelfHeal.setAuto(app, false);
        lastRun(ApplicationExitInfo.REASON_EXIT_SELF, ICALL);
        LauncherActivity a = launch("14-self-heal-off");
        assertNull("nothing starts by itself", shadowOf(app).getNextStartedService());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertNotNull(pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN)));
        assertTrue(a.noteForTest(), a.noteForTest().contains("(1)"));
    }

    /** XBA-128: Manage's facts say what the pack's Prepare cost (pack.json « prepare »). */
    @Test
    public void manageSaysWhatThePrepareCost() throws Exception {
        File json = new File(Pack.packDir(app.getFilesDir(), TID), Pack.JSON);
        org.json.JSONObject o = new org.json.JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("prepare", new org.json.JSONObject("{\"total_s\": 512, \"jobs\": 4, \"opt\": \"2\", \"files\": 163,"
                + " \"compiled\": 163, \"peak_rss_mb\": {\"max\": 589, \"p90\": 302, \"median\": 163},"
                + " \"thermal\": {\"max\": 3, \"severe_s\": 95}}"));
        TestSupport.write(json, o.toString(1));
        LauncherActivity a = launch(null);
        a.goForTest(LauncherActivity.Screen.GAME_DATA);
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), "prepare-16-manage-prepare-cost").length() > 0);
        String d = a.descriptionForTest();
        assertTrue(d, d.contains("8 min 32 s · 4 jobs · -O2 (163 files, 163 compiled)"));
        assertTrue(d, d.contains("589 MB at most"));
    }

    @Test
    public void aGuestExceptionNeedsAFixNamed() throws Exception {
        lastRun(ApplicationExitInfo.REASON_CRASH_NATIVE, ICALL + "{\"t\":\"exception\",\"va\":\"0x00011020\","
                + "\"code\":\"0xC0000094\",\"why\":\"divide by zero\"}\n");
        LauncherActivity a = launch("15-self-heal-exception");
        assertNull("no round on what a Prepare cannot fix", shadowOf(app).getNextStartedService());
        assertTrue(a.noteForTest(), a.noteForTest().contains("guest exception 0xC0000094 at 0x00011020: divide by zero"));
    }
}
