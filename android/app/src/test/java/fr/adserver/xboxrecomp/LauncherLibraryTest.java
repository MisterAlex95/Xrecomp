package fr.adserver.xboxrecomp;

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
                                              { "1080", "w1920dp-h1080dp-land-mdpi" } };
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
        assertEquals(java.util.Arrays.asList("A", "START", "Y"), hints);
        assertTrue("library: stock colours", Theme.isStock());
        assertTrue(a.companionForTest().libraryForTest());
        render("7", null);
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

    /** Y: the Options sheet (Rescan, Storage, Library folder, Settings, About); B closes it, the focus comes back. */
    @Test
    public void optionsSheet() throws Exception {
        for (int i = 0; i < 3; i++)
            folderGame(i);
        useFolder();
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        GameCard focused = (GameCard)a.focusForTest();
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals("X no longer opens Storage", LauncherActivity.Screen.LIBRARY, a.screenForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        Widgets.Sheet sheet = a.sheetForTest();
        assertNotNull(sheet);
        List<String> rows = new ArrayList<>();
        for (Widgets.MenuRow r : sheet.rowsForTest())
            rows.add(r.title);
        assertEquals(java.util.Arrays.asList(app.getString(R.string.LIB_RESCAN), app.getString(R.string.LIB_STORAGE),
                                             app.getString(R.string.LIB_FOLDER_TILE), app.getString(R.string.SETTINGS),
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

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }
}
