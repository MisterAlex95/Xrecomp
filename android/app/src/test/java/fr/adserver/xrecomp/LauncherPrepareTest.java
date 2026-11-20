package fr.adserver.xrecomp;

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
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

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
        PackFreshness.forget();
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

    /** XBA-335: « Send report » beside a failed Prepare: a sheet says what leaves, Send posts it, Cancel does not. */
    @Test
    public void failedSendsAReportOnlyOnSend() throws Exception {
        java.util.List<byte[]> posted = new java.util.ArrayList<>();
        CrashReport.transport = (url, h, body) -> {
            posted.add(body);
            return 200;
        };
        try {
            withToolchain();
            TestSupport.write(PrepareService.logFile(app, TID), "XBA_RESULT error code=CC_FAILED msg=x\n");
            status(PrepareProtocol.FAILED, "compile", 4, 37, -1, "clang: error: 3 files failed to compile",
                   "CC_FAILED");
            LauncherActivity a = launch(null);
            Widgets.PillButton report = pillNamed(a.getWindow().getDecorView(), app.getString(R.string.REPORT_BUTTON));
            assertNotNull("the Send report pill", report);
            report.performClick();
            shadowOf(Looper.getMainLooper()).idle();
            assertNotNull(a.sheetForTest());
            CrashReport r = a.reportShownForTest();
            assertNotNull(r);
            assertEquals(CrashReport.KIND_PREPARE, r.kind);
            assertEquals("CC_FAILED: clang: error: 3 files failed to compile", r.value);
            shot(a, "3c-report-sheet");
            // B closes it: nothing sent.
            a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B));
            a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B));
            assertNull(a.sheetForTest());
            assertTrue(posted.isEmpty());
            // Again, then Send (the sheet's first row).
            report.performClick();
            shadowOf(Looper.getMainLooper()).idle();
            a.focusForTest().performClick();
            for (int i = 0; i < 200 && posted.isEmpty(); i++)
                Thread.sleep(10);
            assertEquals(1, posted.size());
            for (int i = 0; i < 200 && !app.getString(R.string.REPORT_SENT).equals(a.noteForTest()); i++) {
                Thread.sleep(10);
                shadowOf(Looper.getMainLooper()).idle();
            }
            assertEquals(app.getString(R.string.REPORT_SENT), a.noteForTest());
            assertTrue(new String(posted.get(0), java.nio.charset.StandardCharsets.UTF_8).contains("\"prepare_code\":\"CC_FAILED\""));
        } finally {
            CrashReport.transport = CrashReport.HTTP;
        }
    }

    /** The report sheet on 4:3 and square screens: Send and Cancel stay on screen. */
    @Test
    @Config(qualifiers = "w1440dp-h1080dp-land-mdpi")
    public void reportSheet4x3() throws Exception {
        reportSheetShot("3c-report-sheet-4x3");
    }

    @Test
    @Config(qualifiers = "w1080dp-h1080dp-land-mdpi")
    public void reportSheet1x1() throws Exception {
        reportSheetShot("3c-report-sheet-1x1");
    }

    private void reportSheetShot(String name) throws Exception {
        withToolchain();
        TestSupport.write(PrepareService.logFile(app, TID), "XBA_RESULT error code=CC_FAILED msg=x\n");
        status(PrepareProtocol.FAILED, "compile", 4, 37, -1, "clang: error: 3 files failed to compile", "CC_FAILED");
        LauncherActivity a = launch(null);
        pillNamed(a.getWindow().getDecorView(), app.getString(R.string.REPORT_BUTTON)).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        View send = a.focusForTest();
        assertTrue(send instanceof Widgets.MenuRow);
        int[] at = new int[2];
        send.getLocationOnScreen(at);
        int h = a.getWindow().getDecorView().getHeight();
        assertTrue("Send is on screen: " + at[1] + "+" + send.getHeight() + " of " + h, at[1] + send.getHeight() <= h);
        shot(a, name);
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

    /** The shell refused the pack (PACK_INVALID / PACK_DLOPEN): it is dropped and Home offers Prepare again. */
    @Test
    public void badPackFromShellOffersPrepareAgain() throws Exception {
        withToolchain();
        GameStore.setAutoStart(app, true);
        File dir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        Intent in = new Intent(app, LauncherActivity.class)
                .putExtra(LauncherActivity.EXTRA_BAD_PACK, "dlopen failed: libtitle.so: bad ELF magic");
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class, in).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        shot(a, "6-bad-pack");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertTrue(!dir.exists());
        assertEquals(Pack.Check.NO_PACK, Pack.check(app.getFilesDir(), TID));
        assertTrue(a.noteForTest(), a.noteForTest().contains("bad ELF magic"));
        assertNull("« open directly » does not restart the shell", shadowOf(a).getNextStartedActivity());
    }

    /**
     * K-2: the title left by itself (HalReturnToFirmware to the dashboard). The shell hands back with the kernel's
     * reason; Home says it, the pack stays, and « open directly » does not start the game again at once.
     */
    @Test
    public void titleExitShowsWhyOnHome() throws Exception {
        withToolchain();
        GameStore.setAutoStart(app, true);
        File dir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());   // the game's process already ended
        Intent in = ShellActivity.titleExitedIntent(app, "the game returned to the dashboard (routine 2)");
        assertTrue((in.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TASK) != 0);
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class, in).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertTrue(a.noteForTest(), a.noteForTest().contains("returned to the dashboard (routine 2)"));
        assertTrue("the pack stays", dir.exists());
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertNull("« open directly » does not restart the game", shadowOf(a).getNextStartedActivity());
        assertEquals("The game ended.", LauncherActivity.titleExitedNote(app, " "));
    }

    @Test
    public void autoStartDoesNotSkipPrepare() throws Exception {
        withToolchain();
        GameStore.setAutoStart(app, true);
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(shadowOf(a).getNextStartedActivity());
    }

    /** XBA-74: the last run crashed (the system's exit record), with what discovered.jsonl gained since the Prepare. */
    private void crashedRun(String discovered) throws Exception {
        withToolchain();
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        File work = new File(new File(app.getFilesDir(), "work"), TID);
        SelfHeal.prepareStarted(work);
        GameStore.rememberLaunch(app, GameStore.defaultGameDir(app).getAbsolutePath(), "Sample Game", TID);
        long now = System.currentTimeMillis();
        SelfHeal.launched(work, now - 60_000);
        if (!discovered.isEmpty())
            TestSupport.write(SelfHeal.discoveryFile(work), discovered);
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());   // no game process now
        shadowOf(am).addApplicationExitInfo(
                org.robolectric.shadows.ShadowActivityManager.ApplicationExitInfoBuilder.newBuilder()
                        .setProcessName(app.getPackageName())
                        .setReason(android.app.ApplicationExitInfo.REASON_CRASH_NATIVE)
                        .setTimestamp(now - 1000)
                        .build());
    }

    @Test
    public void crashWithNewCodeOffersPrepareAgain() throws Exception {
        SelfHeal.setAuto(app, false);   // XBA-129: on (the default), the launcher starts the round itself
        crashedRun("{\"t\":\"run\"}\n{\"t\":\"icall\",\"va\":\"0x0012ABCD\",\"ret\":\"0x00123456\"}\n");
        LauncherActivity a = launch("7-prepare-again");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(app.getString(R.string.PREPARE_AGAIN), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.HOME_PREPARE_AGAIN_NOTE), a.noteForTest());

        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_START));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_START));
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        SelfHeal.State s = SelfHeal.read(new File(new File(app.getFilesDir(), "work"), TID));
        assertEquals(1, s.rounds);
        assertTrue("the game starts again when the Prepare is done", s.relaunch);
        assertEquals(SelfHeal.Offer.NONE, s.offer);
    }

    @Test
    public void crashWithNothingNewNeedsAFix() throws Exception {
        crashedRun("{\"t\":\"run\"}\n{\"t\":\"unimpl\",\"va\":\"0x00012345\",\"insn\":\"bsf eax, ecx\"}\n");
        LauncherActivity a = launch("8-needs-fix");
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertTrue(a.noteForTest(), a.noteForTest().contains("bsf eax, ecx at 0x00012345"));
    }

    // ---- XBA-127: a pack that no longer matches ----------------------------------------------------------------

    private static String hex(String two) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 32; i++)
            b.append(two);
        return b.toString();
    }

    /** pack.json of TID with one more field, or one changed. */
    private void packField(String key, String value) throws Exception {
        File json = new File(Pack.packDir(app.getFilesDir(), TID), "pack.json");
        org.json.JSONObject o = new org.json.JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put(key, value);
        TestSupport.write(json, o.toString(1));
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

    private void press(LauncherActivity a, int key) {
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, key));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, key));
        shadowOf(Looper.getMainLooper()).idle();
    }

    /** Another revision of the disc (same title ID, another default.xbe): Play never runs the pack; Prepare again. */
    @Test
    public void otherXbeIsNeverRun() throws Exception {
        withToolchain();
        File dir = TestSupport.writePack(app.getFilesDir(), TID, 64);
        packField("xbe_sha256", hex("ab"));   // not the game folder's default.xbe
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        File save = new File(GameTheme.saveDir(app), "UDATA/4d530004/slot0.sav");
        TestSupport.write(save, "my progress");

        LauncherActivity a = launch(null);
        // Home hashes default.xbe on a worker thread, then shows the mismatch.
        File xbe = new File(GameStore.defaultGameDir(app), "default.xbe");
        for (int i = 0; i < 500 && PackFreshness.knownXbeSha(xbe) == null; i++)
            Thread.sleep(10);
        shadowOf(Looper.getMainLooper()).idle();
        Intent next = shadowOf(a).getNextStartedActivity();
        assertTrue("the game did not start", next == null || next.getComponent() == null
                                             || !ShellActivity.class.getName().equals(next.getComponent().getClassName()));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertEquals(app.getString(R.string.PREPARE_AGAIN), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.HOME_OTHER_XBE_NOTE), a.noteForTest());
        shot(a, "9-other-xbe");

        // A on Prepare again: a Prepare starts; the saves are untouched.
        press(a, KeyEvent.KEYCODE_BUTTON_START);
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertTrue(save.isFile());
        assertEquals("my progress", PrepareProtocol.readText(save, 1 << 10));
        assertTrue("the old pack stays until the new one replaces it", dir.isDirectory());
    }

    /** « Open the game directly » with another default.xbe than the pack's: hashed before the start, not started. */
    @Test
    public void otherXbeStopsOpenDirectly() throws Exception {
        withToolchain();
        GameStore.setAutoStart(app, true);
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        packField("xbe_sha256", hex("ab"));
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        GameStore.rememberLaunch(app, GameStore.defaultGameDir(app).getAbsolutePath(), "Sample Game", TID);
        ActivityManager am = app.getSystemService(ActivityManager.class);
        shadowOf(am).setProcesses(java.util.Collections.emptyList());
        LauncherActivity a = launch(null);
        assertNull("not started", shadowOf(a).getNextStartedActivity());
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertEquals(app.getString(R.string.HOME_OTHER_XBE_NOTE), a.noteForTest());
    }

    /** The same default.xbe as the pack's: Play starts the game (the hash is the game folder's, read once). */
    @Test
    public void sameXbePlays() throws Exception {
        withToolchain();
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        File xbe = new File(GameStore.defaultGameDir(app), "default.xbe");
        packField("xbe_sha256", GameStore.sha256(xbe));
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch(null);
        press(a, KeyEvent.KEYCODE_BUTTON_START);
        Intent next = shadowOf(a).getNextStartedActivity();
        assertNotNull(next);
        assertEquals(ShellActivity.class.getName(), next.getComponent().getClassName());
    }

    private void withToolkitDigest(String digest) throws Exception {
        File assets = tmp.newFolder("assets");
        Toolchain.assetsOverride = TestSupport.toolAssets(assets, "1");
        TestSupport.write(new File(assets, "prepare/toolkit.zip.id"), digest + "\n");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
    }

    /**
     * After an app update whose toolkit changed: the pack still plays (same shell ABI), Home says it was prepared by an
     * older version and offers Prepare again next to Play, never in its place.
     */
    @Test
    public void olderToolkitStillPlaysAndOffersPrepareAgain() throws Exception {
        withToolkitDigest(hex("11"));
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        packField("toolkit_sha256", hex("22"));
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch("10-older-toolkit");
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull("playable", a.homePrepForTest());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertTrue(a.noteForTest(), a.noteForTest().startsWith("This update translates games better"));
        Widgets.PillButton again = pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN));
        assertNotNull("Prepare again is offered", again);

        // Prepare again: a plain Prepare.
        again.performClick();
        shadowOf(Looper.getMainLooper()).idle();
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertTrue("not a self-healing round: the game does not start by itself after it",
                   !SelfHeal.read(new File(new File(app.getFilesDir(), "work"), TID)).relaunch);
    }

    @Test
    public void olderToolkitPlayIsNotBlocked() throws Exception {
        withToolkitDigest(hex("11"));
        TestSupport.writePack(app.getFilesDir(), TID, 64);   // from before toolkit_sha256: an older toolkit
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch(null);
        assertTrue(a.noteForTest(), a.noteForTest().startsWith("This update translates games better"));
        press(a, KeyEvent.KEYCODE_BUTTON_START);
        Intent next = shadowOf(a).getNextStartedActivity();
        assertNotNull(next);
        assertEquals(ShellActivity.class.getName(), next.getComponent().getClassName());
    }

    /**
     * Profile-free WP6: the Enhancements switch differs from what the pack was built with (pack.json title_layer):
     * Home offers Prepare again next to Play, as for a newer toolkit; switched back, it says nothing.
     */
    @Test
    public void enhancementsChangedOffersPrepareAgain() throws Exception {
        withToolkitDigest(hex("11"));
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        packField("toolkit_sha256", hex("11"));
        packField("title_layer", "on");
        assertEquals("on", Pack.titleLayer(app.getFilesDir(), TID));
        assertTrue(GameStore.setEnhancements(app, TID, false));
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch(null);
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertTrue(a.noteForTest(), a.noteForTest().startsWith("The Enhancements setting"));
        assertNotNull(pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN)));

        assertTrue(GameStore.setEnhancements(app, TID, true));
        a = launch(null);
        assertNull(pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN)));
        // A pack that says nothing (no enhancements for the title) never differs.
        packField("title_layer", "");
        assertNull(Pack.titleLayer(app.getFilesDir(), TID));
        assertTrue(GameStore.setEnhancements(app, TID, false));
        a = launch(null);
        assertNull(pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN)));
    }

    @Test
    public void currentToolkitSaysNothing() throws Exception {
        withToolkitDigest(hex("11"));
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        packField("toolkit_sha256", hex("11"));
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        LauncherActivity a = launch(null);
        assertNull(pillNamed(a.getWindow().getDecorView(), app.getString(R.string.PREPARE_AGAIN)));
        String note = a.noteForTest();
        assertTrue(note, note == null || !note.startsWith("This update translates games better"));
    }
}
