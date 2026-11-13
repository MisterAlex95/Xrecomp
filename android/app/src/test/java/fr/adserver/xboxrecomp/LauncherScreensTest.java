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
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

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
        LauncherActivity.lsfgBuiltOverride = null;
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
            assertTrue(name + ": a focused row or button", a.focusForTest() instanceof Widgets.Selectable);
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
        // No image: the theme's colours from the title ID's hue, no hero.
        GameTheme g = GameTheme.load(app, TID);
        assertNotNull(g);
        assertEquals("none", g.source);
        assertEquals(g.colors.get(GamePalette.GLOW), a.backdropForTest().glowForTest());
        assertNull(a.backdropForTest().heroForTest());
    }

    @Test
    public void homeReadyWithTitleImage() throws Exception {
        writeXbe(TestSupport.xbeWithImage(0x4D530004, "Sample Game",
                                          TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture())));
        render("home-ready-art", true, null);
        LauncherActivity a = launch();
        assertTrue(a.hasArtForTest());
        assertTrue(TitleArt.cacheFile(app.getFilesDir(), TID).isFile());
        assertEquals(Theme.GLOW, a.backdropForTest().glowForTest());
        assertTrue(Theme.GLOW != GamePalette.STOCK[GamePalette.ACCENT]);
        assertNotNull(a.backdropForTest().heroForTest());
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
        render("prepare-running", false, null);   // nothing to focus while it runs: Y hides, B cancels
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

    /** Calm: Home is one row of buttons (a solid Play, three quiet pills); A and B are the hints, Start still plays. */
    @Test
    public void homeIsOneRowOfButtons() throws Exception {
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        Widgets.PillButton play = a.homePrimaryForTest();
        assertTrue(play.primaryForTest());
        assertTrue(a.focusForTest() == play);
        List<String> pills = new ArrayList<>();
        View v = play;
        for (int i = 0; i < 3; i++) {
            v = v.focusSearch(View.FOCUS_RIGHT);
            assertTrue(v instanceof Widgets.PillButton);
            assertFalse(((Widgets.PillButton)v).primaryForTest());
            pills.add(((Widgets.PillButton)v).title);
            v.requestFocus();
            idle(200);
        }
        assertEquals(java.util.Arrays.asList(app.getString(R.string.SETTINGS), app.getString(R.string.CONTROLS),
                                             app.getString(R.string.HOME_MANAGE)), pills);
        List<String> hints = new ArrayList<>();
        for (Widgets.Hint h : a.hintsForTest())
            hints.add(h.glyphForTest());
        assertEquals(java.util.Arrays.asList("A", "B"), hints);
        assertEquals(Theme.ACCENT, play.focusedFillForTest());
    }

    /** Calm: Preparing is Home's layout with the progress; no focusable, Y hides and B cancels. */
    @Test
    public void preparingHasNoRowToFocus() throws Exception {
        withToolchain();
        status(PrepareProtocol.RUNNING, "compile", 5, 42, 760, "func_00012a40.c", "");
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        a.forceFocusForTest();
        assertNull(a.focusForTest());
        List<String> hints = new ArrayList<>();
        for (Widgets.Hint h : a.hintsForTest())
            hints.add(h.glyphForTest() + " " + h.labelForTest());
        assertEquals(java.util.Arrays.asList("Y " + app.getString(R.string.HINT_HIDE),
                                             "B " + app.getString(R.string.CANCEL)), hints);
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
    }

    // ---- The other screens -----------------------------------------------------------------------------------

    @Test
    public void otherScreens() throws Exception {
        render("settings", true, a -> a.goForTest(LauncherActivity.Screen.SETTINGS));
        render("settings-graphics", true, a -> {
            a.goForTest(LauncherActivity.Screen.SETTINGS);
            idle(300);
            key(a, KeyEvent.KEYCODE_BUTTON_R1);   // the Graphics tab
            a.forceFocusForTest();
            for (int i = 0; i < 4; i++) {   // down to Aspect ratio
                View next = a.focusForTest().focusSearch(View.FOCUS_DOWN);
                next.requestFocus();
                idle(200);
            }
            key(a, KeyEvent.KEYCODE_DPAD_RIGHT);   // a value changes: it pulses
        });
        render("settings-advanced", true, a -> {
            a.goForTest(LauncherActivity.Screen.SETTINGS);
            idle(300);
            key(a, KeyEvent.KEYCODE_BUTTON_L1);   // wraps to Advanced
        });
        render("controls", false, a -> a.goForTest(LauncherActivity.Screen.CONTROLS));
        render("game-data", true, a -> a.goForTest(LauncherActivity.Screen.GAME_DATA));
        render("storage", true, a -> a.goForTest(LauncherActivity.Screen.STORAGE));
        render("about", false, a -> a.goForTest(LauncherActivity.Screen.ABOUT));
        render("licences", false, a -> a.goForTest(LauncherActivity.Screen.LICENCES));
        render("source", true, a -> a.goForTest(LauncherActivity.Screen.SOURCE));
    }

    // ---- Game theme ------------------------------------------------------------------------------------------

    private void withArt(byte[] xpr) throws Exception {
        GameTheme.clear(app, TID);   // the cached title image too: a test swaps the art under one title ID
        writeXbe(TestSupport.xbeWithImage(0x4D530004, "Sample Game", xpr));
    }

    /** Home in a blue, a warm and a grey (no colour: the title ID's hue, the hero still baked) game's theme. */
    @Test
    public void homeThemed() throws Exception {
        withArt(TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture()));
        render("home-themed-blue", true, null);
        withArt(TestSupport.xprArgb(64, 64, GamePaletteTest.image("flame")));
        render("home-themed-warm", true, null);
        withArt(TestSupport.xprSolid(64, 64, 0xFF5A5A5C));
        render("home-themed-grey", true, null);
        assertNotNull(launch().backdropForTest().heroForTest());
    }

    /** The accent the stage was built with: the focus outline's (the stage is built again when the theme changes). */
    private static int stageAccent(LauncherActivity a) {
        return a.glideForTest().outlineColorForTest();
    }

    private void themeStep(LauncherActivity a) {
        Widgets.MenuRow r = row(a, app.getString(R.string.GAME_THEME));
        assertNotNull("Settings › Launcher › Game theme", r);
        r.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        idle(300);
    }

    /** The game's colours everywhere at once, the status green kept; Settings › Game theme: colours only, off. */
    @Test
    public void themeFollowsTheGameAndTheSetting() throws Exception {
        withArt(TestSupport.xprSolid(64, 64, 0xFF2850DC));
        LauncherActivity a = launch();
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        assertEquals(GamePalette.STOCK[GamePalette.OK], Theme.OK);
        assertEquals(Theme.ACCENT, stageAccent(a));
        assertNotNull(a.backdropForTest().heroForTest());
        assertEquals(Theme.GLOW, a.backdropForTest().glowForTest());

        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        Widgets.MenuRow r = row(a, app.getString(R.string.GAME_THEME));
        assertEquals(app.getString(R.string.VAL_THEME_FULL), r.value);
        themeStep(a);
        assertEquals(GameTheme.Mode.COLORS, GameTheme.mode(app, TID));
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        assertNull("colours only: no hero", a.backdropForTest().heroForTest());
        assertEquals(app.getString(R.string.VAL_THEME_COLORS), row(a, app.getString(R.string.GAME_THEME)).value);
        shot(a, "ui-settings-theme-colors", false);
        themeStep(a);
        assertEquals(GameTheme.Mode.OFF, GameTheme.mode(app, TID));
        assertTrue("off: the stock colours", Theme.isStock());
        assertEquals(GamePalette.STOCK[GamePalette.ACCENT], stageAccent(a));
        assertEquals(LauncherActivity.Screen.SETTINGS, a.screenForTest());
        shot(a, "ui-settings-theme-off", false);
        themeStep(a);
        assertEquals(GameTheme.Mode.FULL, GameTheme.mode(app, TID));
        assertEquals(0xFFA7C2FF, stageAccent(a));

        // A fresh launcher starts in the game's colours (from the cache: no stock flash).
        Theme.reset();
        LauncherActivity b = launch();
        assertEquals(0xFFA7C2FF, Theme.ACCENT);
        assertEquals(0xFFA7C2FF, stageAccent(b));
    }

    /** No game set up: the stock colours, and no Game theme row. */
    @Test
    public void noGameNoTheme() throws Exception {
        new File(GameStore.defaultGameDir(app), "default.xbe").delete();
        LauncherActivity a = launch();
        assertTrue(Theme.isStock());
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        assertNull(row(a, app.getString(R.string.GAME_THEME)));
    }

    /**
     * The hero backdrop alone (no views): over its brightest pixel in the content area (between the dots of the
     * grid), TEXT_FAINT keeps 3:1 and TEXT_DIM 4.5:1, for a bright image (a white and blue checker).
     */
    @Test
    public void heroKeepsSmallTextReadable() {
        int n = 128;
        int[] px = new int[n * n];
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++)
                px[y * n + x] = (x / 16 + y / 16) % 2 == 0 ? 0xFFFFFFFF : 0xFF2850DC;
        }
        GamePalette.Palette p = GamePalette.build(GamePalette.analyse(px, n, n), 212);
        Theme.apply(new Theme.Colors(p.colors));
        GamePalette.Image h = GamePalette.bakeHero(px, n, n);
        Widgets.Backdrop b = new Widgets.Backdrop();
        b.setGlows(Theme.GLOW, Theme.GLOW2);
        b.setHero(android.graphics.Bitmap.createBitmap(h.px, h.w, h.h, android.graphics.Bitmap.Config.ARGB_8888));
        android.graphics.Bitmap out = android.graphics.Bitmap.createBitmap(1920, 1080,
                                                                            android.graphics.Bitmap.Config.ARGB_8888);
        b.setBounds(0, 0, 1920, 1080);
        b.draw(new android.graphics.Canvas(out));
        double max = 0;
        for (int y = 72; y < 1008; y++) {
            if (y % 48 <= 3 || y % 48 >= 45)
                continue;
            for (int x = 72; x < 1848; x++) {
                if (x % 48 <= 3 || x % 48 >= 45)
                    continue;
                max = Math.max(max, GamePalette.luminance(out.getPixel(x, y)));
            }
        }
        double faint = (GamePalette.luminance(Theme.TEXT_FAINT) + 0.05) / (max + 0.05);
        double dim = (GamePalette.luminance(Theme.TEXT_DIM) + 0.05) / (max + 0.05);
        assertTrue("TEXT_FAINT over the hero: " + faint, faint >= 3.0);
        assertTrue("TEXT_DIM over the hero: " + dim, dim >= 4.5);
        assertTrue("the hero shows: " + max, max > GamePalette.luminance(Theme.BG_TOP));
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

    /** XBA-121: Graphics, Widescreen and 480p write RECOMP_WIDESCREEN / RECOMP_480P to recomp.cfg; Off removes them. */
    @Test
    public void settingsConsoleVideo() throws Exception {
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        assertNull("not on General", row(a, app.getString(R.string.OPT_WIDESCREEN)));
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        Widgets.MenuRow wide = row(a, app.getString(R.string.OPT_WIDESCREEN));
        Widgets.MenuRow p480 = row(a, app.getString(R.string.OPT_480P));
        assertNotNull("Settings, Graphics, Widescreen", wide);
        assertNotNull("Settings, Graphics, 480p", p480);
        assertEquals(app.getString(R.string.VAL_OFF), wide.value);
        assertEquals(app.getString(R.string.VAL_OFF), p480.value);
        assertEquals(app.getString(R.string.OPT_WIDESCREEN_DETAIL), wide.description);
        File ext = GameStore.externalFilesDir(app);
        assertNotNull(ext);

        // Each step is saved at once.
        wide.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(app.getString(R.string.VAL_ON), wide.value);
        p480.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(app.getString(R.string.VAL_ON), p480.value);
        RecompConfig c = RecompConfig.load(ext);
        assertEquals("1", c.get(RecompConfig.ENV_WIDESCREEN));
        assertEquals("1", c.get(RecompConfig.ENV_480P));

        // A fresh Settings screen reads them back; Off is the runtime default and writes no line.
        a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        wide = row(a, app.getString(R.string.OPT_WIDESCREEN));
        assertEquals("read back from recomp.cfg", app.getString(R.string.VAL_ON), wide.value);
        wide.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);   // wraps to Off
        assertEquals(app.getString(R.string.VAL_OFF), wide.value);
        c = RecompConfig.load(ext);
        assertNull(c.get(RecompConfig.ENV_WIDESCREEN));
        assertEquals("1", c.get(RecompConfig.ENV_480P));
    }

    /** Calm: Settings' tabs: L1 / R1 switch them (wrapping), each keeps its focus; Frame cap follows the renderer. */
    @Test
    public void settingsTabs() throws Exception {
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        assertEquals(LauncherActivity.TAB_GENERAL, a.settingsTabForTest());
        assertNotNull(row(a, app.getString(R.string.UI_SOUND)));
        a.forceFocusForTest();
        View second = a.focusForTest().focusSearch(View.FOCUS_DOWN);
        second.requestFocus();
        idle(200);
        String keep = ((Widgets.MenuRow)a.focusForTest()).title;
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(LauncherActivity.TAB_GRAPHICS, a.settingsTabForTest());
        assertNull(row(a, app.getString(R.string.UI_SOUND)));
        assertNotNull(row(a, app.getString(R.string.OPT_VSYNC)));
        // Frame cap and Resolution only matter with the GPU renderer: Auto is the GPU renderer (XBA-119), so they
        // are dimmed only with Software.
        Widgets.MenuRow cap = row(a, app.getString(R.string.OPT_GPU_FPS_CAP));
        Widgets.MenuRow gpu = row(a, app.getString(R.string.OPT_GPU));
        assertEquals(app.getString(R.string.VAL_AUTO), gpu.value);
        assertFalse(cap.dim);
        gpu.requestFocusFromTouch();
        while (!app.getString(R.string.VAL_GPU_CPU).equals(gpu.value))
            key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertTrue(cap.dim);
        assertTrue(row(a, app.getString(R.string.OPT_GPU_SCALE)).dim);
        while (!app.getString(R.string.VAL_GPU_GL).equals(gpu.value))
            key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertFalse(cap.dim);
        assertFalse(row(a, app.getString(R.string.OPT_GPU_SCALE)).dim);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(LauncherActivity.TAB_ADVANCED, a.settingsTabForTest());
        assertNotNull(row(a, app.getString(R.string.RESET)));
        assertNotNull("About ends Advanced", row(a, app.getString(R.string.ABOUT)));
        key(a, KeyEvent.KEYCODE_BUTTON_R1);   // wraps
        assertEquals(LauncherActivity.TAB_GENERAL, a.settingsTabForTest());
        a.forceFocusForTest();
        idle(200);
        assertEquals("General keeps its focus", keep, ((Widgets.MenuRow)a.focusForTest()).title);
        key(a, KeyEvent.KEYCODE_BUTTON_L1);   // wraps back
        assertEquals(LauncherActivity.TAB_ADVANCED, a.settingsTabForTest());
        Widgets.Tabs tabs = null;
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>()))
            if (v instanceof Widgets.Tabs)
                tabs = (Widgets.Tabs)v;
        assertNotNull(tabs);
        assertEquals(LauncherActivity.TAB_ADVANCED, tabs.activeForTest());
        assertEquals(Theme.ACCENT, tabs.accentForTest());
        // The focused row's description, and nothing else, in the column.
        row(a, app.getString(R.string.RESET)).requestFocusFromTouch();
        idle(200);
        assertTrue(a.descriptionForTest().startsWith(app.getString(R.string.RESET_DETAIL)));
    }

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    /** U4: the screen change, the glide and the focus animate for the durations of Theme. */
    @Test
    public void motionDurations() throws Exception {
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        assertEquals(Theme.ANIM_SCREEN_MS, a.screenAnimatorForTest().getDuration());
        assertEquals(150, Theme.ANIM_SCREEN_MS);
        Widgets.Selectable play = (Widgets.Selectable)a.focusForTest();
        assertEquals(Theme.ANIM_FOCUS_MS, play.focusAnimatorForTest().getDuration());
        assertEquals(Theme.FOCUS_SCALE, play.getScaleX(), 0.001f);
        assertTrue(a.glideForTest().targetForTest() == play);
        View next = play.focusSearch(View.FOCUS_RIGHT);   // Home's row of buttons: Play, then Settings
        assertTrue(next instanceof Widgets.PillButton);
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

    /** XBA-100: X on About opens the licence texts the APK carries, LICENSE first, the D-pad scrolls them. */
    @Test
    public void licencesFromAbout() throws Exception {
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.ABOUT);
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        idle(300);
        String t = a.licencesForTest();
        assertNotNull("X opens Licences", t);
        assertTrue(t, t.startsWith("──── LICENSE ────"));
        assertTrue("sp00nz's MIT notice", t.contains("Copyright (c) 2026 sp00nz"));
        for (String part : new String[] { "──── NOTICE ────", "SDL2 2.32.10", "──── LGPL-2.1 ────",
                                          "LLVM Exceptions", "PYTHON SOFTWARE FOUNDATION LICENSE", "Capstone" })
            assertTrue(part, t.contains(part));
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);   // scrolls, does not leave the screen
        idle(300);
        assertNotNull(a.licencesForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        idle(300);
        assertNull("B goes back to About", a.licencesForTest());
    }

    /** Calm: every screen shows at most three hints, in one footer line. */
    @Test
    public void atMostThreeHints() throws Exception {
        withToolchain();
        LauncherActivity a = launch();
        for (LauncherActivity.Screen s : new LauncherActivity.Screen[] { LauncherActivity.Screen.HOME,
                LauncherActivity.Screen.SETTINGS, LauncherActivity.Screen.CONTROLS, LauncherActivity.Screen.GAME_DATA,
                LauncherActivity.Screen.ABOUT, LauncherActivity.Screen.LICENCES, LauncherActivity.Screen.SOURCE, LauncherActivity.Screen.STORAGE,
                LauncherActivity.Screen.WELCOME }) {
            a.goForTest(s);
            idle(300);
            int n = a.hintsForTest().size();
            assertTrue(s + ": " + n + " hints", n >= 1 && n <= LauncherActivity.MAX_HINTS);
        }
    }

    /** U4: moves, selects, backs and errors play their cue (and the cues respect the settings). */
    @Test
    public void feedbackCues() throws Exception {
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(200);
        List<UiFeedback.Cue> played = a.feedbackForTest().played;
        played.clear();
        a.focusForTest().focusSearch(View.FOCUS_RIGHT).requestFocus();
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

    /** WP-E: a pack whose title profile forces RECOMP_LSFG=0 (env_force) shows it on the frame generation row. */
    @Test
    public void frameGenerationUnavailableForThisTitle() throws Exception {
        LauncherActivity.lsfgBuiltOverride = true;
        TestSupport.writePack(app.getFilesDir(), TID, 64);
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        assertNull("not on General", row(a, app.getString(R.string.OPT_LSFG)));
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        Widgets.MenuRow r = row(a, app.getString(R.string.OPT_LSFG));
        assertNotNull("Settings › Graphics › Frame generation", r);
        assertEquals(app.getString(R.string.OPT_LSFG_DETAIL), r.description);
        assertFalse(r.dim);

        File json = new File(Pack.packDir(app.getFilesDir(), TID), "pack.json");
        org.json.JSONObject o = new org.json.JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("env_force", new org.json.JSONObject().put("RECOMP_LSFG", "0"));
        TestSupport.write(json, o.toString());
        a.goForTest(LauncherActivity.Screen.HOME);
        idle(300);
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        r = row(a, app.getString(R.string.OPT_LSFG));
        assertNotNull(r);
        assertEquals(app.getString(R.string.LSFG_TITLE_UNAVAILABLE), r.description);
        assertTrue(r.dim);
        // The setting itself still cycles (it is global: the other games keep it).
        r.requestFocusFromTouch();
        idle(200);
        assertTrue(a.descriptionForTest().contains(app.getString(R.string.LSFG_TITLE_UNAVAILABLE_DETAIL)));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(app.getString(R.string.VAL_LSFG_2X), r.value);
    }
}
