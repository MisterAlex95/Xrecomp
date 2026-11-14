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
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.os.Looper;
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
 * A game on the home screen (docs/android/app-shell.md « A game on the home screen »): Manage's « Add to home
 * screen » row and its pin request (id, intent), hidden when the home screen takes no requests; ACTION_PLAY_TITLE
 * starting a ready game, showing the Home of one that is not, opening the library with a note for a title ID it no
 * longer has; the icon (title image on its border's colour, monogram) and the labels.
 *
 * Synthetic only: XBEs from TestSupport.xbe in a temporary library folder, an image game from a prepared index.json.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherShortcutTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File root;
    private static final String[] NAMES = { "Sample Racer", "Sample Quest", "Tiny Tanks", "Night Drive" };

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

    private static int tid(int i) {
        return 0x58420001 + i;   // « XB » + a number: ours
    }

    private static String tidHex(int i) {
        return String.format("%08X", tid(i));
    }

    private void folderGame(int i) throws Exception {
        File d = new File(root, NAMES[i]);
        TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(tid(i), NAMES[i], XbeInfo.REGION_WORLD, 0,
                                                                         0x100));
    }

    /** An image the scan does not open (its index.json item and cached XBE, as LauncherLibraryTest.imageGame). */
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

    private void useFolder() {
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
    }

    private LauncherActivity launch(Intent in) throws Exception {
        LauncherActivity a = (in == null ? Robolectric.buildActivity(LauncherActivity.class)
                                         : Robolectric.buildActivity(LauncherActivity.class, in)).setup().get();
        idle(300);
        Thread t = a.scannerForTest();
        if (t != null)
            t.join(20_000);
        idle(400);
        return a;
    }

    /** A first start: the library scanned (index.json written), as before any shortcut can exist. */
    private void scanned() throws Exception {
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        a.finish();
    }

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    private Widgets.MenuRow row(LauncherActivity a, int title) {
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow && app.getString(title).equals(((Widgets.MenuRow)v).title))
                return (Widgets.MenuRow)v;
        }
        return null;
    }

    private Intent shortcut(String titleId) {
        return TitleShortcut.intent(app, titleId);
    }

    // ---- Manage › Add to home screen --------------------------------------------------------------------------

    @Test
    public void manageRowRequestsThePin() throws Exception {
        folderGame(0);
        folderGame(1);
        useFolder();
        scanned();
        LauncherActivity a = launch(null);
        GameCard card = null;
        for (GameCard c : a.libraryCardsForTest()) {
            if (NAMES[1].equals(c.titleForTest()))
                card = c;
        }
        assertNotNull(card);
        card.performClick();
        idle(300);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        a.goForTest(LauncherActivity.Screen.GAME_DATA);
        idle(300);
        Widgets.MenuRow r = row(a, R.string.SHORTCUT_ADD);
        assertNotNull("Manage offers « Add to home screen »", r);
        assertEquals(app.getString(R.string.SHORTCUT_ADD_DETAIL), r.description);
        r.performClick();
        idle(300);
        assertEquals(app.getString(R.string.SHORTCUT_REQUESTED), a.noteForTest());
        ShortcutManager sm = app.getSystemService(ShortcutManager.class);
        List<ShortcutInfo> pinned = sm.getPinnedShortcuts();
        assertEquals(1, pinned.size());
        ShortcutInfo s = pinned.get(0);
        assertEquals("game_" + tidHex(1), s.getId());
        assertEquals(NAMES[1], s.getShortLabel().toString());   // « Sample Quest »: 12 characters, whole
        assertEquals(NAMES[1], s.getLongLabel().toString());
        Intent in = s.getIntent();
        assertEquals(LauncherActivity.ACTION_PLAY_TITLE, in.getAction());
        assertEquals(tidHex(1), in.getStringExtra(LauncherActivity.EXTRA_TITLE_ID));
        assertEquals(LauncherActivity.class.getName(), in.getComponent().getClassName());
        assertTrue((in.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0);
        // The pinned shortcut follows a later change of art or title, without a crash (updateShortcuts).
        TitleShortcut.refresh(app, tidHex(1), "Renamed", null);
        assertEquals("Renamed", sm.getPinnedShortcuts().get(0).getShortLabel().toString());
    }

    @Test
    public void manageHidesTheRowWithoutPinSupport() throws Exception {
        shadowOf(app.getSystemService(ShortcutManager.class)).setIsRequestPinShortcutSupported(false);
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        LauncherActivity a = launch(null);
        a.goForTest(LauncherActivity.Screen.GAME_DATA);
        idle(300);
        assertNull(row(a, R.string.SHORTCUT_ADD));
    }

    /** One game, no library folder (today's flow): the row is there too, the title ID from the XBE. */
    @Test
    public void manageRowForTheOneGame() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), TestSupport.xbe(0x4D530004, "Sample"));
        LauncherActivity a = launch(null);
        a.goForTest(LauncherActivity.Screen.GAME_DATA);
        idle(300);
        Widgets.MenuRow r = row(a, R.string.SHORTCUT_ADD);
        assertNotNull(r);
        r.performClick();
        idle(300);
        assertEquals("game_4D530004",
                     app.getSystemService(ShortcutManager.class).getPinnedShortcuts().get(0).getId());
    }

    // ---- ACTION_PLAY_TITLE ------------------------------------------------------------------------------------

    @Test
    public void shortcutStartsAReadyGame() throws Exception {
        folderGame(0);
        folderGame(1);
        folderGame(2);
        useFolder();
        scanned();
        LauncherActivity a = launch(shortcut(tidHex(2)));
        Intent started = shadowOf(a).getNextStartedActivity();
        assertNotNull(started);
        assertEquals(ShellActivity.class.getName(), started.getComponent().getClassName());
        assertEquals(tidHex(2), GameStore.launchedTitleId(app));
        assertEquals(tidHex(2), Library.read(Library.gamesFile(app.getFilesDir())).selected);
        assertTrue(a.isFinishing());
    }

    @Test
    public void shortcutShowsTheHomeOfAGameNotReady() throws Exception {
        folderGame(0);
        imageGame(3);
        useFolder();
        scanned();
        LauncherActivity a = launch(shortcut(tidHex(3)));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(app.getString(R.string.LIB_IMPORT), a.homePrimaryForTest().title);
        assertEquals(tidHex(3), Library.read(Library.gamesFile(app.getFilesDir())).selected);
        Intent started = shadowOf(a).getNextStartedActivity();
        assertTrue("nothing started", started == null
                                      || !ShellActivity.class.getName().equals(started.getComponent().getClassName()));
        assertNull(shadowOf(app).getNextStartedService());
        assertFalse(a.isFinishing());
    }

    @Test
    public void shortcutOfAGameNoLongerThereOpensTheLibrary() throws Exception {
        folderGame(0);
        folderGame(1);
        useFolder();
        scanned();
        LauncherActivity a = launch(shortcut("4D53FFFF"));
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(app.getString(R.string.SHORTCUT_GAME_GONE), a.noteForTest());
        assertNull(shadowOf(a).getNextStartedActivity());
        // No extra at all: the same, no crash.
        a = launch(new Intent(app, LauncherActivity.class).setAction(LauncherActivity.ACTION_PLAY_TITLE));
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(app.getString(R.string.SHORTCUT_GAME_GONE), a.noteForTest());
    }

    // ---- Icon and labels --------------------------------------------------------------------------------------

    /** The icon as build/screenshots/<name>.png, to look at. */
    private static void save(Bitmap bm, String name) throws java.io.IOException {
        File out = new File(System.getProperty("xba.screenshots", "build/screenshots"), name + ".png");
        out.getParentFile().mkdirs();
        try (java.io.OutputStream o = new java.io.FileOutputStream(out)) {
            bm.compress(Bitmap.CompressFormat.PNG, 100, o);
        }
    }

    /** The icon from a decoded $$XTIMAGE (the test picture of TestSupport), for the screenshots. */
    @Test
    public void iconFromAnXbeImage() throws Exception {
        File xbe = new File(tmp.newFolder("g"), "default.xbe");
        TestSupport.write(xbe, TestSupport.xbeWithImage(tid(0), NAMES[0],
                                                        TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture())));
        Bitmap art = TitleArt.load(app.getFilesDir(), xbe, tidHex(0));
        assertNotNull(art);
        Bitmap ic = TitleShortcut.icon(art, tidHex(0), NAMES[0]);
        assertEquals(0xFF, ic.getPixel(0, 0) >>> 24);
        save(ic, "shortcut-icon-image");
    }

    @Test
    public void iconFromTheTitleImage() throws Exception {
        Bitmap art = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        art.eraseColor(0xFF2050C0);
        art.setPixel(32, 32, 0xFFFFFFFF);
        Bitmap ic = TitleShortcut.icon(art, "58420001", "Sample Racer");
        assertEquals(TitleShortcut.SIDE, ic.getWidth());
        assertEquals(TitleShortcut.SIDE, ic.getHeight());
        assertEquals("the border's colour behind", 0xFF2050C0, ic.getPixel(2, 2));
        assertEquals(0xFF2050C0, ic.getPixel(TitleShortcut.SIDE / 2 - 40, TitleShortcut.SIDE / 2));

        // A logo on nothing: the dark background.
        Bitmap cut = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
        cut.eraseColor(0);
        for (int y = 16; y < 48; y++)
            for (int x = 16; x < 48; x++)
                cut.setPixel(x, y, 0xFFE0A020);
        assertEquals(TitleShortcut.DARK, TitleShortcut.borderColor(cut));
        Bitmap ic2 = TitleShortcut.icon(cut, "58420001", "Sample Racer");
        assertEquals(TitleShortcut.DARK, ic2.getPixel(2, 2));
        assertEquals(0xFFE0A020, ic2.getPixel(TitleShortcut.SIDE / 2, TitleShortcut.SIDE / 2));
        save(ic2, "shortcut-icon-cutout");
    }

    @Test
    public void monogramIconWithoutImage() throws Exception {
        Bitmap ic = TitleShortcut.icon(null, "58420001", "Sample Racer");
        assertEquals(TitleShortcut.SIDE, ic.getWidth());
        assertEquals(TitleArt.monogramColor("58420001"), ic.getPixel(2, 2));
        save(ic, "shortcut-icon-monogram");
    }

    @Test
    public void labels() {
        assertEquals("Halo", TitleShortcut.label("Halo", TitleShortcut.SHORT_MAX));
        assertEquals("Burnout 3…", TitleShortcut.label("Burnout 3: Takedown", TitleShortcut.SHORT_MAX));
        assertEquals("Supercalifr…", TitleShortcut.label("Supercalifragilistic", TitleShortcut.SHORT_MAX));
        assertEquals("Burnout 3: Takedown", TitleShortcut.label("Burnout 3: Takedown", TitleShortcut.LONG_MAX));
        assertEquals("game_4D530004", TitleShortcut.id("4D530004"));
        assertEquals("4D530004", TitleShortcut.titleId("game_4D530004"));
        assertNull(TitleShortcut.titleId("launcher"));
    }
}
