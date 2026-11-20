package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.os.Looper;
import android.view.KeyEvent;

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

/**
 * How it works (the launcher's onboarding): shown once on the first start (before Welcome or the library), B skips
 * it, A walks its pages, Settings › General opens it again. Screenshots build/screenshots/guide-<page>-<en|fr>-<size>.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherGuideTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private static final String[] LOCALES = { "en", "fr" };
    private static final String[][] SIZES = { { "1080", "w1920dp-h1080dp-land-mdpi" },
                                              { "4x3", "w1440dp-h1080dp-land-mdpi" },
                                              { "1x1", "w1080dp-h1080dp-land-mdpi" } };

    @Before
    public void setUp() {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        LauncherActivity.guideOnFirstStart = true;
    }

    @After
    public void tearDown() {
        LauncherActivity.guideOnFirstStart = false;
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
        return a;
    }

    private void forget() {
        GameStore.prefs(app).edit().remove(LauncherActivity.KEY_GUIDE_SEEN).commit();
    }

    @Test
    public void showsOnceOnTheFirstStartAndBSkipsIt() {
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.GUIDE, a.screenForTest());
        assertEquals(0, a.guidePageForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.WELCOME, a.screenForTest());
        assertEquals(LauncherActivity.Screen.WELCOME, launch().screenForTest());   // seen: never again by itself
    }

    @Test
    public void aWalksThePagesThenLandsWhereTheStartWouldHave() {
        LauncherActivity a = launch();
        for (int i = 0; i < LauncherActivity.GUIDE_PAGES.length; i++) {
            assertEquals(LauncherActivity.Screen.GUIDE, a.screenForTest());
            assertEquals(i, a.guidePageForTest());
            a.forceFocusForTest();
            key(a, KeyEvent.KEYCODE_BUTTON_A);
        }
        assertEquals(LauncherActivity.Screen.WELCOME, a.screenForTest());
    }

    @Test
    public void l1AndR1TurnThePages() {
        LauncherActivity a = launch();
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(2, a.guidePageForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_L1);
        assertEquals(1, a.guidePageForTest());
    }

    @Test
    public void theLibraryStartShowsItFirst() throws Exception {
        File root = tmp.newFolder("xbox");
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
        LauncherActivity a = launch();
        assertEquals(LauncherActivity.Screen.GUIDE, a.screenForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
    }

    @Test
    public void offOnTheFirstStartWhenSwitchedOff() {
        LauncherActivity.guideOnFirstStart = false;
        assertEquals(LauncherActivity.Screen.WELCOME, launch().screenForTest());
    }

    @Test
    public void settingsOpensItAgain() {
        LauncherActivity a = launch();
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        a.goForTest(LauncherActivity.Screen.GUIDE);
        idle(300);
        assertEquals(LauncherActivity.Screen.GUIDE, a.screenForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.SETTINGS, a.screenForTest());
    }

    @Test
    public void screenshots() throws Exception {
        for (String loc : LOCALES) {
            for (String[] size : SIZES) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                forget();
                LauncherActivity a = launch();
                for (int p = 0; p < LauncherActivity.GUIDE_PAGES.length; p++) {
                    assertEquals(p, a.guidePageForTest());
                    if (a.focusForTest() == null)
                        a.forceFocusForTest();
                    idle(400);
                    File f = TestSupport.screenshot(a.getWindow().getDecorView(),
                                                    "guide-" + (p + 1) + "-" + loc + "-" + size[0]);
                    assertTrue(f.length() > 0);
                    key(a, KeyEvent.KEYCODE_BUTTON_R1);
                }
            }
        }
    }
}
