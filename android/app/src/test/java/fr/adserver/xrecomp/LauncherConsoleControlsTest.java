package fr.adserver.xrecomp;

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

/**
 * XBA-158 / XBA-168 / XBA-174: a GBA, PS1 or N64 game's Controls, from its Home's « Controls » pill, with a pad only:
 * the console's own buttons, each remapped to the player's (RECOMP_GBA_PAD_MAP ... in recomp.cfg for all the console's
 * games), « This game only » (its own file, which ShellActivity hands to the runtime), Y back to the usual mapping,
 * the N64 stick's range and dead zone; rendered to build/screenshots/console-controls-*.png (en, fr).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherConsoleControlsTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File root;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        root = tmp.newFolder("xbox");
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    /** A key press as the window delivers it (the launcher's own handling, else ViewRootImpl's focus search). */
    private static void key(LauncherActivity a, int code) {
        boolean used = a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, code));
        int dir = code == KeyEvent.KEYCODE_DPAD_LEFT ? View.FOCUS_LEFT : code == KeyEvent.KEYCODE_DPAD_RIGHT
                ? View.FOCUS_RIGHT : code == KeyEvent.KEYCODE_DPAD_UP ? View.FOCUS_UP
                : code == KeyEvent.KEYCODE_DPAD_DOWN ? View.FOCUS_DOWN : 0;
        View f = a.focusForTest();
        if (!used && dir != 0 && f != null) {
            View next = f.focusSearch(dir);
            if (next != null)
                next.requestFocus(dir);
        }
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, code));
        idle(300);
    }

    private LauncherActivity launch() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        Thread t = a.scannerForTest();
        if (t != null)
            t.join(20_000);
        idle(400);
        return a;
    }

    /** One Xbox game (the library needs one) and a ROM of console c in a library folder of its own. */
    private File rom(String folder, String name, byte[] content) throws Exception {
        File d = new File(root, "Sample Racer");
        TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(0x58420001, "Sample Racer",
                                                                       XbeInfo.REGION_WORLD, 0, 0x100));
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
        File dir = tmp.newFolder(folder);
        File rom = new File(dir, name);
        TestSupport.write(rom, content);
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3A" + folder,
                          dir.getAbsolutePath());
        return rom;
    }

    private static <T extends View> List<T> views(View v, Class<T> type, List<T> out) {
        if (type.isInstance(v))
            out.add(type.cast(v));
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup)v;
            for (int i = 0; i < g.getChildCount(); i++)
                views(g.getChildAt(i), type, out);
        }
        return out;
    }

    private static Widgets.MenuRow row(LauncherActivity a, String title) {
        for (Widgets.MenuRow r : views(a.getWindow().getDecorView(), Widgets.MenuRow.class, new ArrayList<>()))
            if (title.equals(r.title))
                return r;
        return null;
    }

    private static boolean focusedOn(LauncherActivity a, String title) {
        return a.focusForTest() instanceof Widgets.MenuRow && title.equals(((Widgets.MenuRow)a.focusForTest()).title);
    }

    /** Down with the D-pad until the row titled title has the focus, else up. */
    private static void focusRow(LauncherActivity a, String title) {
        for (int i = 0; i < 40 && !focusedOn(a, title); i++)
            key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        for (int i = 0; i < 40 && !focusedOn(a, title); i++)
            key(a, KeyEvent.KEYCODE_DPAD_UP);
        assertTrue("focus on " + title, a.focusForTest() instanceof Widgets.MenuRow
                                         && title.equals(((Widgets.MenuRow)a.focusForTest()).title));
    }

    /** From the library: A on the card (its Home), then the Controls pill (right of the others) with the pad. */
    private void openControls(LauncherActivity a, String card) {
        openControls(a, card, R.string.CONTROLS, LauncherActivity.Screen.ROM_CONTROLS);
    }

    /** The same with another pill of the Home (XBA-181: « Options »), landing on screen. */
    private void openControls(LauncherActivity a, String card, int pill, LauncherActivity.Screen screen) {
        a.forceFocusForTest();
        idle(300);
        for (GameCard c : a.libraryCardsForTest())
            if (card.equals(c.titleForTest()))
                c.requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        a.forceFocusForTest();
        idle(300);
        String controls = app.getString(pill);
        for (int i = 0; i < 6 && !(a.focusForTest() instanceof Widgets.PillButton
                                   && controls.equals(((Widgets.PillButton)a.focusForTest()).title)); i++)
            key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertTrue("the " + controls + " pill", a.focusForTest() instanceof Widgets.PillButton
                                                && controls.equals(((Widgets.PillButton)a.focusForTest()).title));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(screen, a.screenForTest());
        a.forceFocusForTest();
        idle(300);
    }

    private String cfgLine(String key) {
        return RecompConfig.load(GameStore.externalFilesDir(app)).get(key);
    }

    private void shot(LauncherActivity a, String name) throws Exception {
        idle(400);
        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), name).length() > 0);
    }

    /**
     * A GBA game: every GBA button has a row with its usual controls (A = A + Y); right on A makes it B, written to
     * recomp.cfg for every GBA game; « This game only » copies the mapping into the game's own file, where a change
     * stays (recomp.cfg untouched) and runEnv hands it to the runtime; Y resets the shown scope; back to all games drops
     * the copy; B goes back to the Home.
     */
    @Test
    public void gbaRemapConsoleThenGame() throws Exception {
        File rom = rom("gba", "Pocket Quest.gba", ConsoleTest.gba("AXVE"));
        LauncherActivity a = launch();
        String key = LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(rom)).tid;
        openControls(a, "Pocket Quest");
        for (int t = 0; t < ConsolePadMap.targets(Console.GBA).length; t++)
            assertNotNull("row " + t, row(a, app.getString(ConsoleControls.targetLabel(Console.GBA, t))));
        assertNull("no stick rows on a GBA", row(a, app.getString(R.string.XB_LSTICK)));
        assertEquals("A + Y", row(a, app.getString(R.string.CC_A)).value);
        assertEquals(app.getString(R.string.CC_SCOPE_CONSOLE, "Game Boy Advance"),
                     row(a, app.getString(R.string.CC_SCOPE)).value);
        assertTrue(a.descriptionForTest(), a.descriptionForTest().contains("Game Boy Advance"));
        shot(a, "console-controls-1-gba");

        // A: right once -> B (east), for every GBA game.
        focusRow(a, app.getString(R.string.CC_A));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("a=east", cfgLine("RECOMP_GBA_PAD_MAP"));
        assertEquals(app.getString(R.string.SRC_EAST), row(a, app.getString(R.string.CC_A)).value);
        assertTrue(a.descriptionForTest(), a.descriptionForTest().contains(app.getString(R.string.CC_B)));   // shared
        assertNull("no own mapping yet", ConsoleControls.gameText(app, key));
        assertNull("the game follows recomp.cfg", ConsoleControls.runEnv(app, key)[0][1]);

        // This game only: a copy, then L on L2 alone for this game.
        focusRow(a, app.getString(R.string.CC_SCOPE));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_CONTROLS, a.screenForTest());
        assertEquals("a=east", ConsoleControls.gameText(app, key));
        assertEquals(app.getString(R.string.CC_SCOPE_GAME), row(a, app.getString(R.string.CC_SCOPE)).value);
        assertEquals(app.getString(R.string.CC_GAME_MAP_ON), a.noteForTest());
        a.forceFocusForTest();
        idle(300);
        focusRow(a, app.getString(R.string.CC_L));
        for (int i = 0; i < 6; i++)   // L1 + L2 -> L1 (first source) -> R1 -> L2: two steps from L1
            if (!app.getString(R.string.SRC_L2).equals(row(a, app.getString(R.string.CC_L)).value))
                key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(app.getString(R.string.SRC_L2), row(a, app.getString(R.string.CC_L)).value);
        assertEquals("a=east,l=left_trigger", ConsoleControls.gameText(app, key));
        assertEquals("recomp.cfg keeps the console's", "a=east", cfgLine("RECOMP_GBA_PAD_MAP"));
        String[][] env = ConsoleControls.runEnv(app, key);
        assertEquals("RECOMP_GBA_PAD_MAP", env[0][0]);
        assertEquals("a=east,l=left_trigger", env[0][1]);
        assertNull(env[1][1]);
        assertNull(env[2][1]);
        assertEquals("2 changed", 2, a.romMapForTest().changedCount());
        shot(a, "console-controls-2-gba-game");

        // Y: this game's mapping back to the usual one (still its own), recomp.cfg untouched.
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        assertEquals("", ConsoleControls.gameText(app, key));
        assertEquals("A + Y", row(a, app.getString(R.string.CC_A)).value);
        assertEquals("a=east", cfgLine("RECOMP_GBA_PAD_MAP"));
        assertEquals(app.getString(R.string.CTL_RESET_DONE), a.noteForTest());

        // Back to all games: the copy goes, the console's mapping shows again.
        a.forceFocusForTest();
        idle(300);
        focusRow(a, app.getString(R.string.CC_SCOPE));
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);
        assertNull(ConsoleControls.gameText(app, key));
        assertEquals(app.getString(R.string.SRC_EAST), row(a, app.getString(R.string.CC_A)).value);
        assertEquals("a=east", ConsoleControls.effective(app, GameStore.externalFilesDir(app), key).format());

        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertEquals(key, a.romEntryForTest().tid);
    }

    /**
     * An N64 game (no toolchain needed for its Home): the analog stick row (left / right / not used), the range and the
     * dead zone (« Usual », then values), the C buttons; written as RECOMP_N64_PAD_MAP. French screenshot.
     */
    @Test
    public void n64StickRangeDeadZone() throws Exception {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        rom("n64", "Star Race.z64", ConsoleTest.n64(0));
        LauncherActivity a = launch();
        openControls(a, "Star Race");
        assertNotNull(row(a, app.getString(R.string.CC_C_LEFT)));
        assertEquals(app.getString(R.string.SRC_RSTICK_LEFT), row(a, app.getString(R.string.CC_C_LEFT)).value);
        assertEquals(app.getString(R.string.SRC_L2) + " + " + app.getString(R.string.SRC_R2),
                     row(a, app.getString(R.string.CC_Z)).value);
        assertEquals(app.getString(R.string.STICK_LEFT), row(a, app.getString(R.string.CC_STICK)).value);
        assertEquals(app.getString(R.string.CC_RUNTIME_DEFAULT), row(a, app.getString(R.string.CC_RANGE)).value);
        focusRow(a, app.getString(R.string.CC_STICK));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("stick=right", cfgLine("RECOMP_N64_PAD_MAP"));
        focusRow(a, app.getString(R.string.CC_RANGE));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("64", row(a, app.getString(R.string.CC_RANGE)).value);
        focusRow(a, app.getString(R.string.CC_DEADZONE));
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);   // wraps: Usual -> 30 %
        assertEquals("stick=right,range=64,deadzone=30", cfgLine("RECOMP_N64_PAD_MAP"));
        assertEquals(app.getString(R.string.CC_DEADZONE_DETAIL), a.descriptionForTest());
        shot(a, "console-controls-3-n64");

        RuntimeEnvironment.setQualifiers("fr-w1920dp-h1080dp-land-mdpi");
        LauncherActivity b = launch();
        openControls(b, "Star Race");
        assertEquals("Votre stick droit", row(b, "Stick analogique").value);
        assertNotNull(row(b, "C gauche"));
        shot(b, "console-controls-4-n64-fr");
    }

    /**
     * XBA-181: a GBA game's Options (its Home's « Options » pill): its own Scale and Picture filter (« As in Settings »
     * showing Settings' value, then each value; scale_&lt;KEY&gt; / filter_&lt;KEY&gt;, which ShellActivity exports), and
     * its saves (save/gba-&lt;KEY&gt;/), deleted with A twice.
     */
    @Test
    public void gbaOptions() throws Exception {
        File rom = rom("gba", "Pocket Quest.gba", ConsoleTest.gba("AXVE"));
        RecompConfig cfg = RecompConfig.load(GameStore.externalFilesDir(app));
        cfg.set("RECOMP_SCALE", "stretch");
        cfg.save();
        LauncherActivity a = launch();
        String key = LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(rom)).tid;
        File save = new File(app.getFilesDir(), "save/" + key + "/game.sav");
        TestSupport.write(save, new byte[64]);
        openControls(a, "Pocket Quest", R.string.ROM_OPTIONS, LauncherActivity.Screen.ROM_OPTIONS);
        String scaleTitle = app.getString(R.string.OPT_CSCALE), filterTitle = app.getString(R.string.OPT_CFILTER);
        assertEquals(app.getString(R.string.ROM_OPT_FOLLOW, app.getString(R.string.VAL_CSCALE_STRETCH)),
                     row(a, scaleTitle).value);
        assertEquals(app.getString(R.string.ROM_OPT_FOLLOW, app.getString(R.string.VAL_CFILTER_CONSOLE)),
                     row(a, filterTitle).value);
        assertNotNull("its saves", row(a, app.getString(R.string.LIB_ST_SAVES)));
        assertNull("no Vulkan driver row: only the N64 renderer uses one", row(a, app.getString(R.string.OPT_VK_DRIVER)));
        shot(a, "console-options-1-gba");

        focusRow(a, scaleTitle);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("integer", ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.SCALE));
        assertEquals(app.getString(R.string.VAL_CSCALE_INTEGER), row(a, scaleTitle).value);
        assertEquals(app.getString(R.string.ROM_OPT_SAVED), a.noteForTest());
        focusRow(a, filterTitle);
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);
        assertEquals("linear", ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.FILTER));
        assertEquals("recomp.cfg untouched", "stretch", cfgLine("RECOMP_SCALE"));
        java.util.List<String[]> env = ConsoleVideo.runEnv(Console.GBA,
                                                           ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.SCALE),
                                                           ConsoleVideo.gameChoice(app, key, ConsoleVideo.Setting.FILTER),
                                                           RecompConfig.load(GameStore.externalFilesDir(app)));
        assertEquals("integer", env.get(0)[1]);
        assertEquals("linear", env.get(1)[1]);

        focusRow(a, app.getString(R.string.LIB_ST_SAVES));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertTrue("armed, not deleted yet", save.isFile());
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        for (int i = 0; i < 50 && save.exists(); i++)
            Thread.sleep(20);
        idle(300);
        assertFalse(save.exists());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
    }

    /**
     * An N64 game's Options: its own Vulkan driver (« As in Settings (<Settings' driver>) », System, each installed
     * driver; vk_driver_&lt;KEY&gt;, which ShellActivity resolves), and the warning of the driver it would start with.
     * (A GBA game has no such row: gbaOptions.)
     */
    @Test
    public void n64OptionsVulkanDriver() throws Exception {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        File n64 = rom("n64", "Star Race.z64", ConsoleTest.n64(0));
        VkDrivers vk = VkDrivers.of(app);
        VkDrivers.Driver d = vk.install(new java.io.ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())),
                                        VkDrivers.Origin.manual());
        java.nio.file.Files.write(new File(vk.root(), d.id + VkDrivers.STATUS_SUFFIX).toPath(),
                                  "unsupported\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        LauncherActivity a = launch();
        String key = LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(n64)).tid;
        assertEquals(Console.N64, Console.ofKey(key));
        openControls(a, "Star Race", R.string.ROM_OPTIONS, LauncherActivity.Screen.ROM_OPTIONS);
        String title = app.getString(R.string.OPT_VK_DRIVER);
        Widgets.MenuRow r = row(a, title);
        assertNotNull("N64: the Vulkan driver row", r);
        assertEquals(app.getString(R.string.ROM_OPT_FOLLOW, app.getString(R.string.VKD_SYSTEM_SHORT)), r.value);
        focusRow(a, title);
        assertEquals(app.getString(R.string.ROM_OPT_VK_DRIVER_DETAIL), a.descriptionForTest());
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(VkDrivers.SYSTEM, vk.gameChoice(key));
        assertEquals(app.getString(R.string.VKD_SYSTEM_SHORT), row(a, title).value);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(d.id, vk.gameChoice(key));
        assertEquals(VkDriverStore.label(d), row(a, title).value);
        assertEquals(app.getString(R.string.ROM_OPT_SAVED), a.noteForTest());
        assertTrue("the driver's « not compatible » warning",
                   a.descriptionForTest().contains(app.getString(R.string.VKD_LAST_UNSUPPORTED)));
        assertNull("Settings untouched", vk.activeId());
        assertEquals(d.id, vk.resolve(key).id);
        shot(a, "console-options-2-n64-vkdriver");
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertNull("back to Settings' choice", vk.gameChoice(key));
    }

    /** A PS1 disc: Cross / Circle / Square / Triangle, L1 .. R3, both DualShock sticks. */
    @Test
    public void ps1Buttons() throws Exception {
        File dir = tmp.newFolder("ps1img");
        File bin = new File(dir, "Space Race.bin");
        TestSupport.write(bin, ConsoleTest.ps1(true, "BOOT = cdrom:\\SLUS_005.94;1\n", "PLAYSTATION"));
        TestSupport.write(new File(dir, "Space Race.cue"),
                          "FILE \"Space Race.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
        rom("ps1", "unused.txt", new byte[1]);
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Aps1img",
                          dir.getAbsolutePath());
        LauncherActivity a = launch();
        openControls(a, "Space Race");
        for (int t = 0; t < ConsolePadMap.targets(Console.PS1).length; t++)
            assertNotNull("row " + t, row(a, app.getString(ConsoleControls.targetLabel(Console.PS1, t))));
        assertEquals(app.getString(R.string.SRC_SOUTH), row(a, app.getString(R.string.CC_CROSS)).value);
        assertEquals(app.getString(R.string.SRC_L2), row(a, app.getString(R.string.CC_L2)).value);
        assertEquals(app.getString(R.string.STICK_RIGHT), row(a, app.getString(R.string.XB_RSTICK)).value);
        focusRow(a, app.getString(R.string.CC_CROSS));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("cross=east", cfgLine("RECOMP_PS1_PAD_MAP"));
        assertFalse(a.romMapForTest().isDefault());
        shot(a, "console-controls-5-ps1");
    }
}
