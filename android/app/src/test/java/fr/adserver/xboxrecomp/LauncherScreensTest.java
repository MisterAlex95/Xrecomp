package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.os.Looper;
import android.view.KeyEvent;
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
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;

/**
 * The launcher's screens and states rendered to build/screenshots/ui-<name>-<en|fr>-<720|1080>.png for review (each
 * with its focused row, as on a gamepad), and the behaviour of the UI pass: the overlap-free setting rows, the hero's
 * states, the title image and monogram, the motion (durations), the sounds and haptics, Controls, the idle dim.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherScreensTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String TID = "4D530004";
    private static final String[] LOCALES = { "en", "fr" };
    private static final String[][] SIZES = { { "720", "w1280dp-h720dp-land-mdpi" },
                                              { "1080", "w1920dp-h1080dp-land-mdpi" } };

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        writeXbe(TestSupport.xbe(0x4D530004, "Sample Game"));
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
        LauncherActivity.clock = android.os.SystemClock::uptimeMillis;
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private void writeXbe(byte[] b) throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), b);
    }

    private void withToolchain() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder(), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.newFolder()).getAbsolutePath();
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
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static void key(LauncherActivity a, int code) {
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, code));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, code));
        idle(300);
    }

    private LauncherActivity launch() {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        return a;
    }

    /** Draws the activity once the motion has settled, with its focus brought as a D-pad press does. */
    private File shot(LauncherActivity a, String name, boolean wantFocus) throws Exception {
        if (wantFocus && a.focusForTest() == null)
            a.forceFocusForTest();
        idle(400);
        if (wantFocus)
            assertTrue(name + ": a focused row", a.focusForTest() instanceof Widgets.MenuRow);
        File f = TestSupport.screenshot(a.getWindow().getDecorView(), name);
        assertTrue(f.length() > 0);
        return f;
    }

    /** Renders one state in en / fr at 720p / 1080p: act opens it on a fresh activity. */
    private void render(String name, boolean wantFocus, Consumer<LauncherActivity> act) throws Exception {
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                LauncherActivity a = launch();
                if (act != null)
                    act.accept(a);
                shot(a, "ui-" + name + "-" + loc + "-" + size[0], wantFocus);
            }
        }
    }

    private static List<Widgets.MenuRow> rows(View v, List<Widgets.MenuRow> out) {
        if (v instanceof Widgets.MenuRow)
            out.add((Widgets.MenuRow)v);
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup)v;
            for (int i = 0; i < g.getChildCount(); i++)
                rows(g.getChildAt(i), out);
        }
        return out;
    }

    private static Widgets.MenuRow row(LauncherActivity a, String title) {
        for (Widgets.MenuRow r : rows(a.getWindow().getDecorView(), new ArrayList<>()))
            if (title.equals(r.title))
                return r;
        return null;
    }

    // ---- Home ------------------------------------------------------------------------------------------------

    @Test
    public void homeReadyWithMonogram() throws Exception {
        long now = System.currentTimeMillis();
        GameStore.sessionStart(app, now - 3 * 3600_000L);
        GameStore.sessionEnd(app, now - 3600_000L);   // two hours, an hour ago
        render("home-ready-monogram", true, null);
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertFalse("no title image in this XBE: the monogram", a.hasArtForTest());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertEquals(TitleArt.monogramColor(TID), a.backdropForTest().glowForTest());
    }

    @Test
    public void homeReadyWithTitleImage() throws Exception {
        writeXbe(TestSupport.xbeWithImage(0x4D530004, "Sample Game",
                                          TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture())));
        render("home-ready-art", true, null);
        LauncherActivity a = launch();
        assertTrue(a.hasArtForTest());
        assertTrue(TitleArt.cacheFile(app.getFilesDir(), TID).isFile());
        assertTrue(a.backdropForTest().glowForTest() != Theme.ACCENT);
    }

    @Test
    public void homeNoGame() throws Exception {
        new File(GameStore.defaultGameDir(app), "default.xbe").delete();
        render("home-no-game", true, a -> {
            assertEquals(LauncherActivity.Screen.WELCOME, a.screenForTest());
            a.goForTest(LauncherActivity.Screen.HOME);
        });
        render("welcome", true, null);
    }

    @Test
    public void homeNeedsPrepare() throws Exception {
        withToolchain();
        render("home-needs-prepare", true, null);
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertEquals(app.getString(R.string.PREPARE), a.homePrimaryForTest().title);
    }

    @Test
    public void homePreparing() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, "compile", 4, 42, 760, "func_00012a40.c (1 893 of 4 512)", "");
        render("home-preparing", true, a -> {
            assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
            key(a, KeyEvent.KEYCODE_BUTTON_Y);   // Menu: Home, the Prepare carries on
        });
        render("prepare-running", true, null);
    }

    @Test
    public void homeFailed() throws Exception {
        withToolchain();
        status(PrepareProtocol.FAILED, "compile", 4, 37, -1,
               "clang: error: 3 files failed to compile (func_00012a40.c, func_00012b00.c, …)", "CC_FAILED");
        render("prepare-failed", true, null);
        render("home-failed", true, a -> key(a, KeyEvent.KEYCODE_BUTTON_B));
    }

    @Test
    public void homeReadyPrepared() throws Exception {
        withToolchain();
        TestSupport.writePack(app.getFilesDir(), TID, 48 << 20);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        GameStore.sessionStart(app, System.currentTimeMillis() - 40 * 60_000L);
        GameStore.sessionEnd(app, System.currentTimeMillis() - 2 * 60_000L);
        render("home-ready-prepared", true, null);
    }

    // ---- The other screens -----------------------------------------------------------------------------------

    @Test
    public void otherScreens() throws Exception {
        render("settings", true, a -> a.goForTest(LauncherActivity.Screen.SETTINGS));
        render("settings-graphics", true, a -> {
            a.goForTest(LauncherActivity.Screen.SETTINGS);
            idle(300);
            a.forceFocusForTest();
            for (int i = 0; i < 4; i++) {
                View next = a.focusForTest().focusSearch(View.FOCUS_DOWN);
                next.requestFocus();
                idle(200);
            }
            key(a, KeyEvent.KEYCODE_DPAD_RIGHT);   // a value changes: it pulses
        });
        render("controls", false, a -> a.goForTest(LauncherActivity.Screen.CONTROLS));
        render("game-data", true, a -> a.goForTest(LauncherActivity.Screen.GAME_DATA));
        render("about", false, a -> a.goForTest(LauncherActivity.Screen.ABOUT));
        render("source", true, a -> a.goForTest(LauncherActivity.Screen.SOURCE));
    }

    // ---- Behaviour -------------------------------------------------------------------------------------------

    /** U1: Settings has the launcher rows (open directly, sounds, vibration); long titles keep off the value. */
    @Test
    public void settingsRows() throws Exception {
        RuntimeEnvironment.setQualifiers("fr-w1280dp-h720dp-land-mdpi");
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        Widgets.MenuRow open = row(a, app.getString(R.string.OPEN_DIRECTLY));
        Widgets.MenuRow sound = row(a, app.getString(R.string.UI_SOUND));
        Widgets.MenuRow buzz = row(a, app.getString(R.string.UI_HAPTICS));
        assertNotNull(open);
        assertNotNull(sound);
        assertNotNull(buzz);
        assertEquals(app.getString(R.string.VAL_ON), sound.value);
        sound.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertFalse(GameStore.uiSound(app));
        assertEquals(app.getString(R.string.VAL_OFF), sound.value);
        assertEquals(Theme.ANIM_TICK_MS, sound.tickAnimatorForTest().getDuration());
        buzz.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertFalse(GameStore.uiHaptics(app));
        // Home has no « Open directly » toggle any more: it lives in Settings only.
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        boolean before = GameStore.autoStart(app);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals(before, GameStore.autoStart(app));
    }

    /** U4: the screen change, the glide and the focus animate for the durations of Theme. */
    @Test
    public void motionDurations() throws Exception {
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        assertEquals(Theme.ANIM_SCREEN_MS, a.screenAnimatorForTest().getDuration());
        assertEquals(150, Theme.ANIM_SCREEN_MS);
        Widgets.MenuRow play = (Widgets.MenuRow)a.focusForTest();
        assertEquals(Theme.ANIM_FOCUS_MS, play.focusAnimatorForTest().getDuration());
        assertEquals(Theme.FOCUS_SCALE, play.getScaleX(), 0.001f);
        assertTrue(a.glideForTest().targetForTest() == play);
        View next = play.focusSearch(View.FOCUS_DOWN);
        assertTrue(next instanceof Widgets.MenuRow);
        next.requestFocus();
        idle(40);
        assertNotNull(a.glideForTest().animatorForTest());
        assertEquals(Theme.ANIM_GLIDE_MS, a.glideForTest().animatorForTest().getDuration());
        idle(300);
        assertTrue(a.glideForTest().targetForTest() == next);
        assertEquals(1f, play.getScaleX(), 0.001f);
        a.goForTest(LauncherActivity.Screen.ABOUT);
        assertTrue(a.screenAnimatorForTest().isRunning());
        idle(300);
        assertFalse(a.screenAnimatorForTest().isRunning());
    }

    /** U4: moves, selects, backs and errors play their cue (and the cues respect the settings). */
    @Test
    public void feedbackCues() throws Exception {
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(200);
        List<UiFeedback.Cue> played = a.feedbackForTest().played;
        played.clear();
        a.focusForTest().focusSearch(View.FOCUS_DOWN).requestFocus();
        assertEquals(UiFeedback.Cue.MOVE, played.get(played.size() - 1));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(played.toString(), UiFeedback.Cue.SELECT, played.get(played.size() - 1));
        assertTrue(a.screenForTest() != LauncherActivity.Screen.HOME);
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(UiFeedback.Cue.BACK, played.get(played.size() - 1));
    }

    /** U4: the five UI sounds are there and small (tools/android/ui_sounds.sh). */
    @Test
    public void uiSoundsAreSmall() {
        File raw = new File("src/main/res/raw");
        long total = 0;
        for (String n : new String[] { "move", "select", "back", "error", "done" }) {
            File f = new File(raw, "ui_" + n + ".wav");
            assertTrue(f + " exists", f.isFile());
            total += f.length();
        }
        assertTrue("UI sounds: " + total + " bytes", total < 200_000);
    }

    /** U2: a play session from the launcher: started with the shell, closed when the launcher comes back. */
    @Test
    public void playTime() throws Exception {
        assertEquals(0, GameStore.lastPlayed(app));
        LauncherActivity a = launch();
        a.forceFocusForTest();
        key(a, KeyEvent.KEYCODE_BUTTON_START);
        assertNotNull(shadowOf(a).getNextStartedActivity());
        long started = GameStore.lastPlayed(app);
        assertTrue(started > 0);
        GameStore.sessionEnd(app, started + 25 * 60_000L);
        assertEquals(25 * 60_000L, GameStore.playTime(app));
        GameStore.sessionEnd(app, started + 50 * 60_000L);   // already closed: nothing more
        assertEquals(25 * 60_000L, GameStore.playTime(app));
        GameStore.sessionStart(app, started);
        GameStore.sessionEnd(app, started + 3 * GameStore.SESSION_MAX);
        assertEquals(25 * 60_000L + GameStore.SESSION_MAX, GameStore.playTime(app));
    }

    /** U5: the Preparing screen dims after two idle minutes (a fake clock), and a key only wakes it. */
    @Test
    public void preparingDimsWhenIdle() throws Exception {
        withToolchain();
        long[] now = { 1_000_000 };
        LauncherActivity.clock = () -> now[0];
        status(PrepareProtocol.RUNNING, "compile", 4, 42, 760, "func_00012a40.c (1 893 of 4 512)", "");
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        now[0] += LauncherActivity.DIM_AFTER_MS - 1000;
        status(PrepareProtocol.RUNNING, "compile", 4, 43, 750, "func_00012a44.c", "");
        idle(600);
        assertFalse(a.dimmedForTest());
        now[0] += 2000;
        status(PrepareProtocol.RUNNING, "compile", 4, 44, 740, "func_00012a48.c", "");
        idle(600);
        assertTrue(a.dimmedForTest());
        for (String loc : LOCALES)
            TestSupport.screenshot(a.getWindow().getDecorView(), "ui-prepare-dimmed-" + loc + "-1080");
        // B wakes the screen and does nothing else (no cancel armed).
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertFalse(a.dimmedForTest());
        assertNull(shadowOf(app).getNextStartedService());
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertTrue(a.noteForTest().isEmpty());
    }

    /** Dual screen: a second display shows Home's panel; Settings offers « Bottom screen » only then. */
    @Test
    public void secondDisplayShowsTheLauncherPanel() throws Exception {
        LauncherActivity one = launch();
        assertFalse(one.companionForTest().screen().isShowing());
        one.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        assertNull("one screen: no Bottom screen row", row(one, app.getString(R.string.BOTTOM_SCREEN)));

        int id = org.robolectric.shadows.ShadowDisplayManager.addDisplay("w1240dp-h1080dp-mdpi");
        idle(300);
        LauncherActivity a = launch();
        CompanionScreen screen = a.companionForTest().screen();
        assertTrue(screen.isShowing());
        assertEquals(id, screen.shownOn());
        assertTrue(screen.content() instanceof LauncherPanel);
        TestSupport.screenshot(screen.content(), "ui-dual-bottom-home");

        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        Widgets.MenuRow bottom = row(a, app.getString(R.string.BOTTOM_SCREEN));
        assertNotNull(bottom);
        assertEquals(app.getString(R.string.VAL_PANEL), bottom.value);
        bottom.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(GameStore.BottomScreen.BLACK, GameStore.bottomScreen(app));
        assertNull("Black: an empty window", screen.content());
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(GameStore.BottomScreen.OFF, GameStore.bottomScreen(app));
        assertFalse(screen.isShowing());
    }

    /** U5: when the Prepare ends, Home with Play focused and the « done » cue. */
    @Test
    public void prepareDoneFocusesPlay() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, "pack", 5, 90, 5, "libtitle.so", "");
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        status(PrepareProtocol.DONE, "pack", 5, 100, -1, "", "");
        idle(600);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertTrue(a.focusForTest() == a.homePrimaryForTest());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        List<UiFeedback.Cue> played = a.feedbackForTest().played;
        assertEquals(UiFeedback.Cue.DONE, played.get(played.size() - 1));
    }
}
