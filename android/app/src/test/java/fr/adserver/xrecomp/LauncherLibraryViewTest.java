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
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Consumer;

/**
 * The library's view in the launcher (XBA-206, docs/android/app-shell.md « Game library »): X on a card and on a
 * game's Home (favourites first, a star), the Sort pill, the console tabs (L1 / R1), the search typed on the keyboard,
 * « Recently played », the view kept for the next start, the play session closed with the game's own count. Rendered to
 * build/screenshots/library-view-*.png. Synthetic games only.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherLibraryViewTest {
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
                                            "Castle Builder", "Robot Soccer", "Snow Board", "Desert Run" };
    private static final long MIN = 60_000;

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
    }

    @After
    public void tearDown() {
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    /** A key press as the window delivers it (LauncherLibraryTest.key: the D-pad moves the focus by geometry). */
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

    private static String tidHex(int i) {
        return String.format("%08X", 0x58420001 + i);
    }

    private File folderGame(int i) throws Exception {
        File d = new File(root, NAMES[i]);
        TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(0x58420001 + i, NAMES[i], XbeInfo.REGION_WORLD,
                                                                         0, 0x100));
        return d;
    }

    private void useFolder() {
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
    }

    /** Games 0..n-1 in the folder; those in played get a record with when they were played and for how long. */
    private void library(int n, long[][] played) throws Exception {
        Library.Games g = new Library.Games();
        for (int i = 0; i < n; i++)
            folderGame(i);
        long now = System.currentTimeMillis();
        for (long[] p : played) {
            int i = (int)p[0];
            Library.Record r = new Library.Record();
            r.titleId = tidHex(i);
            r.title = NAMES[i];
            File d = new File(root, NAMES[i]);
            r.dataDir = d.getAbsolutePath();
            r.dataKind = Library.KIND_IN_PLACE;
            r.source = LibraryIndex.fileLoc(d);
            r.lastPlayed = now - p[1] * MIN;
            r.playMs = p[2] * MIN;
            g.put(r);
        }
        Library.write(Library.gamesFile(app.getFilesDir()), g);
        useFolder();
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

    private static List<String> titles(List<GameCard> cards) {
        List<String> out = new ArrayList<>();
        for (GameCard c : cards)
            out.add(c.titleForTest());
        return out;
    }

    private static GameCard card(LauncherActivity a, String title) {
        for (GameCard c : a.libraryCardsForTest())
            if (title.equals(c.titleForTest()))
                return c;
        return null;
    }

    private static List<String> glyphs(LauncherActivity a) {
        List<String> out = new ArrayList<>();
        for (Widgets.Hint h : a.hintsForTest())
            out.add(h.glyphForTest());
        return out;
    }

    private void render(String name, Consumer<LauncherActivity> act) throws Exception {
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                LauncherActivity a = launch();
                if (a.focusForTest() == null)
                    a.forceFocusForTest();
                idle(300);
                if (act != null)
                    act.accept(a);
                idle(400);
                File f = TestSupport.screenshot(a.getWindow().getDecorView(),
                                                "library-view-" + name + "-" + loc + "-" + size[0]);
                assertTrue(f.length() > 0);
            }
        }
    }

    // ---- Favourites ---------------------------------------------------------------------------------------------

    @Test
    public void xMakesAFavouriteFirstWithAStar() throws Exception {
        library(7, new long[0][]);
        LauncherActivity a = launch();
        assertEquals(Arrays.asList("A", "X", "Y"), glyphs(a));
        assertEquals("Night Drive", a.libraryCardsForTest().get(0).titleForTest());
        a.forceFocusForTest();
        idle(300);
        GameCard puzzle = card(a, "Puzzle Garden");
        puzzle.requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        GameCard first = a.libraryCardsForTest().get(0);
        assertEquals("Puzzle Garden", first.titleForTest());
        assertTrue(first.favouriteForTest());
        assertFalse(card(a, "Night Drive").favouriteForTest());
        assertSame("the focus follows the game", first, a.focusForTest());
        assertTrue(a.noteForTest(), a.noteForTest().contains("Puzzle Garden"));
        assertTrue(LibraryView.load(app.getFilesDir()).favourite(tidHex(4)));

        // Kept: first again at the next start; X again takes it out.
        LauncherActivity b = launch();
        assertEquals("Puzzle Garden", b.libraryCardsForTest().get(0).titleForTest());
        assertTrue(b.libraryCardsForTest().get(0).favouriteForTest());
        b.forceFocusForTest();
        idle(300);
        b.libraryCardsForTest().get(0).requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals("Night Drive", b.libraryCardsForTest().get(0).titleForTest());
        assertFalse(LibraryView.load(app.getFilesDir()).favourite(tidHex(4)));
        // X on a pill does nothing.
        b.librarySortPillForTest().requestFocusFromTouch();
        idle(300);
        key(b, KeyEvent.KEYCODE_BUTTON_X);
        assertTrue(LibraryView.load(app.getFilesDir()).favourites.isEmpty());
    }

    @Test
    public void xOnAGamesHome() throws Exception {
        library(3, new long[][] { { 0, 30, 95 } });
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        card(a, "Sample Racer").requestFocusFromTouch();
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertTrue(glyphs(a).contains("X"));
        assertEquals(app.getString(R.string.LIB_FAV_ADD), hintLabel(a, "X"));
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertTrue(LibraryView.load(app.getFilesDir()).favourite(tidHex(0)));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(app.getString(R.string.LIB_FAV_REMOVE), hintLabel(a, "X"));
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals("Sample Racer", a.libraryCardsForTest().get(0).titleForTest());
        assertTrue(a.libraryCardsForTest().get(0).favouriteForTest());
    }

    private static String hintLabel(LauncherActivity a, String glyph) {
        for (Widgets.Hint h : a.hintsForTest())
            if (glyph.equals(h.glyphForTest()))
                return h.labelForTest();
        return null;
    }

    // ---- Sort, console, search ----------------------------------------------------------------------------------

    @Test
    public void sortPillStepsAndIsKept() throws Exception {
        library(4, new long[][] { { 2, 600, 5 }, { 1, 10, 30 }, { 3, 60, 300 } });
        LauncherActivity a = launch();
        assertEquals(Arrays.asList("Night Drive", "Sample Quest", "Sample Racer", "Tiny Tanks"),
                     titles(a.libraryCardsForTest()));
        assertTrue("4 games: one row, no « Recently played »", a.libraryRecentCardsForTest().isEmpty());
        Widgets.PillButton sort = a.librarySortPillForTest();
        assertEquals(app.getString(R.string.LIB_SORT, app.getString(R.string.LIB_SORT_NAME)), sort.title);
        sort.performClick();
        idle(300);
        assertEquals(LibraryView.Sort.LAST_PLAYED, a.libraryViewForTest().sort);
        assertEquals(Arrays.asList("Sample Quest", "Night Drive", "Tiny Tanks", "Sample Racer"),
                     titles(a.libraryCardsForTest()));
        assertEquals(app.getString(R.string.LIB_SORT, app.getString(R.string.LIB_SORT_LAST_PLAYED)),
                     a.librarySortPillForTest().title);
        assertSame("the pill stays (the focus with it)", sort, a.librarySortPillForTest());
        sort.performClick();
        idle(300);
        assertEquals(LibraryView.Sort.CONSOLE, a.libraryViewForTest().sort);
        sort.performClick();
        idle(300);
        assertEquals(LibraryView.Sort.MOST_PLAYED, a.libraryViewForTest().sort);
        assertEquals(Arrays.asList("Night Drive", "Sample Quest", "Tiny Tanks", "Sample Racer"),
                     titles(a.libraryCardsForTest()));
        LauncherActivity b = launch();
        assertEquals(LibraryView.Sort.MOST_PLAYED, b.libraryViewForTest().sort);
        assertEquals(Arrays.asList("Night Drive", "Sample Quest", "Tiny Tanks", "Sample Racer"),
                     titles(b.libraryCardsForTest()));
    }

    @Test
    public void consoleTabsFilterAndAreKept() throws Exception {
        library(2, new long[0][]);
        File gba = tmp.newFolder("gba");
        TestSupport.write(new File(gba, "Pocket Quest.gba"), ConsoleTest.gba("AXVE"));
        Library.addFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Agba",
                          gba.getAbsolutePath());
        LauncherActivity a = launch();
        assertEquals(3, a.libraryCardsForTest().size());
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(Console.XBOX, a.libraryViewForTest().console);
        assertEquals(Arrays.asList("Sample Quest", "Sample Racer"), titles(a.libraryCardsForTest()));
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(Console.GBA, a.libraryViewForTest().console);
        assertEquals(Arrays.asList("Pocket Quest"), titles(a.libraryCardsForTest()));
        LauncherActivity b = launch();
        assertEquals(Console.GBA, b.libraryViewForTest().console);
        assertEquals(1, b.libraryCardsForTest().size());
        key(b, KeyEvent.KEYCODE_BUTTON_L1);
        key(b, KeyEvent.KEYCODE_BUTTON_L1);
        assertNull(b.libraryViewForTest().console);
        key(b, KeyEvent.KEYCODE_BUTTON_L1);   // round: from « All » to the last console
        assertEquals(Console.GBA, b.libraryViewForTest().console);
        render("console", null);
    }

    @Test
    public void consoleOfAnXboxOnlyLibraryIsIgnored() throws Exception {
        library(2, new long[0][]);
        LibraryView.State s = new LibraryView.State();
        s.console = Console.GBA;   // saved when GBA games were there
        LibraryView.save(app.getFilesDir(), s);
        LauncherActivity a = launch();
        assertEquals(2, a.libraryCardsForTest().size());
        key(a, KeyEvent.KEYCODE_BUTTON_R1);   // no tabs: nothing
        assertNull(a.libraryViewForTest().console);
    }

    @Test
    public void searchFollowsTheKeyboardAndBClearsIt() throws Exception {
        library(7, new long[0][]);
        LauncherActivity a = launch();
        a.forceFocusForTest();
        idle(300);
        a.librarySearchPillForTest().performClick();
        idle(300);
        android.widget.EditText t = a.librarySearchForTest();
        assertNotNull(t);
        assertSame(t, a.focusForTest());
        t.setText("sample");
        idle(100);
        assertEquals(Arrays.asList("Sample Quest", "Sample Racer"), titles(a.libraryCardsForTest()));
        t.setText("SAMPLE rac");
        idle(100);
        assertEquals(Arrays.asList("Sample Racer"), titles(a.libraryCardsForTest()));
        assertSame("the field keeps the focus while typing", t, a.focusForTest());
        t.onEditorAction(android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH);
        idle(300);
        assertSame(a.libraryCardsForTest().get(0), a.focusForTest());
        assertEquals(app.getString(R.string.LIB_SEARCH_Q, "SAMPLE rac"), a.librarySearchPillForTest().title);
        t.setText("zzz");
        idle(100);
        assertTrue(a.libraryCardsForTest().isEmpty());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals("B clears the search, the library stays", LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertFalse(a.isFinishing());
        assertEquals("", a.libraryViewForTest().query);
        assertEquals(7, a.libraryCardsForTest().size());
        assertNull(a.librarySearchForTest());
        render("search", b -> {
            b.librarySearchPillForTest().performClick();
            idle(300);
            b.librarySearchForTest().setText("s");
        });
    }

    // ---- Recently played ----------------------------------------------------------------------------------------

    @Test
    public void recentlyPlayedRow() throws Exception {
        library(9, new long[][] { { 6, 3000, 40 }, { 2, 5, 192 }, { 8, 120, 15 } });
        LauncherActivity a = launch();
        assertEquals(Arrays.asList("Tiny Tanks", "Castle Builder", "Ocean Trip"),
                     titles(a.libraryRecentCardsForTest()));
        assertEquals(9, a.libraryCardsForTest().size());
        a.forceFocusForTest();
        idle(300);
        assertSame("the last game played: its grid card gets the focus", card(a, "Tiny Tanks"), a.focusForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_R1);   // (one console: nothing)
        a.librarySearchPillForTest().performClick();
        idle(300);
        a.librarySearchForTest().setText("o");
        idle(100);
        assertTrue("a search hides the row", a.libraryRecentCardsForTest().isEmpty());
        LibraryView.State fav = new LibraryView.State();
        fav.toggleFavourite(tidHex(4));   // Puzzle Garden: first in the grid, its star
        LibraryView.save(app.getFilesDir(), fav);
        render("recent", b -> {
            assertEquals("Puzzle Garden", b.libraryCardsForTest().get(0).titleForTest());
            b.libraryRecentCardsForTest().get(0).requestFocusFromTouch();
        });
    }

    // ---- Play time ----------------------------------------------------------------------------------------------

    @Test
    public void theSessionEndsWithTheGamesOwnCount() throws Exception {
        library(2, new long[][] { { 0, 0, 0 } });
        long start = System.currentTimeMillis() - 90 * MIN;
        GameStore.sessionStart(app, start, tidHex(0));
        Library.Games g = Library.read(Library.gamesFile(app.getFilesDir()));
        Library.sessionStart(g, tidHex(0), start);
        Library.write(Library.gamesFile(app.getFilesDir()), g);
        // The game was on screen 12 min (and 78 min in the background).
        PlayClock c = PlayClock.begin(null, tidHex(0), start, start + 2000);
        c.resume(start + 2000);
        c.pause(start + 2000 + 12 * MIN);
        assertTrue(c.write(PlayClock.file(app.getFilesDir())));
        launch();
        Library.Record r = Library.read(Library.gamesFile(app.getFilesDir())).get(tidHex(0));
        assertEquals(12 * MIN, r.playMs);
        assertEquals(start, r.lastPlayed);
        assertEquals(0, GameStore.sessionStartedAt(app));
        assertEquals(12 * MIN, GameStore.playTime(app));
    }
}
