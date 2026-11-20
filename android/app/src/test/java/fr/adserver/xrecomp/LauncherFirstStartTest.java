package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
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
import org.robolectric.shadows.ShadowActivity;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.List;

/**
 * XRecomp's first start (SETUP: library folder, the games found, a few settings, ready), Help and questions, and
 * Home's « Good to know » bubbles. Screenshots build/screenshots/first-<step>-<en|fr>-<1080|4x3|1x1>, help-*, tip-*.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherFirstStartTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String[] LOCALES = { "en", "fr" };
    private static final String[][] SIZES = { { "1080", "w1920dp-h1080dp-land-mdpi" },
                                              { "4x3", "w1440dp-h1080dp-land-mdpi" },
                                              { "1x1", "w1080dp-h1080dp-land-mdpi" } };
    private static final String TREE = "content://com.android.externalstorage.documents/tree/primary%3AGames";

    @Before
    public void setUp() {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        LauncherActivity.guidanceOn = true;
    }

    @After
    public void tearDown() {
        LauncherActivity.guidanceOn = false;
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
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
        if (a.screenForTest() == LauncherActivity.Screen.DEMO) {   // the animation first: skipped (demo tests below)
            key(a, KeyEvent.KEYCODE_BUTTON_A);
            idle(300);
        }
        return a;
    }

    private void withToolchain() throws Exception {
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder(), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.newFolder()).getAbsolutePath();
    }

    /** /sdcard/Games with a GBA and an N64 game (and an Xbox folder when xbox). */
    private File games(boolean any) throws Exception {
        File root = new File(Environment.getExternalStorageDirectory(), "Games");
        root.mkdirs();
        if (any) {
            TestSupport.write(new File(root, "GBA/Pocket Quest.gba"), ConsoleTest.gba("AXVE"));
            TestSupport.write(new File(root, "N64/Star Race.z64"), ConsoleTest.n64(0));
            TestSupport.write(new File(root, "Xbox/Sample Racer/default.xbe"),
                              TestSupport.xbe(0x4D530004, "Sample Racer"));
        }
        return root;
    }

    /** The folder picker answers with /sdcard/Games, then the scan runs to its end. */
    private void pickFolder(LauncherActivity a) throws Exception {
        ShadowActivity.IntentForResult r;
        ShadowActivity.IntentForResult last = null;
        while ((r = shadowOf(a).getNextStartedActivityForResult()) != null)
            last = r;
        assertNotNull("the folder picker", last);
        assertEquals(Intent.ACTION_OPEN_DOCUMENT_TREE, last.intent.getAction());
        shadowOf(a).receiveResult(last.intent, Activity.RESULT_OK, new Intent().setData(Uri.parse(TREE)));
        idle(300);
        Thread t = a.scannerForTest();
        if (t != null)
            t.join(10_000);
        idle(500);
    }

    private void press(LauncherActivity a, String label) {
        java.util.List<View> all = new java.util.ArrayList<>(a.focusablesForTest());
        if (a.sheetForTest() != null)
            all.addAll(a.sheetForTest().rowsForTest());
        for (View v : all)
            if (v instanceof Widgets.PillButton && label.equals(((Widgets.PillButton)v).title)
                || v instanceof Widgets.MenuRow && label.equals(((Widgets.MenuRow)v).title)) {
                v.performClick();
                idle(400);
                return;
            }
        throw new AssertionError("no « " + label + " » on " + a.screenForTest());
    }

    @Test
    public void aFreshStartWalksTheSetupToTheLibrary() throws Exception {
        games(true);
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.SETUP, a.screenForTest());
        assertEquals(0, a.setupStep());
        press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
        pickFolder(a);
        assertEquals(LauncherActivity.Screen.SETUP, a.screenForTest());
        assertEquals(1, a.setupStep());
        assertEquals(3, a.libraryCountForTest());
        press(a, app.getString(R.string.SETUP_CONTINUE));
        assertEquals(2, a.setupStep());
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(3, a.setupStep());
        press(a, app.getString(R.string.SETUP_OPEN_LIBRARY));
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(LauncherActivity.Screen.LIBRARY, launch().screenForTest());   // done: never again
    }

    @Test
    public void aRecreatedActivityResumesTheSetupWhereItWas() throws Exception {
        games(true);
        LauncherActivity a = launch();
        press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
        pickFolder(a);
        assertEquals(1, a.setupStep());
        LauncherActivity again = launch();   // the library is on now: still the setup, at its step
        assertEquals(LauncherActivity.Screen.SETUP, again.screenForTest());
        assertEquals(1, again.setupStep());
    }

    @Test
    public void bGoesBackAStep() throws Exception {
        games(true);
        LauncherActivity a = launch();
        press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
        pickFolder(a);
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(0, a.setupStep());
    }

    @Test
    public void anEmptyFolderSaysSoAndOffersAnother() throws Exception {
        games(false);
        LauncherActivity a = launch();
        press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
        pickFolder(a);
        assertEquals(1, a.setupStep());
        assertEquals(0, a.libraryCountForTest());
        press(a, app.getString(R.string.SETUP_OTHER_FOLDER));
        assertEquals(0, a.setupStep());
    }

    /** Games on the network (XBA-319): a .torrent added from step 1 goes on to step 2, its games « On the network ». */
    @Test
    public void aNetworkSourceTakesTheSetupToItsGames() throws Exception {
        File tor = new File(tmp.newFolder(), "mine.torrent");
        TestSupport.write(tor, TorrentMetaTest.torrent("My games", true,
                TorrentMetaTest.files(TorrentMetaTest.file(4L << 30, "Halo Sample (USA).iso"),
                                      TorrentMetaTest.file(2L << 30, "Night Racer.xiso"),
                                      TorrentMetaTest.file(16L << 20, "gba", "Pocket Quest (Europe).gba"))));
        for (String loc : LOCALES) {
            RuntimeEnvironment.setQualifiers(loc + "-" + SIZES[0][1]);
            GameStore.prefs(app).edit().clear().commit();
            for (RemoteLibrary.Source src : RemoteLibrary.sources(app.getFilesDir()))
                RemoteLibrary.remove(app.getFilesDir(), src.id);
            LauncherActivity a = launch();
            assertEquals(0, a.setupStep());
            press(a, app.getString(R.string.SETUP_NETWORK));
            assertNotNull(a.sheetForTest());
            shot(a, "first-network-" + loc + "-1080");
            press(a, app.getString(R.string.NET_ADD_TORRENT));
            ShadowActivity.IntentForResult r, last = null;
            while ((r = shadowOf(a).getNextStartedActivityForResult()) != null)
                last = r;
            assertNotNull(last);
            shadowOf(a).receiveResult(last.intent, Activity.RESULT_OK, new Intent().setData(Uri.fromFile(tor)));
            idle(300);
            press(a, app.getString(R.string.NET_CONSOLE_BY_NAME));
            assertEquals(LauncherActivity.Screen.SETUP, a.screenForTest());
            assertEquals(1, a.setupStep());
            assertEquals(3, a.libraryCountForTest());
            shot(a, "first-2-network-" + loc + "-1080");
        }
    }

    @Test
    public void justOneGameTakesWelcomesRoute() {
        LauncherActivity a = launch();
        press(a, app.getString(R.string.SETUP_ONE_GAME));
        assertEquals(LauncherActivity.Screen.SOURCE, a.screenForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.WELCOME, a.screenForTest());
        assertEquals(LauncherActivity.Screen.WELCOME, launch().screenForTest());   // the setup is over
    }

    /** A fresh start opens on « Translation, live »; its end (or B) goes on to the first start. */
    @Test
    public void theAnimationLeadsToTheFirstStart() throws Exception {
        long[] now = { 0 };
        TranslationDemo.clock = () -> now[0];
        try {
            LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
            idle(300);
            assertEquals(LauncherActivity.Screen.DEMO, a.screenForTest());
            now[0] = TranslationDemo.total() + 10;
            TestSupport.screenshot(a.getWindow().getDecorView(), "demo-end");   // a frame: the end is seen there
            idle(500);
            assertEquals(LauncherActivity.Screen.SETUP, a.screenForTest());
            LauncherActivity b = Robolectric.buildActivity(LauncherActivity.class).setup().get();
            idle(300);
            key(b, KeyEvent.KEYCODE_BUTTON_B);
            assertEquals(LauncherActivity.Screen.SETUP, b.screenForTest());
        } finally {
            TranslationDemo.clock = android.os.SystemClock::uptimeMillis;
        }
    }

    /** Each scene of the animation, half way through it (and the call found, scene 4, late). */
    @Test
    public void animationScreenshots() throws Exception {
        long[] now = { 0 };
        TranslationDemo.clock = () -> now[0];
        try {
            for (String loc : LOCALES) {
                for (String[] size : SIZES) {
                    if (!size[0].equals("1080") && loc.equals("fr"))
                        continue;
                    RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                    now[0] = 0;
                    LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
                    idle(300);
                    assertEquals(LauncherActivity.Screen.DEMO, a.screenForTest());
                    long base = 0;
                    for (int s = 0; s < TranslationDemo.SCENES.length; s++) {
                        long at = s == 3 ? TranslationDemo.SCENES[s] - 300 : s == 4 ? 2000 : TranslationDemo.SCENES[s] - 200;
                        now[0] = base + at;
                        a.demoForTest().invalidate();
                        idle(16);
                        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(),
                                                          "demo-" + (s + 1) + "-" + loc + "-" + size[0]).length() > 0);
                        base += TranslationDemo.SCENES[s];
                    }
                }
            }
        } finally {
            TranslationDemo.clock = android.os.SystemClock::uptimeMillis;
        }
    }

    @Test
    public void offWhenGuidanceIsOff() {
        LauncherActivity.guidanceOn = false;
        assertEquals(LauncherActivity.Screen.WELCOME, launch().screenForTest());
    }

    @Test
    public void settingsOpensHelpAndSelfHealingsAnswerOpensSettings() throws Exception {
        withToolchain();
        GameStore.prefs(app).edit().putBoolean(LauncherActivity.KEY_SETUP_DONE, true).commit();
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        press(a, app.getString(R.string.HELP_TITLE));
        assertEquals(LauncherActivity.Screen.HELP, a.screenForTest());
        assertTrue(a.descriptionForTest().startsWith(app.getString(R.string.HELP_A_PREPARE)));
        press(a, app.getString(R.string.HELP_Q_HEAL));
        assertEquals(LauncherActivity.Screen.SETTINGS, a.screenForTest());
    }

    /** An Xbox game that needs preparing: the « Why prepare? » bubble once; Learn more opens its Help question. */
    @Test
    public void homeShowsEachBubbleOnce() throws Exception {
        withToolchain();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertEquals(LauncherActivity.Tip.PREPARE, a.tipShownForTest());
        assertNotNull(a.sheetForTest());
        press(a, app.getString(R.string.TIP_LEARN_MORE));
        assertNull(a.sheetForTest());
        assertEquals(LauncherActivity.Screen.HELP, a.screenForTest());
        assertTrue(a.descriptionForTest().startsWith(app.getString(R.string.HELP_A_PREPARE)));
        LauncherActivity again = launch();
        assertNull("seen: never again", again.tipShownForTest());
        assertNull(again.sheetForTest());
        assertTrue(LauncherActivity.tipSeen(app, LauncherActivity.Tip.PREPARE));
    }

    @Test
    public void bClosesABubbleForGood() throws Exception {
        withToolchain();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        LauncherActivity a = launch();
        assertNotNull(a.sheetForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertNull(a.sheetForTest());
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        assertNull(launch().sheetForTest());
    }

    @Test
    public void screenshots() throws Exception {
        withToolchain();
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                String sfx = "-" + loc + "-" + size[0];
                GameStore.deleteTree(new File(Environment.getExternalStorageDirectory(), "Games"));
                GameStore.prefs(app).edit().clear().commit();
                Library.setFolders(app, new java.util.ArrayList<>());
                GameStore.deleteTree(Library.dir(app.getFilesDir()));
                games(true);
                LauncherActivity a = launch();
                shot(a, "first-1" + sfx);
                press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
                pickFolder(a);
                shot(a, "first-2" + sfx);
                press(a, app.getString(R.string.SETUP_CONTINUE));
                shot(a, "first-3" + sfx);
                key(a, KeyEvent.KEYCODE_BUTTON_R1);
                shot(a, "first-4" + sfx);
                a.goForTest(LauncherActivity.Screen.HELP);
                idle(300);
                a.forceFocusForTest();
                shot(a, "help" + sfx);
            }
        }
    }

    /** The bubbles on an Xbox game's Home: « Why prepare? » as it comes, « New code found » with its choice. */
    @Test
    public void tipScreenshots() throws Exception {
        withToolchain();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                String sfx = "-" + loc + "-" + size[0];
                GameStore.prefs(app).edit().clear().commit();
                LauncherActivity a = launch();
                assertNotNull(a.sheetForTest());
                shot(a, "tip-prepare" + sfx);
                key(a, KeyEvent.KEYCODE_BUTTON_B);
                a.showTipForTest(LauncherActivity.Tip.NEW_CODE, true);
                idle(400);
                assertNotNull(a.sheetForTest());
                shot(a, "tip-new_code" + sfx);
            }
        }
    }

    /** Help's six answers with their drawings; an Xbox game's Home with its track and the addresses its runs found. */
    @Test
    public void helpAndTrackScreenshots() throws Exception {
        withToolchain();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        TestSupport.writePack(app.getFilesDir(), "4D530004", 1 << 20);
        GameStore.sessionStart(app, System.currentTimeMillis() - 40 * 60_000L);
        GameStore.sessionEnd(app, System.currentTimeMillis() - 2 * 60_000L);
        SelfHeal.State st = new SelfHeal.State();
        st.offer = SelfHeal.Offer.IMPROVE;
        st.seeds = 14;
        SelfHeal.write(new File(new File(app.getFilesDir(), "work"), "4D530004"), st);
        String[][] sizes = { SIZES[0], SIZES[2] };
        for (String loc : LOCALES) {
            for (String[] size : sizes) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                String sfx = "-" + loc + "-" + size[0];
                GameStore.prefs(app).edit().clear().commit();
                SelfHeal.setAuto(app, false);
                for (LauncherActivity.Tip t : LauncherActivity.Tip.values())
                    GameStore.prefs(app).edit().putBoolean(t.key(), true).commit();
                LauncherActivity a = launch();
                assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
                shot(a, "home-track" + sfx);
                a.goForTest(LauncherActivity.Screen.HELP);
                idle(300);
                for (int i = 0; i < LauncherActivity.HELP.length; i++) {
                    a.focusablesForTest().get(i).requestFocus();
                    idle(300);
                    shot(a, "help-" + (i + 1) + sfx);
                }
            }
        }
    }

    /** A second display (AYN Thor): the first start's and Help's drawings there, with their touch actions. */
    @Test
    public void theBottomScreenDrawsTheStepAndTheQuestion() throws Exception {
        games(true);
        org.robolectric.shadows.ShadowDisplayManager.addDisplay("w1240dp-h1080dp-mdpi");
        idle(300);
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.SETUP, a.screenForTest());
        CompanionScreen screen = a.companionForTest().screen();
        assertTrue(screen.isShowing());
        assertTrue(a.companionForTest().guideForTest());
        assertTrue(screen.content() instanceof GuidePanel);
        assertEquals(Schema.Kind.PIPE, ((GuidePanel)screen.content()).kindForTest());
        TestSupport.screenshot(screen.content(), "dual-first-1");
        press(a, app.getString(R.string.SETUP_CHOOSE_FOLDER));
        pickFolder(a);
        TestSupport.screenshot(a.companionForTest().screen().content(), "dual-first-2");
        GameStore.prefs(a).edit().putBoolean(LauncherActivity.KEY_SETUP_DONE, true).commit();
        a.goForTest(LauncherActivity.Screen.HELP);
        idle(300);
        for (int i = 0; i < LauncherActivity.HELP_AGAIN; i++) {   // the bottom screen's › twice
            GuidePanel g = (GuidePanel)a.companionForTest().screen().content();
            g.onAction(g.button(GuidePanel.RIGHT));
            idle(300);
        }
        assertTrue(a.descriptionForTest(), a.descriptionForTest().startsWith(app.getString(R.string.HELP_A_AGAIN)));
        View c = a.companionForTest().screen().content();
        assertTrue(c instanceof GuidePanel);
        assertEquals(Schema.Kind.LOOP, ((GuidePanel)c).kindForTest());
        TestSupport.screenshot(c, "dual-help-3");
        a.goForTest(LauncherActivity.Screen.LIBRARY);
        idle(300);
        assertFalse(a.companionForTest().guideForTest());
        assertFalse(a.companionForTest().screen().content() instanceof GuidePanel);
    }

    /** A game's journal: a prepared game gets its first line from pack.json, then what PrepareService and runs add. */
    @Test
    public void theJournalListsWhatHappenedNewestFirst() throws Exception {
        withToolchain();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        TestSupport.writePack(app.getFilesDir(), "4D530004", 1 << 20);
        File files = app.getFilesDir();
        long day = 24 * 3600_000L, t0 = System.currentTimeMillis() - 3 * day;
        List<GameJournal.Event> seeded = GameJournal.read(files, "4D530004");
        assertEquals(1, seeded.size());
        assertEquals(GameJournal.PREPARED, seeded.get(0).kind);
        assertTrue(seeded.get(0).o.optBoolean("seeded"));
        GameJournal.add(files, "4D530004", GameJournal.FOUND, t0 + day, "n", 37, "crash", false);
        GameJournal.add(files, "4D530004", GameJournal.PREPARED, t0 + day + 3600_000L, "total_s", 252.0, "files", 160,
                        "compiled", 6, "background", true);
        GameJournal.add(files, "4D530004", GameJournal.FOUND, t0 + 2 * day, "n", 2, "crash", true);
        assertEquals(GameJournal.FOUND, GameJournal.read(files, "4D530004").get(0).kind);
        for (LauncherActivity.Tip tip : LauncherActivity.Tip.values())
            GameStore.prefs(app).edit().putBoolean(tip.key(), true).commit();
        for (String loc : LOCALES) {
            RuntimeEnvironment.setQualifiers(loc + "-" + SIZES[0][1]);
            LauncherActivity a = launch();
            a.openJournalForTest("4D530004", "Sample Game");
            idle(300);
            assertEquals(LauncherActivity.Screen.JOURNAL, a.screenForTest());
            assertEquals(4, a.focusablesForTest().size());
            shot(a, "journal-" + loc + "-1080");
        }
    }

    private void shot(LauncherActivity a, String name) throws Exception {
        if (a.focusForTest() == null)
            a.forceFocusForTest();
        idle(400);
        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), name).length() > 0);
    }
}
