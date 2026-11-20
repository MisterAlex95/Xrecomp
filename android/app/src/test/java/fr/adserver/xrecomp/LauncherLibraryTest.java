package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Intent;
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
import java.util.Collections;
import java.util.List;
import java.util.function.Consumer;

/**
 * The game library in the launcher (docs/android/app-shell.md « Game library »): the routing (today's flow without a
 * library folder), the grid from a library folder (0 / 1 / 7 / 20 games, en / fr, 720p / 1080p, rendered to
 * build/screenshots/library-*.png), the controller navigation, the theme (stock on the library, the game's on its
 * Home), the launched game written only at launch, the caches switched then, a game that is not imported, the queue.
 *
 * Synthetic only: XBEs from TestSupport.xbe, game folders and empty « images » made here. The scan runs for real over
 * the temporary folder (game folders need no native code; an image is listed from a prepared index.json whose
 * fingerprint matches, so it is not opened).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherLibraryTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File root;
    private static final String[] LOCALES = { "en", "fr" };
    private static final String[][] SIZES = { { "720", "w1280dp-h720dp-land-mdpi" },
                                              { "1080", "w1920dp-h1080dp-land-mdpi" },
                                              { "4x3", "w1440dp-h1080dp-land-mdpi" },
                                              { "1x1", "w1080dp-h1080dp-land-mdpi" } };
    private static final String[] NAMES = { "Sample Racer", "Sample Quest", "Tiny Tanks", "Night Drive",
                                            "Puzzle Garden", "Space Mail", "Ocean Trip", "Rally Cross",
                                            "Castle Builder", "Robot Soccer", "Snow Board", "Desert Run",
                                            "Moon Base", "Jungle Jump", "River Raft", "Sky Pilot", "Cave Diver",
                                            "Ice Hockey", "Street Kart", "A Game With A Rather Long Title Indeed" };

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        // All files access (Environment.isExternalStorageManager: the app op's default mode, then the permission).
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        root = tmp.newFolder("xbox");
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
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    /**
     * A key press as the window delivers it. Activity.dispatchKeyEvent alone never moves the focus: on a device the
     * D-pad navigation is ViewRootImpl's, for a direction key nobody consumed. So does it here: focusSearch from the
     * launcher's focused view, then requestFocus, as ViewRootImpl.performFocusNavigation does.
     */
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

    private static int tid(int i) {
        return 0x58420001 + i;   // « XB » + a number: ours
    }

    private static String tidHex(int i) {
        return String.format("%08X", tid(i));
    }

    /** A game folder in the library folder: <name>/default.xbe. */
    private File folderGame(int i) throws Exception {
        File d = new File(root, NAMES[i]);
        TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(tid(i), NAMES[i], XbeInfo.REGION_WORLD, 0,
                                                                         0x100));
        return d;
    }

    /** The library folder, read as files (All files access). */
    private void useFolder() {
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
    }

    /**
     * An image the scan does not open: an empty file and an index.json item with its fingerprint, its XBE in the
     * cache (as a scan would have left them).
     */
    private void imageGame(int i) throws Exception {
        File iso = new File(root, NAMES[i] + ".iso");
        TestSupport.write(iso, new byte[64]);
        LibraryIndex x = LibraryIndex.read(Library.indexFile(app.getFilesDir()));
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = LibraryIndex.fileLoc(iso);
        it.kind = LibraryIndex.KIND_ISO;
        it.name = iso.getName();
        it.size = iso.length();
        it.mtime = iso.lastModified();
        it.tid = tidHex(i);
        it.title = NAMES[i];
        it.region = XbeInfo.REGION_NA | XbeInfo.REGION_WORLD;
        it.version = 0x102;
        it.xbeSha256 = "00";
        it.xbe = "xbe/" + it.tid + "-00000000/default.xbe";
        TestSupport.write(it.xbeFile(app.getFilesDir()), TestSupport.xbe(tid(i), NAMES[i]));
        x.root = LibraryIndex.fileLoc(root);
        x.items.add(it);
        LibraryIndex.write(Library.indexFile(app.getFilesDir()), x);
    }

    /** A file that is not an Xbox image: the « Other files » line. */
    private void otherFile(String name) throws Exception {
        File f = new File(root, name);
        TestSupport.write(f, new byte[16]);
        LibraryIndex x = LibraryIndex.read(Library.indexFile(app.getFilesDir()));
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = LibraryIndex.fileLoc(f);
        it.name = name;
        it.size = f.length();
        it.mtime = f.lastModified();
        it.status = LibraryIndex.NOT_XISO;
        x.items.add(it);
        x.identify = LibraryScanner.identifyStamp(app);   // as this build scanned it: not opened again (XBA-192)
        LibraryIndex.write(Library.indexFile(app.getFilesDir()), x);
    }

    private LauncherActivity launch() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        waitScan(a);
        return a;
    }

    private static void waitScan(LauncherActivity a) throws Exception {
        Thread t = a.scannerForTest();
        if (t != null)
            t.join(20_000);
        idle(400);
    }

    private static List<GameCard> cards(View v, List<GameCard> out) {
        if (v instanceof GameCard)
            out.add((GameCard)v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                cards(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    private static GameCard card(LauncherActivity a, String title) {
        for (GameCard c : a.libraryCardsForTest()) {
            if (title.equals(c.titleForTest()))
                return c;
        }
        return null;
    }

    private void render(String name, Consumer<LauncherActivity> act) throws Exception {
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                LauncherActivity a = launch();
                if (act != null)
                    act.accept(a);
                if (a.focusForTest() == null)
                    a.forceFocusForTest();
                idle(400);
                File f = TestSupport.screenshot(a.getWindow().getDecorView(), "library-" + name + "-" + loc + "-"
                                                                             + size[0]);
                assertTrue(f.length() > 0);
            }
        }
    }

    // ---- Routing ----------------------------------------------------------------------------------------------

    /** One game, no library folder: today's Home, and the game became the library's first record. */
    @Test
    public void oneGameWithoutFolderKeepsTodaysFlow() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        Library.Games g = Library.read(Library.gamesFile(app.getFilesDir()));
        assertEquals(1, g.games.size());
        Library.Record r = g.get("4D530004");
        assertNotNull(r);
        assertEquals(Library.KIND_IMPORTED, r.dataKind);
        assertEquals("4D530004", GameStore.launchedTitleId(app));
        assertEquals("4D530004", SaveCaches.owner(Library.cacheOwnerFile(app.getFilesDir())));
        assertNull("nothing remembered by opening the launcher", GameStore.chosenDir(app));
        // B quits (no library).
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertTrue(a.isFinishing());
    }

    /** No game and no folder: Welcome, as before; nothing written. */
    @Test
    public void noGameWelcome() throws Exception {
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.WELCOME, a.screenForTest());
        assertFalse(Library.gamesFile(app.getFilesDir()).exists());
    }

    // ---- The grid ---------------------------------------------------------------------------------------------

    @Test
    public void gridFromTheFolder() throws Exception {
        for (int i = 0; i < 7; i++) {
            if (i == 2 || i == 5)
                imageGame(i);
            else
                folderGame(i);
        }
        otherFile("notes.iso");
        useFolder();
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        List<GameCard> cs = a.libraryCardsForTest();
        assertEquals("seven games, no folder tile (the folder is in Options)", 7, cs.size());
        for (GameCard c : cs)
            assertFalse(app.getString(R.string.LIB_FOLDER_TILE).equals(c.titleForTest()));
        assertEquals(app.getString(R.string.LIB_CHIP_NOT_IMPORTED), card(a, NAMES[2]).chipForTest());
        assertTrue("a state worth saying shows", card(a, NAMES[2]).chipShownForTest());
        assertEquals(app.getString(R.string.STATE_READY), card(a, NAMES[0]).chipForTest());
        assertFalse("« Ready » is not shown", card(a, NAMES[0]).chipShownForTest());
        assertEquals("not imported is neutral", Theme.TEXT_DIM,
                     LibraryScreen.chipColor(GameState.Chip.NOT_IMPORTED));
        List<String> hints = new ArrayList<>();
        for (Widgets.Hint h : a.hintsForTest())
            hints.add(h.glyphForTest());
        assertEquals(java.util.Arrays.asList("A", "X", "Y"), hints);   // XBA-206: X Favourite; Start not hinted
        assertTrue("library: stock colours", Theme.isStock());
        assertTrue(a.companionForTest().libraryForTest());
        render("7", null);
    }

    /**
     * XBA-242: Xbox cards show the XBE's title image as their cover ($$XTIMAGE, else $$XSIMAGE), decoded at the scan
     * and cached; a game with neither keeps its monogram.
     */
    @Test
    public void gridShowsTitleImages() throws Exception {
        byte[] title = TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture());
        int[] px = new int[64 * 64];
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                px[y * 64 + x] = 0xFF000000 | (x * 4) << 16 | (y * 2) << 8 | 0xC0;
        byte[] saveImage = TestSupport.xprArgb(64, 64, px);
        TestSupport.write(new File(new File(root, NAMES[0]), "default.xbe"),
                          TestSupport.xbeWithImage(tid(0), NAMES[0], title));
        TestSupport.write(new File(new File(root, NAMES[1]), "default.xbe"),
                          TestSupport.xbeWithSection(tid(1), NAMES[1], GameArt.SAVE_SECTION, saveImage));
        folderGame(2);
        useFolder();
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertTrue("title image", card(a, NAMES[0]).hasArtForTest());
        assertTrue("save image when there is no title image", card(a, NAMES[1]).hasArtForTest());
        assertFalse("neither: the monogram", card(a, NAMES[2]).hasArtForTest());
        assertTrue(TitleArt.cacheFile(app.getFilesDir(), tidHex(0)).isFile());
        assertTrue(TitleArt.cacheFile(app.getFilesDir(), tidHex(1)).isFile());
        render("title-art", null);
    }

    @Test
    public void gridSizes() throws Exception {
        useFolder();
        render("0", null);
        folderGame(0);
        render("1", null);
        for (int i = 1; i < 20; i++)
            folderGame(i);
        render("20", a -> {
            a.forceFocusForTest();
            for (int i = 0; i < 9; i++)
                key(a, KeyEvent.KEYCODE_DPAD_DOWN);   // the grid scrolls to the focused card
        });
        LauncherActivity a = launch();
        assertEquals(20, a.libraryCardsForTest().size());
    }

    /** An empty library: why, and a « Library folder » button; no card. */
    @Test
    public void emptyLibraryOffersTheFolder() throws Exception {
        useFolder();
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertTrue(a.libraryCardsForTest().isEmpty());
        a.forceFocusForTest();
        idle(300);
        assertTrue(a.focusForTest() instanceof Widgets.PillButton);
        assertEquals(app.getString(R.string.LIB_FOLDER_TILE), ((Widgets.PillButton)a.focusForTest()).title);
    }

    /** Y: the Options sheet (Rescan, Storage, Library folders, Settings, About); B closes it, the focus comes back. */
    @Test
    public void optionsSheet() throws Exception {
        for (int i = 0; i < 3; i++)
            folderGame(i);
        useFolder();
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals("X no longer opens Storage (XBA-206: a favourite)", LauncherActivity.Screen.LIBRARY,
                     a.screenForTest());
        GameCard focused = (GameCard)a.focusForTest();   // (the cards are made again, the favourite first)
        assertTrue(focused.favouriteForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        Widgets.Sheet sheet = a.sheetForTest();
        assertNotNull(sheet);
        List<String> rows = new ArrayList<>();
        for (Widgets.MenuRow r : sheet.rowsForTest())
            rows.add(r.title);
        assertEquals(java.util.Arrays.asList(app.getString(R.string.LIB_RESCAN), app.getString(R.string.LIB_STORAGE),
                                             app.getString(R.string.LIB_FOLDERS), app.getString(R.string.NET_SOURCES),
                                             app.getString(R.string.SETTINGS),
                                             app.getString(R.string.ABOUT)), rows);
        assertEquals(root.getName(), sheet.rowsForTest().get(2).value);
        assertTrue(a.focusForTest() == sheet.rowsForTest().get(0));
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);   // the focus stays in the sheet
        assertTrue(a.focusForTest() instanceof Widgets.MenuRow);
        key(a, KeyEvent.KEYCODE_BUTTON_START);   // nothing behind the sheet
        assertNull(shadowOf(a).getNextStartedActivity());
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-options");
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertNull(a.sheetForTest());
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertTrue("the focus comes back to the card", a.focusForTest() == focused);

        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        a.sheetForTest().rowsForTest().get(1).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.STORAGE, a.screenForTest());
        assertNull(a.sheetForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
    }

    // ---- Navigation and theme --------------------------------------------------------------------------------

    @Test
    public void navigationHomeAndBack() throws Exception {
        // Two full rows of the five-column grid: with a short last row, D-pad down from the second column lands on
        // the first card below and up then comes back to the first column, not to the card it left.
        for (int i = 0; i < 10; i++)
            folderGame(i);
        useFolder();
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        assertTrue(a.focusForTest() instanceof GameCard);
        GameCard first = (GameCard)a.focusForTest();
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        GameCard second = (GameCard)a.focusForTest();
        assertTrue("D-pad right: the next card", first != second);
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        assertTrue("D-pad down: the row below", a.focusForTest() instanceof GameCard);
        key(a, KeyEvent.KEYCODE_DPAD_UP);
        assertEquals(second, a.focusForTest());
        String title = second.titleForTest();

        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertFalse("the game's Home: its colours", Theme.isStock());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertNull("opening a game's Home does not make it the launched game", GameStore.chosenDir(app));
        Library.Games g = Library.read(Library.gamesFile(app.getFilesDir()));
        assertEquals(title, g.selected().title);

        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertTrue("back to the library: stock colours", Theme.isStock());
        idle(300);
        assertEquals("the focus comes back to the game", title, ((GameCard)a.focusForTest()).titleForTest());
    }

    /** The launched game changes only at Play, and the shared caches move aside then. */
    @Test
    public void launchedGameWrittenOnlyAtLaunch() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        File save = GameTheme.saveDir(app);
        TestSupport.write(new File(save, "Cache/unpacked.bin"), "first game's cache");
        File b = folderGame(1);
        useFolder();
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        GameCard c = card(a, NAMES[1]);
        assertNotNull(c);
        c.performClick();
        idle(300);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals("4D530004", GameStore.launchedTitleId(app));
        assertNull(GameStore.chosenDir(app));

        key(a, KeyEvent.KEYCODE_BUTTON_START);
        Intent started = shadowOf(a).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShellActivity.class.getName(), started.getComponent().getClassName());
        assertEquals(b.getAbsolutePath(), GameStore.chosenDir(app));
        assertEquals(tidHex(1), GameStore.launchedTitleId(app));
        assertEquals(tidHex(1), SaveCaches.owner(Library.cacheOwnerFile(app.getFilesDir())));
        assertTrue("the first game's cache stashed",
                   new File(SaveCaches.stashDir(save, "4D530004"), "Cache/unpacked.bin").isFile());
        assertFalse(new File(save, "Cache").exists());
        Library.Games g = Library.read(Library.gamesFile(app.getFilesDir()));
        assertTrue(g.get(tidHex(1)).lastPlayed > 0);
        assertEquals(tidHex(1), GameStore.sessionTid(app));
    }

    // ---- A game that is not imported --------------------------------------------------------------------------

    @Test
    public void notImportedHomeAndPrepareNow() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.newFolder("native")).getAbsolutePath();
        imageGame(3);
        useFolder();
        LauncherActivity a = launch();
        GameCard c = card(a, NAMES[3]);
        assertNotNull(c);
        c.performClick();
        idle(300);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(app.getString(R.string.LIB_IMPORT), a.homePrimaryForTest().title);
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-home-not-imported");

        // « Prepare now »: the cached default.xbe is the Prepare's source.
        Widgets.Selectable now = null;
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.PillButton
                && app.getString(R.string.LIB_PREPARE_NOW).equals(((Widgets.PillButton)v).title))
                now = (Widgets.Selectable)v;
        }
        assertNotNull("Prepare now", now);
        now.performClick();
        idle(300);
        Intent s = shadowOf(app).getNextStartedService();
        assertNotNull(s);
        File xbeDir = new File(Library.xbeCacheDir(app.getFilesDir()), tidHex(3) + "-00000000");
        assertEquals(xbeDir.getAbsolutePath(), s.getStringExtra(PrepareService.EXTRA_GAME_DIR));
        assertEquals(tidHex(3), s.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
    }

    @Test
    public void queuedShowsItsPlace() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.newFolder("native")).getAbsolutePath();
        folderGame(0);
        folderGame(1);
        useFolder();
        PrepareProtocol.Queue q = new PrepareProtocol.Queue();
        q.add(new PrepareProtocol.Job("4D530004", "/x", "Other"));
        q.add(new PrepareProtocol.Job(tidHex(1), root.getAbsolutePath(), NAMES[1]));
        PrepareProtocol.writeQueue(PrepareProtocol.queueFile(app.getFilesDir()), q);
        PrepareProtocol.Status st = new PrepareProtocol.Status();
        st.state = PrepareProtocol.QUEUED;
        st.titleId = tidHex(1);
        st.startedAt = st.updatedAt = System.currentTimeMillis();
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, tidHex(1)), st);
        ActivityManager.RunningAppProcessInfo p = new ActivityManager.RunningAppProcessInfo();
        p.processName = app.getPackageName() + ":prepare";
        p.pid = 4242;
        shadowOf(app.getSystemService(ActivityManager.class)).setProcesses(Collections.singletonList(p));
        LauncherActivity a = launch();
        assertEquals(app.getString(R.string.LIB_QUEUED_N, 2), card(a, NAMES[1]).chipForTest());
        assertEquals(app.getString(R.string.CHIP_NEEDS_PREPARE), card(a, NAMES[0]).chipForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-queued");
    }

    /** The Source screen offers the library folder (a row of its list). */
    @Test
    public void sourceOffersTheLibraryFolder() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SOURCE);
        idle(300);
        boolean found = false;
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow && app.getString(R.string.LIB_CARD).equals(((Widgets.MenuRow)v).title))
                found = true;
        }
        assertTrue(found);
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-source-three-cards");
    }

    private static Widgets.MenuRow menuRow(LauncherActivity a, String title) {
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow && title.equals(((Widgets.MenuRow)v).title))
                return (Widgets.MenuRow)v;
        }
        return null;
    }

    /** Storage: per game, each thing deleted on its own with A twice; saves stay unless deleted on their own. */
    @Test
    public void storageDeletesOneThingAtATime() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        TestSupport.writePack(app.getFilesDir(), "4D530004", 4096);
        File saves = new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin");
        TestSupport.write(saves, new byte[64]);
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        for (int i = 0; i < 50 && menuRow(a, app.getString(R.string.LIB_ST_PACK)).value
                                       .equals(app.getString(R.string.SIZE_COUNTING)); i++) {
            Thread.sleep(20);
            idle(50);
        }
        assertNotNull(menuRow(a, app.getString(R.string.LIB_ST_DATA)));
        assertNotNull(menuRow(a, app.getString(R.string.LIB_ST_SAVES)));
        assertFalse("the size is the row's value", menuRow(a, app.getString(R.string.LIB_ST_PACK)).value
                                                       .equals(app.getString(R.string.SIZE_COUNTING)));
        a.forceFocusForTest();
        idle(300);
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-storage");

        File pack = Pack.packDir(app.getFilesDir(), "4D530004");
        menuRow(a, app.getString(R.string.LIB_ST_PACK)).performClick();
        idle(100);
        assertTrue("the first press only arms", pack.isDirectory());
        menuRow(a, app.getString(R.string.LIB_ST_PACK)).performClick();
        for (int i = 0; i < 100 && pack.exists(); i++) {
            Thread.sleep(20);
            idle(50);
        }
        assertFalse(pack.exists());
        assertTrue("saves kept", saves.isFile());
        assertTrue("the game kept", new File(GameStore.defaultGameDir(app), "default.xbe").isFile());
    }

    /**
     * Storage › Export all saves: Android's « create document » picker, then a zip of the saves only; Import saves
     * puts them back (the current ones kept aside) and says how many games got theirs.
     */
    @Test
    public void storageExportsAndImportsSaves() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        File saves = new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin");
        TestSupport.write(saves, "level 3");
        TestSupport.write(new File(GameTheme.saveDir(app), "Cache/4d530004/big.bin"), new byte[64]);
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        Widgets.MenuRow ex = menuRow(a, app.getString(R.string.SAVES_EXPORT_ALL));
        assertNotNull(ex);
        assertEquals(app.getResources().getQuantityString(R.plurals.SAVES_GAMES, 1, 1), ex.value);
        assertNotNull(menuRow(a, app.getString(R.string.SAVES_IMPORT)));
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-storage-saves");

        ex.performClick();
        idle(100);
        org.robolectric.shadows.ShadowActivity.IntentForResult r = shadowOf(a).getNextStartedActivityForResult();
        assertEquals(Intent.ACTION_CREATE_DOCUMENT, r.intent.getAction());
        assertEquals("application/zip", r.intent.getType());
        assertTrue(r.intent.getStringExtra(Intent.EXTRA_TITLE).startsWith("xrecomp-saves-all-"));
        android.net.Uri out = android.net.Uri.parse("content://docs.test/document/saves.zip");
        java.io.ByteArrayOutputStream zip = new java.io.ByteArrayOutputStream();
        shadowOf(app.getContentResolver()).registerOutputStream(out, zip);
        shadowOf(a).receiveResult(r.intent, android.app.Activity.RESULT_OK, new Intent().setData(out));
        waitWorker(a);
        assertEquals(app.getResources().getQuantityString(R.plurals.SAVES_EXPORTED, 1, 1, "saves.zip"),
                     a.noteForTest());
        List<String> names = new ArrayList<>();
        try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(
                 new java.io.ByteArrayInputStream(zip.toByteArray()))) {
            for (java.util.zip.ZipEntry e; (e = z.getNextEntry()) != null;)
                names.add(e.getName());
        }
        assertEquals(java.util.Arrays.asList(SaveArchive.MANIFEST, "UDATA/4d530004/save.bin"), names);

        TestSupport.write(saves, "level 1");
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        menuRow(a, app.getString(R.string.SAVES_IMPORT)).performClick();
        idle(100);
        r = shadowOf(a).getNextStartedActivityForResult();
        assertEquals(Intent.ACTION_OPEN_DOCUMENT, r.intent.getAction());
        android.net.Uri in = android.net.Uri.parse("content://docs.test/document/in.zip");
        shadowOf(app.getContentResolver()).registerInputStream(in, new java.io.ByteArrayInputStream(zip.toByteArray()));
        shadowOf(a).receiveResult(r.intent, android.app.Activity.RESULT_OK, new Intent().setData(in));
        waitWorker(a);
        assertEquals(app.getResources().getQuantityString(R.plurals.SAVES_IMPORTED, 1, 1), a.noteForTest());
        assertEquals("level 3", new String(java.nio.file.Files.readAllBytes(saves.toPath()), "UTF-8"));
        assertEquals("level 1", new String(java.nio.file.Files.readAllBytes(
            new File(GameTheme.saveDir(app), ".backup/4D530004/UDATA/4d530004/save.bin").toPath()), "UTF-8"));
        assertEquals(LauncherActivity.Screen.STORAGE, a.screenForTest());
    }

    /**
     * AND-5: the scan thread outlives its activity. A launcher that goes for good (Play finishes it) cancels its
     * scan; one the system destroys to make it again lets it finish.
     */
    @Test
    public void aFinishedLauncherCancelsItsScan() throws Exception {
        org.robolectric.android.controller.ActivityController<LauncherActivity> c =
            Robolectric.buildActivity(LauncherActivity.class).setup();
        idle(300);
        LauncherActivity a = c.get();
        waitScan(a);
        assertFalse(a.scanCancelledForTest());
        c.pause().stop().destroy();                  // not finishing: a recreation
        assertFalse(a.scanCancelledForTest());

        org.robolectric.android.controller.ActivityController<LauncherActivity> d =
            Robolectric.buildActivity(LauncherActivity.class).setup();
        idle(300);
        LauncherActivity b = d.get();
        waitScan(b);
        b.finish();
        d.pause().stop().destroy();
        assertTrue("the scan of a finished launcher is cancelled", b.scanCancelledForTest());
    }

    /**
     * Export saves, and the system ends the app's process while the picker is in front: the activity made again
     * from its saved state still knows which games to export, and writes them to the picked file.
     */
    @Test
    public void exportSurvivesTheProcessEndingBehindThePicker() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        TestSupport.write(new File(GameTheme.saveDir(app), "UDATA/4d530004/save.bin"), "level 3");
        org.robolectric.android.controller.ActivityController<LauncherActivity> c =
            Robolectric.buildActivity(LauncherActivity.class).setup();
        idle(300);
        LauncherActivity a = c.get();
        waitScan(a);
        a.goForTest(LauncherActivity.Screen.STORAGE);
        idle(300);
        menuRow(a, app.getString(R.string.SAVES_EXPORT_ALL)).performClick();
        idle(100);
        assertEquals(Intent.ACTION_CREATE_DOCUMENT, shadowOf(a).getNextStartedActivityForResult().intent.getAction());
        android.os.Bundle state = new android.os.Bundle();
        c.pause().saveInstanceState(state).stop();
        assertEquals(Collections.singletonList("4D530004"), state.getStringArrayList(LauncherActivity.STATE_EXPORT_KEYS));

        LauncherActivity again = Robolectric.buildActivity(LauncherActivity.class).setup(state).get();
        idle(300);
        waitScan(again);
        android.net.Uri out = android.net.Uri.parse("content://docs.test/document/saves.zip");
        java.io.ByteArrayOutputStream zip = new java.io.ByteArrayOutputStream();
        shadowOf(app.getContentResolver()).registerOutputStream(out, zip);
        shadowOf(again).callOnActivityResult(LauncherActivity.REQ_SAVES_EXPORT, android.app.Activity.RESULT_OK,
                                             new Intent().setData(out));
        waitWorker(again);
        assertEquals(app.getResources().getQuantityString(R.plurals.SAVES_EXPORTED, 1, 1, "saves.zip"),
                     again.noteForTest());
        List<String> names = new ArrayList<>();
        try (java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(
                 new java.io.ByteArrayInputStream(zip.toByteArray()))) {
            for (java.util.zip.ZipEntry e; (e = z.getNextEntry()) != null;)
                names.add(e.getName());
        }
        assertEquals(java.util.Arrays.asList(SaveArchive.MANIFEST, "UDATA/4d530004/save.bin"), names);
    }

    /** An export result whose games are not known (no saved state): said, not an empty file left as the export. */
    @Test
    public void exportWithoutItsGamesIsSaid() throws Exception {
        LauncherActivity a = launch();
        android.net.Uri out = android.net.Uri.parse("content://docs.test/document/saves.zip");
        java.io.ByteArrayOutputStream zip = new java.io.ByteArrayOutputStream();
        shadowOf(app.getContentResolver()).registerOutputStream(out, zip);
        shadowOf(a).callOnActivityResult(LauncherActivity.REQ_SAVES_EXPORT, android.app.Activity.RESULT_OK,
                                         new Intent().setData(out));
        waitWorker(a);
        assertEquals(app.getString(R.string.SAVES_EXPORT_LOST), a.noteForTest());
        assertFalse(a.workerBusyForTest());
        assertEquals(0, zip.size());
    }

    private static void waitWorker(LauncherActivity a) throws Exception {
        for (int i = 0; i < 200 && a.workerBusyForTest(); i++) {
            Thread.sleep(20);
            idle(50);
        }
        idle(100);
    }

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    // ---- Several library folders (XBA-178) ---------------------------------------------------------------------

    /**
     * Two folders, an Xbox one and one of other consoles (read as files): every game in one grid with its console
     * badge, the GBA and N64 ones Ready (their Home and launch: n64HomePlays, gbaCardLaunches);
     * Options › Library folders lists both, « Add a folder » last, and one is taken out of the library (its games
     * leave the grid, nothing in it is deleted).
     */
    @Test
    public void severalFoldersEveryConsole() throws Exception {
        folderGame(0);
        folderGame(1);
        File gba = tmp.newFolder("gba");
        File rom = new File(gba, "Pocket Quest.gba");
        TestSupport.write(rom, ConsoleTest.gba("AXVE"));
        File race = new File(gba, "Star Race.z64");
        TestSupport.write(race, ConsoleTest.n64(0));
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Agba",
                          gba.getAbsolutePath());
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(4, a.libraryCardsForTest().size());
        GameCard quest = card(a, "Pocket Quest"), star = card(a, "Star Race");
        assertNotNull(quest);
        assertNotNull(star);
        assertEquals("GBA", quest.badgeForTest());
        assertEquals("N64", star.badgeForTest());
        assertEquals("Xbox", card(a, NAMES[0]).badgeForTest());
        assertFalse("Ready: no chip", quest.chipShownForTest());
        assertFalse("Ready: no chip (XBA-174)", star.chipShownForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-consoles");

        // The N64 card's Home and launch: n64HomePlays (XBA-174).
        a.forceFocusForTest();
        idle(300);

        // Options › Library folders: both folders, then « Add a folder ».
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        Widgets.Sheet sheet = a.sheetForTest();
        assertEquals(app.getString(R.string.LIB_FOLDERS_N, 2), sheet.rowsForTest().get(2).value);
        sheet.rowsForTest().get(2).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        sheet = a.sheetForTest();
        assertNotNull(sheet);
        List<String> rows = new ArrayList<>();
        for (Widgets.MenuRow r : sheet.rowsForTest())
            rows.add(r.title);
        assertEquals(java.util.Arrays.asList("xbox", "gba", app.getString(R.string.LIB_FOLDER_ADD)), rows);
        assertEquals(gba.getParentFile().getName(), sheet.rowsForTest().get(1).value);   // (a device: « Internal storage »)
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-folders");

        // A on « gba »: Keep (first) or Remove; Keep goes back to the list.
        sheet.rowsForTest().get(1).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        sheet = a.sheetForTest();
        assertEquals(app.getString(R.string.LIB_FOLDER_KEEP), sheet.rowsForTest().get(0).title);
        assertTrue(a.focusForTest() == sheet.rowsForTest().get(0));
        sheet.rowsForTest().get(1).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertNull(a.sheetForTest());
        assertEquals(1, Library.folders(app).size());
        assertEquals(2, a.libraryCardsForTest().size());
        assertNull(card(a, "Pocket Quest"));
        assertNull(card(a, "Star Race"));
        assertEquals("", card(a, NAMES[0]).badgeForTest());   // an Xbox-only library looks as before
        assertTrue("nothing in the folder is deleted", rom.isFile());
        assertEquals(app.getString(R.string.LIB_FOLDER_REMOVED, "gba"), a.noteForTest());
        assertNull(LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(rom)));
    }

    /**
     * XBA-157: A on a GBA card starts the game activity at once with its ROM (no Home, Prepare, import or pack): the
     * ROM becomes the launched game (GameStore.launchedRom, the library key as title_id), the Xbox game's folder stays
     * the remembered one, and the launcher finishes. Start does the same; an Xbox Play afterwards is another game
     * (the ROM is forgotten).
     */
    @Test
    public void gbaCardLaunches() throws Exception {
        folderGame(0);
        File gba = tmp.newFolder("gba");
        File rom = new File(gba, "Pocket Quest.gba");
        TestSupport.write(rom, ConsoleTest.gba("AXVE"));
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Agba",
                          gba.getAbsolutePath());
        LauncherActivity a = launch();
        GameCard quest = card(a, "Pocket Quest");
        assertNotNull(quest);
        String key = LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(rom)).tid;
        assertTrue(key, key.startsWith("gba-AXVE-"));
        String dirBefore = GameStore.chosenDir(app);

        a.forceFocusForTest();
        idle(300);
        quest.requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent started = shadowOf(a).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShellActivity.class.getName(), started.getComponent().getClassName());
        assertTrue((started.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0);
        assertTrue(a.isFinishing());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals("Pocket Quest", GameStore.launchedRomTitle(app));
        assertEquals(key, GameStore.launchedTitleId(app));
        assertEquals("the Xbox game's folder is kept", dirBefore, GameStore.chosenDir(app));
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());   // no Home for a GBA game
        Library.Games g = Library.read(Library.gamesFile(app.getFilesDir()));
        Library.Record gr = g.get(key);   // XBA-206: a record for its play time, never the selected game
        assertNotNull(gr);
        assertTrue(gr.lastPlayed > 0);
        assertFalse(key.equals(g.selected));
        assertFalse(g.selected() != null && key.equals(g.selected().titleId));
        assertEquals(key, GameStore.sessionTid(app));

        // Start on the card (a fresh launcher): the same launch.
        GameStore.rememberLaunch(app, dirBefore, "", null);   // an Xbox launch clears the ROM
        assertNull(GameStore.launchedRom(app));
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Pocket Quest").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_START);
        started = shadowOf(b).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShellActivity.class.getName(), started.getComponent().getClassName());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
    }

    /** A PS1 disc as a .cue and its one track, in a library folder read as files (ConsoleTest.ps1: SLUS-00594). */
    private File ps1Disc(String folder) throws Exception {
        File dir = tmp.newFolder(folder);
        TestSupport.write(new File(dir, "Space Race.bin"),
                          ConsoleTest.ps1(true, "BOOT = cdrom:\\SLUS_005.94;1\n", "PLAYSTATION"));
        File cue = new File(dir, "Space Race.cue");
        TestSupport.write(cue, "FILE \"Space Race.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3A" + folder,
                          dir.getAbsolutePath());
        return cue;
    }

    /**
     * XBA-168: a PS1 card plays like a GBA one: Ready (no chip, the PS1 badge), A or Start starts the game activity at
     * once with the .cue (its tracks found beside it by the runtime: no track documents to hand over for a folder
     * read as files), the library key ps1-<serial> as title_id.
     */
    @Test
    public void ps1CardLaunches() throws Exception {
        folderGame(0);
        File cue = ps1Disc("ps1");
        LauncherActivity a = launch();
        GameCard race = card(a, "Space Race");
        assertNotNull(race);
        assertEquals("PS1", race.badgeForTest());
        assertFalse("Ready: no chip", race.chipShownForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "ps1-library");
        a.forceFocusForTest();
        idle(300);
        race.requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent started = shadowOf(a).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShellActivity.class.getName(), started.getComponent().getClassName());
        assertEquals(cue.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals("ps1-SLUS-00594", GameStore.launchedTitleId(app));
        assertTrue(GameStore.launchedRomParts(app).isEmpty());
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());

        GameStore.rememberLaunch(app, GameStore.chosenDir(app), "", null);
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Space Race").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_START);
        assertNotNull(shadowOf(b).getNextStartedActivity());
        assertEquals(cue.getAbsolutePath(), GameStore.launchedRom(app));
    }

    /**
     * XBA-168: with the toolchain, A on a PS1 card opens its Home like a GBA game's: Play first (the interpreter, no
     * pack needed), « Not prepared » and Prepare beside it. Prepare starts PrepareService with the .cue and the library
     * key ps1-<serial> and shows the Preparing screen; a valid pack says « Prepared »; code the runs found since (the
     * runtime's records: address, hash, bytes) offers Prepare again; Play launches the disc in every state.
     */
    @Test
    public void ps1HomePrepares() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        folderGame(0);
        File cue = ps1Disc("ps1");
        String key = "ps1-SLUS-00594";
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        card(a, "Space Race").requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertNull("not launched", shadowOf(a).getNextStartedActivity());
        assertEquals(key, a.romEntryForTest().tid);
        assertEquals(app.getString(R.string.ROM_NOT_PREPARED_NOTE), a.noteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "ps1-1-home-not-prepared");

        // Prepare (right of Play): the service with the .cue and the library key, then the Preparing screen.
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertEquals(key, started.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(cue.getAbsolutePath(), started.getStringExtra(PrepareService.EXTRA_GAME_DIR));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertEquals(PrepareProtocol.RUNNING, PrepareProtocol.readStatus(PrepareService.statusFile(app, key)).state);

        // Done: a valid pack (console ps1). Its Home says Prepared, Play first.
        File dir = Pack.packDir(app.getFilesDir(), key);
        TestSupport.write(new File(dir, "libtitle.so"), new byte[16]);
        org.json.JSONObject o = new org.json.JSONObject().put("format", 1).put("console", "ps1")
                .put("title_key", "SLUS-00594").put("abi", "arm64-v8a").put("shell_abi", Pack.SHELL_ABI)
                .put("lib", "libtitle.so").put("lib_size", 16).put("source_sha256", "ab").put("discovered", 1);
        TestSupport.write(new File(dir, "pack.json"), o.toString());
        PrepareProtocol.Status st = PrepareProtocol.readStatus(PrepareService.statusFile(app, key));
        st.state = PrepareProtocol.DONE;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, key), st);
        TestSupport.write(Pack.discoveredFile(app.getFilesDir(), key),
                          "80100000 00000000000000AA 0000E0AF\n");   // what the pack was built with
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Space Race").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, b.screenForTest());
        assertEquals(app.getString(R.string.ROM_PREPARED_NOTE), b.noteForTest());
        TestSupport.screenshot(b.getWindow().getDecorView(), "ps1-2-home-prepared");

        // The runs found code since (two more records, an overlay at the same address included): Prepare again.
        TestSupport.write(Pack.discoveredFile(app.getFilesDir(), key),
                          "80100000 00000000000000AA 0000E0AF\n80100000 00000000000000BB 1111E0AF\n"
                          + "80020000 0000000000000001 00000000\n");
        LauncherActivity c = launch();
        c.forceFocusForTest();
        idle(300);
        card(c, "Space Race").requestFocusFromTouch();
        idle(300);
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(app.getString(R.string.ROM_NEW_CODE_NOTE, 2), c.noteForTest());
        TestSupport.screenshot(c.getWindow().getDecorView(), "ps1-3-home-new-code");

        // Play (focused first): the disc to the game activity.
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        Intent game = shadowOf(c).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(ShellActivity.class.getName(), game.getComponent().getClassName());
        assertEquals(cue.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals(key, GameStore.launchedTitleId(app));
    }

    /**
     * XBA-174: an N64 card is Ready (the N64 badge, no chip); A opens its Home, even without the toolchain: Play first
     * and the pak in the controller beside it, no Prepare without the toolchain (n64HomePrepares: with it). The pak button switches this game
     * to the rumble pak and back (N64Pak, a file per game; the note says which); Play starts the game activity with
     * the cartridge and the library key n64-<code>-<crc1>. Start on the card launches at once.
     */
    @Test
    public void n64HomePlays() throws Exception {
        folderGame(0);
        File dir = tmp.newFolder("n64");
        File rom = new File(dir, "Star Race.z64");
        TestSupport.write(rom, ConsoleTest.n64(0));
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3An64",
                          dir.getAbsolutePath());
        String key = "n64-NSME-635A2BFF";
        LauncherActivity a = launch();
        GameCard star = card(a, "Star Race");
        assertNotNull(star);
        assertEquals("N64", star.badgeForTest());
        assertFalse("Ready: no chip", star.chipShownForTest());
        a.forceFocusForTest();
        idle(300);
        star.requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertNull("not launched", shadowOf(a).getNextStartedActivity());
        assertEquals(key, a.romEntryForTest().tid);
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.ROM_N64_NOTE), a.noteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "n64-1-home");

        // The pak button (right of Play): the rumble pak for this game, then back to the controller pak.
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        View pak = a.focusForTest();
        assertTrue(pak instanceof Widgets.PillButton);
        assertEquals(app.getString(R.string.N64_PAK_CONTROLLER), ((Widgets.PillButton)pak).title);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(N64Pak.RUMBLE, N64Pak.choice(app, key));
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertEquals(app.getString(R.string.ROM_N64_NOTE_RUMBLE), a.noteForTest());
        a.forceFocusForTest();
        idle(300);
        pak = a.focusForTest();
        assertTrue("the focus stays on the pak button", pak instanceof Widgets.PillButton
                   && app.getString(R.string.N64_PAK_RUMBLE).equals(((Widgets.PillButton)pak).title));
        TestSupport.screenshot(a.getWindow().getDecorView(), "n64-2-home-rumble");
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(N64Pak.CONTROLLER, N64Pak.choice(app, key));
        assertNull("another game keeps its own (none)", N64Pak.choice(app, "n64-NSME-00000001"));

        // Play: the cartridge to the game activity.
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent game = shadowOf(a).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(ShellActivity.class.getName(), game.getComponent().getClassName());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals(key, GameStore.launchedTitleId(app));

        // Start on the card (a fresh launcher): the same launch, no Home.
        GameStore.rememberLaunch(app, GameStore.chosenDir(app), "", null);
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Star Race").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_START);
        assertNotNull(shadowOf(b).getNextStartedActivity());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals(LauncherActivity.Screen.LIBRARY, b.screenForTest());
    }

    /**
     * XBA-174: with the toolchain, an N64 Home has Prepare like a PS1 disc's: Play first, Prepare beside it, the pak
     * last; « Not prepared » (with the pak hint). Prepare starts PrepareService with the cartridge and the library key
     * n64-<code>-<crc1> and shows the Preparing screen; a valid pack says « Prepared »; code the runs found since
     * offers Prepare again; Play launches the cartridge in every state.
     */
    @Test
    public void n64HomePrepares() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        folderGame(0);
        File dir = tmp.newFolder("n64");
        File rom = new File(dir, "Star Race.v64");
        TestSupport.write(rom, ConsoleTest.n64(1));
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3An64",
                          dir.getAbsolutePath());
        String key = "n64-NSME-635A2BFF";
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        card(a, "Star Race").requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertNull("not launched", shadowOf(a).getNextStartedActivity());
        assertEquals(key, a.romEntryForTest().tid);
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.ROM_N64_NOT_PREPARED_NOTE), a.noteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "n64-3-home-not-prepared");

        // Play, Prepare, the pak: the pak is last.
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        View pak = a.focusForTest();
        assertTrue(pak instanceof Widgets.PillButton);
        assertEquals(app.getString(R.string.N64_PAK_CONTROLLER), ((Widgets.PillButton)pak).title);

        // Prepare (right of Play): the service with the cartridge and the library key, then the Preparing screen.
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);
        View prep = a.focusForTest();
        assertTrue(prep instanceof Widgets.PillButton);
        assertEquals(app.getString(R.string.PREPARE), ((Widgets.PillButton)prep).title);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertEquals(key, started.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(rom.getAbsolutePath(), started.getStringExtra(PrepareService.EXTRA_GAME_DIR));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertEquals(PrepareProtocol.RUNNING, PrepareProtocol.readStatus(PrepareService.statusFile(app, key)).state);

        // Done: a valid pack (console n64). Its Home says Prepared, Play first, no second Prepare.
        File pdir = Pack.packDir(app.getFilesDir(), key);
        TestSupport.write(new File(pdir, "libtitle.so"), new byte[16]);
        org.json.JSONObject o = new org.json.JSONObject().put("format", 1).put("console", "n64")
                .put("title_key", "NSME-635A2BFF").put("abi", "arm64-v8a").put("shell_abi", Pack.SHELL_ABI)
                .put("lib", "libtitle.so").put("lib_size", 16).put("source_sha256", "ab").put("discovered", 1);
        TestSupport.write(new File(pdir, "pack.json"), o.toString());
        PrepareProtocol.Status st = PrepareProtocol.readStatus(PrepareService.statusFile(app, key));
        st.state = PrepareProtocol.DONE;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, key), st);
        TestSupport.write(Pack.discoveredFile(app.getFilesDir(), key),
                          "807FFEA0 9B0F0C6562018CB6 3C018080\n");   // what the pack was built with
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Star Race").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, b.screenForTest());
        assertEquals(app.getString(R.string.ROM_PREPARED_NOTE), b.noteForTest());
        b.forceFocusForTest();
        idle(300);
        key(b, KeyEvent.KEYCODE_DPAD_RIGHT);
        View next = b.focusForTest();
        assertTrue("the pak right after Play", next instanceof Widgets.PillButton
                   && app.getString(R.string.N64_PAK_CONTROLLER).equals(((Widgets.PillButton)next).title));
        TestSupport.screenshot(b.getWindow().getDecorView(), "n64-4-home-prepared");

        // The runs found code since (two more records): Prepare again.
        TestSupport.write(Pack.discoveredFile(app.getFilesDir(), key),
                          "807FFEA0 9B0F0C6562018CB6 3C018080\n807FFEA8 4913B54793434260 3C018080\n"
                          + "80300000 0000000000000001 03E00008\n");
        LauncherActivity c = launch();
        c.forceFocusForTest();
        idle(300);
        card(c, "Star Race").requestFocusFromTouch();
        idle(300);
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(app.getString(R.string.ROM_NEW_CODE_NOTE, 2), c.noteForTest());
        TestSupport.screenshot(c.getWindow().getDecorView(), "n64-5-home-new-code");

        // Play (focused first): the cartridge to the game activity.
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        Intent game = shadowOf(c).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(ShellActivity.class.getName(), game.getComponent().getClassName());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals(key, GameStore.launchedTitleId(app));
    }

    /**
     * Every PS1 disc has a Prepare, a .chd too (XBA-168: its boot EXE is read by the native disc reader); a GBA ROM
     * and an N64 cartridge have one.
     */
    @Test
    public void romPrepares() {
        LibraryScreen.Entry e = new LibraryScreen.Entry("ps1-SLUS-00594");
        e.active = new LibraryIndex.Item();
        e.active.name = "Space Race.cue";
        assertTrue(LauncherActivity.romPrepares(e));
        e.active.name = "Space Race.CHD";
        assertTrue(LauncherActivity.romPrepares(e));
        e.active = null;
        assertTrue(LauncherActivity.romPrepares(e));
        assertTrue(LauncherActivity.romPrepares(new LibraryScreen.Entry("gba-AXVE-00000001")));
        assertTrue(LauncherActivity.romPrepares(new LibraryScreen.Entry("n64-NSME-00000001")));
    }

    /** The track documents of a .cue seen through SAF (XBA-168): their URIs under the tree; nothing otherwise. */
    @Test
    public void romParts() {
        String tree = "content://com.android.externalstorage.documents/tree/primary%3Aps1";
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = "doc:primary:ps1/Game.cue";
        it.name = "Game.cue";
        it.parts.add("doc:primary:ps1/Game (Track 1).bin");
        it.parts.add("doc:primary:ps1/Game (Track 2).bin");
        assertEquals(java.util.Arrays.asList(
                         "content://com.android.externalstorage.documents/tree/primary%3Aps1/document/"
                         + "primary%3Aps1%2FGame%20(Track%201).bin",
                         "content://com.android.externalstorage.documents/tree/primary%3Aps1/document/"
                         + "primary%3Aps1%2FGame%20(Track%202).bin"),
                     LauncherActivity.romParts(it, tree));
        assertTrue(LauncherActivity.romParts(it, null).isEmpty());
        it.loc = "file:/sdcard/ps1/Game.cue";
        assertTrue("a .cue read as a file finds its tracks itself", LauncherActivity.romParts(it, tree).isEmpty());
        it.loc = "doc:primary:ps1/Game.bin";
        it.name = "Game.bin";
        assertTrue(LauncherActivity.romParts(it, tree).isEmpty());
        assertTrue(LauncherActivity.romParts(null, tree).isEmpty());
    }

    /** What the game process opens for a ROM: its path, or its document under the library folder's tree. */
    @Test
    public void romSpec() {
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = "file:/storage/emulated/0/gba/Game.gba";
        assertEquals("/storage/emulated/0/gba/Game.gba", LauncherActivity.romSpec(it, null));
        it.loc = "doc:primary:gba/Game.gba";
        String tree = "content://com.android.externalstorage.documents/tree/primary%3Agba";
        assertEquals("content://com.android.externalstorage.documents/tree/primary%3Agba/document/"
                     + "primary%3Agba%2FGame.gba", LauncherActivity.romSpec(it, tree));
        assertNull(LauncherActivity.romSpec(it, null));
        it.loc = "";
        assertNull(LauncherActivity.romSpec(it, tree));
        assertNull(LauncherActivity.romSpec(null, tree));
    }

    /** The one library folder of an older launcher: migrated as the first entry, its games listed as before. */
    @Test
    public void olderSingleFolderMigrates() throws Exception {
        folderGame(0);
        GameStore.prefs(app).edit()
                .putString(Library.PREF_TREE, "content://com.android.externalstorage.documents/tree/primary%3Axbox")
                .putString(Library.PREF_PATH, root.getAbsolutePath()).commit();
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(1, a.libraryCardsForTest().size());
        assertEquals(1, Library.folders(app).size());
        assertEquals(root.getAbsolutePath(), Library.folders(app).get(0).path);
        assertNull(GameStore.prefs(app).getString(Library.PREF_TREE, null));
    }

    /**
     * XBA-158: with the on-device toolchain, A on a GBA card opens its Home: Play first (the interpreter, no pack
     * needed), « Not prepared » and Prepare beside it. Prepare starts PrepareService with the ROM and the library key
     * and shows the Preparing screen; a valid pack says « Prepared »; addresses the runs found since offer Prepare
     * again; Play launches the ROM in every state.
     */
    @Test
    public void gbaHomePrepares() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        folderGame(0);
        File gba = tmp.newFolder("gba");
        File rom = new File(gba, "Pocket Quest.gba");
        TestSupport.write(rom, ConsoleTest.gba("AXVE"));
        useFolder();
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Agba",
                          gba.getAbsolutePath());
        LauncherActivity a = launch();
        String key = LibraryIndex.read(Library.indexFile(app.getFilesDir())).find(LibraryIndex.fileLoc(rom)).tid;
        a.forceFocusForTest();
        idle(300);
        card(a, "Pocket Quest").requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, a.screenForTest());
        assertNull("not launched", shadowOf(a).getNextStartedActivity());
        assertEquals(key, a.romEntryForTest().tid);
        assertEquals(app.getString(R.string.ROM_NOT_PREPARED_NOTE), a.noteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "gba-1-home-not-prepared");

        // Prepare (right of Play): the service with the ROM and the library key, then the Preparing screen.
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Intent started = shadowOf(app).getNextStartedService();
        assertNotNull("PrepareService started", started);
        assertEquals(PrepareService.ACTION_START, started.getAction());
        assertEquals(key, started.getStringExtra(PrepareService.EXTRA_TITLE_ID));
        assertEquals(rom.getAbsolutePath(), started.getStringExtra(PrepareService.EXTRA_GAME_DIR));
        assertEquals(LauncherActivity.Screen.PREPARE, a.screenForTest());
        assertEquals(PrepareProtocol.RUNNING, PrepareProtocol.readStatus(PrepareService.statusFile(app, key)).state);
        TestSupport.screenshot(a.getWindow().getDecorView(), "gba-2-preparing");

        // Done: a valid pack. Its Home says Prepared, Play first.
        File dir = Pack.packDir(app.getFilesDir(), key);
        TestSupport.write(new File(dir, "libtitle.so"), new byte[16]);
        org.json.JSONObject o = new org.json.JSONObject().put("format", 1).put("console", "gba")
                .put("title_key", key.substring(4)).put("abi", "arm64-v8a").put("shell_abi", Pack.SHELL_ABI)
                .put("lib", "libtitle.so").put("lib_size", 16).put("discovered", 0);
        TestSupport.write(new File(dir, "pack.json"), o.toString());
        PrepareProtocol.Status s = PrepareProtocol.readStatus(PrepareService.statusFile(app, key));
        s.state = PrepareProtocol.DONE;
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, key), s);
        LauncherActivity b = launch();
        b.forceFocusForTest();
        idle(300);
        card(b, "Pocket Quest").requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.ROM_HOME, b.screenForTest());
        assertEquals(app.getString(R.string.ROM_PREPARED_NOTE), b.noteForTest());
        TestSupport.screenshot(b.getWindow().getDecorView(), "gba-3-home-prepared");

        // The runs found addresses since: Prepare again offered, Play still first.
        TestSupport.write(Pack.discoveredFile(app.getFilesDir(), key), "0x08000101\n0x080001F0\n");
        LauncherActivity c = launch();
        c.forceFocusForTest();
        idle(300);
        card(c, "Pocket Quest").requestFocusFromTouch();
        idle(300);
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(app.getString(R.string.ROM_NEW_CODE_NOTE, 2), c.noteForTest());
        TestSupport.screenshot(c.getWindow().getDecorView(), "gba-4-home-new-code");

        // Play (focused first): the ROM to the game activity.
        key(c, KeyEvent.KEYCODE_BUTTON_A);
        Intent game = shadowOf(c).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(ShellActivity.class.getName(), game.getComponent().getClassName());
        assertEquals(rom.getAbsolutePath(), GameStore.launchedRom(app));
        assertEquals(key, GameStore.launchedTitleId(app));
    }

    /** A Ready game's preparation on its card and the bottom screen: Prepared OK, out of date WARN, Not prepared neutral. */
    @Test
    public void preparedChipColours() {
        LibraryScreen.Entry e = new LibraryScreen.Entry("n64-NSME-635A2BFF");
        e.chip = GameState.Chip.READY;
        assertEquals(Theme.OK, LibraryScreen.chipColor(e));
        assertFalse("a plain « Ready » is not shown", LibraryScreen.chipShown(e));
        assertEquals(app.getString(R.string.STATE_READY), LibraryScreen.chipText(app, e));
        e.prepared = GameState.Prepared.PREPARED;
        assertEquals(Theme.OK, LibraryScreen.chipColor(e));
        assertTrue(LibraryScreen.chipShown(e));
        assertEquals(app.getString(R.string.LIB_CHIP_PREPARED), LibraryScreen.chipText(app, e));
        e.prepared = GameState.Prepared.OUTDATED;
        assertEquals(Theme.WARN, LibraryScreen.chipColor(e));
        assertEquals(app.getString(R.string.LIB_CHIP_OUTDATED), LibraryScreen.chipText(app, e));
        e.prepared = GameState.Prepared.NOT_PREPARED;
        assertEquals(Theme.TEXT_DIM, LibraryScreen.chipColor(e));
        assertEquals(app.getString(R.string.LIB_CHIP_NOT_PREPARED), LibraryScreen.chipText(app, e));
        e.prepared = GameState.Prepared.NONE;
        e.chip = GameState.Chip.NEEDS_PREPARING;
        assertEquals(Theme.WARN, LibraryScreen.chipColor(e));
        assertEquals(app.getString(R.string.CHIP_NEEDS_PREPARE), LibraryScreen.chipText(app, e));
    }

    // ---- Network library (XBA-320, XBA-321) ----------------------------------------------------------------------

    /**
     * A torrent source: its games show before they are downloaded (the remote-content art, the cleaned file name,
     * « On the network »), alongside a local game; A opens the download sheet, Download queues it (NetService started)
     * and the card says so; Cancel takes it out. Options › Network sources lists the source. Rendered in every size
     * and language (library-network-*.png).
     */
    @Test
    public void torrentGamesShowAndDownloadOnA() throws Exception {
        folderGame(0);
        useFolder();
        RemoteLibrary.addTorrent(app.getFilesDir(), TorrentMetaTest.torrent("My Xbox games", true,
                TorrentMetaTest.files(TorrentMetaTest.file(4L << 30, "Halo Sample (USA) (Rev 1).iso"),
                                      TorrentMetaTest.file(2L << 30, "Night Racer [!].xiso"),
                                      TorrentMetaTest.file(16L << 20, "gba", "Pocket Quest (Europe).gba"))),
                null, 1);
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        GameCard halo = card(a, "Halo Sample");
        assertNotNull(halo);
        assertTrue(halo.remoteForTest());
        assertFalse(halo.hasArtForTest());
        assertEquals(app.getString(R.string.NET_CHIP_ON_NETWORK), halo.chipForTest());
        assertNotNull(card(a, "Night Racer"));
        assertNotNull(card(a, "Pocket Quest"));
        assertFalse(card(a, NAMES[0]).remoteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-network");

        // A on the card: the sheet with Download (its size); A again queues it.
        halo.requestFocusFromTouch();
        idle(200);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        Widgets.Sheet sheet = a.sheetForTest();
        assertNotNull(sheet);
        assertTrue(sheet.rowsForTest().get(0).title.startsWith(app.getString(R.string.NET_DOWNLOAD)));
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-network-sheet");
        shadowOf(app).clearStartedServices();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertNull(a.sheetForTest());
        Intent svc = shadowOf(app).getNextStartedService();
        assertNotNull(svc);
        assertEquals(NetService.ACTION_KICK, svc.getAction());
        List<NetDownloads.Job> jobs = NetDownloads.read(app.getFilesDir());
        assertEquals(1, jobs.size());
        assertEquals("Halo Sample", jobs.get(0).title);
        assertEquals(NetDownloads.QUEUED, jobs.get(0).state);
        halo = card(a, "Halo Sample");
        assertEquals(app.getString(R.string.NET_CHIP_WAITING), halo.chipForTest());

        // A on it while it waits: progress / Cancel; Cancel takes it out.
        halo.requestFocusFromTouch();
        idle(200);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        sheet = a.sheetForTest();
        assertEquals(app.getString(R.string.NET_CANCEL_DOWNLOAD), sheet.rowsForTest().get(1).title);
        shadowOf(app).clearStartedServices();
        sheet.rowsForTest().get(1).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        svc = shadowOf(app).getNextStartedService();   // the service cancels it (pause, then delete)
        assertEquals(NetService.ACTION_CANCEL, svc.getAction());
        assertEquals(jobs.get(0).key, svc.getStringExtra(NetService.EXTRA_KEY));
        NetRunner.cancelFiles(app.getFilesDir(), LibraryScanner.netDir(app), jobs.get(0).key);   // as it does
        idle(2500);   // the next library poll
        assertEquals(app.getString(R.string.NET_CHIP_ON_NETWORK), card(a, "Halo Sample").chipForTest());

        // Options › Network sources: the source with its games, then the add rows.
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        sheet = a.sheetForTest();
        assertEquals("My Xbox games", sheet.rowsForTest().get(3).value);
        sheet.rowsForTest().get(3).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        sheet = a.sheetForTest();
        List<String> rows = new ArrayList<>();
        for (Widgets.MenuRow r : sheet.rowsForTest())
            rows.add(r.title);
        assertEquals(java.util.Arrays.asList("My Xbox games", app.getString(R.string.NET_ADD_TORRENT),
                                             app.getString(R.string.NET_PASTE_MAGNET),
                                             app.getString(R.string.NET_SHOW),
                                             app.getString(R.string.NET_MOBILE_DATA)), rows);
        assertEquals(app.getString(R.string.NET_SOURCE_GAMES_N, 3), sheet.rowsForTest().get(0).value);
        assertNull("the add rows' labels stay whole", sheet.rowsForTest().get(1).value);
        assertNull(sheet.rowsForTest().get(2).value);
        assertEquals(app.getString(R.string.NET_ON), sheet.rowsForTest().get(3).value);
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-network-sources");

        // Show their games in the library: Off hides them (and the Show pill), On brings them back.
        sheet.rowsForTest().get(3).requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(app.getString(R.string.NET_OFF), a.sheetForTest().rowsForTest().get(3).value);
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        idle(300);
        assertNull(card(a, "Halo Sample"));
        assertNotNull(card(a, NAMES[0]));
        assertNull(a.libraryWherePillForTest());
        NetService.setShowInLibrary(app, true);
        a.refreshForTest();
        idle(300);
        assertNotNull(card(a, "Halo Sample"));
    }

    /**
     * The Show pill (XBA-319): all games, those on this device, those on the network, kept; the cards on the network
     * are told apart by their art and chip. Rendered (library-network-where-*.png).
     */
    @Test
    public void showPillSeparatesLocalAndNetworkGames() throws Exception {
        folderGame(0);
        folderGame(1);
        useFolder();
        RemoteLibrary.addTorrent(app.getFilesDir(), TorrentMetaTest.torrent("Mixed", false,
                TorrentMetaTest.files(TorrentMetaTest.file(4L << 30, "Halo Sample (USA).iso"),
                                      TorrentMetaTest.file(16L << 20, "Pocket Quest.gba"))), null, 1);
        LauncherActivity a = launch();
        Widgets.PillButton where = a.libraryWherePillForTest();
        assertNotNull(where);
        assertEquals(app.getString(R.string.LIB_WHERE_ALL), where.title);
        assertEquals(4, a.libraryCardsForTest().size());
        where.performClick();
        idle(300);
        assertEquals(app.getString(R.string.LIB_WHERE_LOCAL), a.libraryWherePillForTest().title);
        assertEquals(2, a.libraryCardsForTest().size());
        for (GameCard c : a.libraryCardsForTest())
            assertFalse(c.remoteForTest());
        a.libraryWherePillForTest().performClick();
        idle(300);
        assertEquals(2, a.libraryCardsForTest().size());
        for (GameCard c : a.libraryCardsForTest())
            assertTrue(c.remoteForTest());
        TestSupport.screenshot(a.getWindow().getDecorView(), "library-network-where");
        assertEquals(LibraryView.Where.REMOTE, LibraryView.load(app.getFilesDir()).where);   // kept
        // No game on the network any more (the source removed): no pill, every game shows.
        RemoteLibrary.remove(app.getFilesDir(), RemoteLibrary.sources(app.getFilesDir()).get(0).id);
        a.refreshForTest();
        idle(300);
        assertNull(a.libraryWherePillForTest());
        assertEquals(2, a.libraryCardsForTest().size());
    }

    /** The same library in every size and language (the remote art and chip must read on a 4:3 and a square screen). */
    @Test
    public void torrentGamesRender() throws Exception {
        folderGame(0);
        folderGame(1);
        useFolder();
        RemoteLibrary.addTorrent(app.getFilesDir(), TorrentMetaTest.torrent("Mixed", false,
                TorrentMetaTest.files(TorrentMetaTest.file(4L << 30, "Halo Sample (USA).iso"),
                                      TorrentMetaTest.file(16L << 20, "Pocket Quest.gba"),
                                      TorrentMetaTest.file(700L << 20, "ps1", "Disc Racer.cue"))),
                null, 1);
        render("network", null);
        // The longest Show label (« Afficher : Sur le réseau ») beside Search and Sort on every screen
        LibraryView.State v = LibraryView.load(app.getFilesDir());
        v.where = LibraryView.Where.REMOTE;
        LibraryView.save(app.getFilesDir(), v);
        render("network-where", null);
    }

    /**
     * A download that ended (its job DONE): the next polls find the game in the download folder's scan, move the
     * focus to it and import it (an Xbox image read in place), once, on the library screen only.
     */
    @Test
    public void aFinishedDownloadGoesOnToTheImport() throws Exception {
        folderGame(0);
        useFolder();
        byte[] tor = TorrentMetaTest.torrent("My Xbox games", true,
                TorrentMetaTest.files(TorrentMetaTest.file(64, "Night Racer (USA).iso")));
        RemoteLibrary.Source s = RemoteLibrary.addTorrent(app.getFilesDir(), tor, null, 1);
        RemoteLibrary.Game g = RemoteLibrary.games(s, RemoteLibrary.meta(app.getFilesDir(), s.id)).get(0);
        NetDownloads.Job j = NetDownloads.enqueue(app.getFilesDir(), g, 2);
        NetDownloads.edit(app.getFilesDir(), l -> {
            NetDownloads.find(l, j.key).state = NetDownloads.DONE;
            NetDownloads.find(l, j.key).done = 64;
        });
        // The downloaded image, and the index item a scan made of it (not opened again: same fingerprint).
        File dl = LibraryScanner.netDir(app);
        File iso = NetDownloads.jobFile(dl, NetDownloads.find(NetDownloads.read(app.getFilesDir()), j.key),
                                        "Night Racer (USA).iso");
        TestSupport.write(iso, new byte[64]);
        LibraryIndex x = new LibraryIndex();
        LibraryIndex.Item it = new LibraryIndex.Item();
        it.loc = LibraryIndex.fileLoc(iso);
        it.kind = LibraryIndex.KIND_ISO;
        it.name = iso.getName();
        it.size = iso.length();
        it.mtime = iso.lastModified();
        it.tid = tidHex(5);
        it.title = NAMES[5];
        it.xbeSha256 = "00";
        it.xbe = "xbe/" + it.tid + "-00000000/default.xbe";
        it.folder = LibraryScanner.NET_FOLDER;
        TestSupport.write(it.xbeFile(app.getFilesDir()), TestSupport.xbe(tid(5), NAMES[5]));
        x.items.add(it);
        x.identify = LibraryScanner.identifyStamp(app);
        LibraryIndex.write(Library.indexFile(app.getFilesDir()), x);

        LauncherActivity a = launch();
        assertNull("the network entry left: the game is local", card(a, "Night Racer"));
        assertNotNull(card(a, NAMES[5]));
        idle(2500);   // the next net poll
        waitScan(a);
        assertFalse(NetDownloads.find(NetDownloads.read(app.getFilesDir()), j.key).auto);
        assertEquals(tidHex(5), Library.read(Library.gamesFile(app.getFilesDir())).selected);
        Thread w = a.workerForTest();
        assertNotNull("the import started", w);
        w.join(20_000);
        idle(500);
    }

    @Test
    public void aFinishedDownloadWaitsWhileASheetIsOpen() throws Exception {
        folderGame(0);
        useFolder();
        byte[] rom = ConsoleTest.gba("AXVE");
        byte[] tor = TorrentMetaTest.torrent("T", true,
                TorrentMetaTest.files(TorrentMetaTest.file(rom.length, "Pocket Quest.gba")));
        RemoteLibrary.Source s = RemoteLibrary.addTorrent(app.getFilesDir(), tor, null, 1);
        RemoteLibrary.Game g = RemoteLibrary.games(s, RemoteLibrary.meta(app.getFilesDir(), s.id)).get(0);
        NetDownloads.Job j = NetDownloads.enqueue(app.getFilesDir(), g, 2);
        TestSupport.write(NetDownloads.jobFile(LibraryScanner.netDir(app), j, "Pocket Quest.gba"), rom);
        NetDownloads.edit(app.getFilesDir(), l -> NetDownloads.find(l, j.key).state = NetDownloads.DONE);
        NetDownloads.edit(app.getFilesDir(), l -> NetDownloads.find(l, j.key).auto = false);   // (until Options opens)
        LauncherActivity a = launch();
        assertNotNull(card(a, "Pocket Quest"));
        assertFalse("the network entry left: the ROM is local", card(a, "Pocket Quest").remoteForTest());
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_Y);   // Options open: nothing moves under it
        assertNotNull(a.sheetForTest());
        NetDownloads.edit(app.getFilesDir(), l -> NetDownloads.find(l, j.key).auto = true);   // the download ends now
        idle(2500);
        assertTrue(NetDownloads.find(NetDownloads.read(app.getFilesDir()), j.key).auto);
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        idle(2500);   // the sheet closed: it goes on (a GBA game is ready: the focus moves to it)
        assertFalse(NetDownloads.find(NetDownloads.read(app.getFilesDir()), j.key).auto);
    }

    /**
     * A second torrent while a download moves on (Alex, 2026-11-20: « ça propose pas la plateforme »): the library
     * poll saw the progress change every 2 s and rebuilt the screen, closing the console sheet before a choice. The
     * sheet stays; the refresh comes once it closes; the source gets the console picked.
     */
    @Test
    public void theConsoleSheetStaysWhileADownloadMovesOn() throws Exception {
        folderGame(0);
        useFolder();
        byte[] tor = TorrentMetaTest.torrent("First", true,
                TorrentMetaTest.files(TorrentMetaTest.file(64L << 20, "Halo Sample (USA).iso")));
        RemoteLibrary.Source s = RemoteLibrary.addTorrent(app.getFilesDir(), tor, null, 1);
        RemoteLibrary.Game g = RemoteLibrary.games(s, RemoteLibrary.meta(app.getFilesDir(), s.id)).get(0);
        NetDownloads.Job j = NetDownloads.enqueue(app.getFilesDir(), g, 2);
        NetDownloads.edit(app.getFilesDir(), l -> NetDownloads.find(l, j.key).state = NetDownloads.RUNNING);
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);

        String hash = "0123456789abcdef0123456789abcdef01234567";
        app.getSystemService(android.content.ClipboardManager.class)
           .setPrimaryClip(android.content.ClipData.newPlainText("m", "magnet:?xt=urn:btih:" + hash + "&dn=Second"));
        key(a, KeyEvent.KEYCODE_BUTTON_Y);   // Options › Network sources › Paste a magnet link
        Widgets.Sheet sheet = a.sheetForTest();
        for (Widgets.MenuRow r : sheet.rowsForTest())
            if (r.title.equals(app.getString(R.string.NET_SOURCES)))
                r.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        for (Widgets.MenuRow r : a.sheetForTest().rowsForTest())
            if (r.title.equals(app.getString(R.string.NET_PASTE_MAGNET)))
                r.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        sheet = a.sheetForTest();
        assertNotNull(sheet);
        assertEquals(app.getString(R.string.NET_CONSOLE_BY_NAME), sheet.rowsForTest().get(0).title);

        for (int p = 1; p <= 3; p++) {   // the download moves on: polls see it
            long done = p * (8L << 20);
            NetDownloads.edit(app.getFilesDir(), l -> NetDownloads.find(l, j.key).done = done);
            idle(2500);
            assertSame("the console sheet stays open", sheet, a.sheetForTest());
        }
        Widgets.MenuRow gba = null;
        for (Widgets.MenuRow r : sheet.rowsForTest())
            if (r.title.equals(Console.GBA.badge()))
                gba = r;
        assertNotNull(gba);
        gba.requestFocusFromTouch();
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        idle(300);
        assertNull(a.sheetForTest());
        List<RemoteLibrary.Source> l = RemoteLibrary.sources(app.getFilesDir());
        assertEquals(2, l.size());
        assertEquals(hash, l.get(1).id);
        assertEquals(Console.GBA, l.get(1).console);
        assertEquals(null, l.get(0).console);
    }
}
