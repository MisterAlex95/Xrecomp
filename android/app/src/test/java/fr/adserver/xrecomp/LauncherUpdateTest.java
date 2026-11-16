package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;

import org.json.JSONArray;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
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
 * XRecomp's own updates in the launcher: the modal at the launch (Install now / Later / Skip), the Updates screen
 * (Settings › General), a build that cannot update itself. Screenshots in build/screenshots/ui-update-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherUpdateTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private Application app;
    private VkDriverIndexTest.FakeHttp http;
    private final List<File> installed = new ArrayList<>();

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        shadowOf(app).grantPermissions(android.Manifest.permission.INTERNET);
        shadowOf(app.getPackageManager()).setCanRequestPackageInstalls(true);
        http = new VkDriverIndexTest.FakeHttp();
        LauncherActivity.updateHttpFactory = c -> http;
        LauncherActivity.resetUpdateOfferForTest();
        AppUpdate.unsupportedHook = c -> null;
        AppUpdate.apkCheck = (c, f) -> null;
        AppUpdate.installer = (c, f, v) -> installed.add(f);
    }

    @After
    public void tearDown() {
        LauncherActivity.updateHttpFactory = null;
        AppUpdate.unsupportedHook = null;
        AppUpdate.apkCheck = AppUpdate::checkApk;
        AppUpdate.installer = AppUpdate::install;
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static void key(LauncherActivity a, int code) {
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, code));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, code));
        idle(300);
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

    /** GitHub has a release whose commit is a day newer than this app (version code 1 in the tests). */
    private void serveUpdate() throws Exception {
        byte[] apk = "apk".getBytes();
        String page = new JSONArray().put(AppUpdateTest.release("xrecomp-v0.13.0", false, AppUpdateTest.SHA_REL,
                                                                "2026-11-16T16:36:34Z", "xrecomp-v0.13.0.apk", apk))
                                     .toString();
        String commit = AppUpdateTest.commit("2026-11-16T15:46:04Z");
        http.on(AppUpdateTest.RELEASES, r -> VkDriverIndexTest.resp(200, page.getBytes()));
        http.on(AppUpdate.API + "/git/commits/" + AppUpdateTest.SHA_REL,
                r -> VkDriverIndexTest.resp(200, commit.getBytes()));
        http.on("https://github.com/MisterAlex95/Xrecomp/releases/download/xrecomp-v0.13.0/xrecomp-v0.13.0.apk",
                r -> VkDriverIndexTest.resp(200, apk));
    }

    private LauncherActivity launchAndWait() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        Thread t = a.updateCheckerForTest();
        if (t != null)
            t.join(10_000);
        idle(400);
        return a;
    }

    @Test
    public void launchOffersTheUpdateInAModal() throws Exception {
        serveUpdate();
        LauncherActivity a = launchAndWait();
        Widgets.Sheet s = a.sheetForTest();
        assertNotNull("the modal is open", s);
        List<Widgets.MenuRow> r = s.rowsForTest();
        assertEquals(3, r.size());
        assertEquals(app.getString(R.string.UPD_INSTALL_NOW), r.get(0).title);
        assertEquals(app.getString(R.string.UPD_LATER), r.get(1).title);
        assertEquals(app.getString(R.string.UPD_SKIP), r.get(2).title);
        TestSupport.screenshot(a.getWindow().getDecorView(), "ui-update-modal");
        // Install now: downloaded, then handed to the installer.
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        for (int i = 0; i < 50 && installed.isEmpty(); i++)
            idle(100);
        assertEquals(1, installed.size());
        idle(500);
        assertEquals(app.getString(R.string.UPD_HANDED, "0.13.0"), a.noteForTest());
    }

    @Test
    public void laterAndSkip() throws Exception {
        serveUpdate();
        LauncherActivity a = launchAndWait();
        assertNotNull(a.sheetForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);   // Later (B closes the modal)
        assertNull(a.sheetForTest());
        // Not offered twice in one run.
        a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(1000);
        assertNull("once per run", a.sheetForTest());
        // Skip this version: never offered again, even in a new run.
        LauncherActivity.resetUpdateOfferForTest();
        a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(1000);
        assertNotNull(a.sheetForTest());
        a.sheetForTest().rowsForTest().get(2).performClick();
        idle(300);
        assertNull(a.sheetForTest());
        assertEquals(app.getString(R.string.UPD_SKIPPED, "0.13.0"), a.noteForTest());
        LauncherActivity.resetUpdateOfferForTest();
        a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(1000);
        assertNull("skipped", a.sheetForTest());
    }

    @Test
    public void updatesScreen() throws Exception {
        serveUpdate();
        LauncherActivity a = launchAndWait();
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        Widgets.MenuRow u = row(a, app.getString(R.string.UPD_TITLE));
        assertNotNull("Settings › General › Updates", u);
        assertEquals(app.getString(R.string.UPD_AVAILABLE, "0.13.0"), u.value);
        a.goForTest(LauncherActivity.Screen.UPDATE);
        idle(300);
        assertNotNull(row(a, app.getString(R.string.UPD_INSTALL, "0.13.0")));
        assertNotNull(row(a, app.getString(R.string.UPD_CHECK_NOW)));
        assertEquals(app.getString(R.string.VAL_ON), row(a, app.getString(R.string.UPD_AUTO)).value);
        assertEquals(app.getString(R.string.UPD_CHANNEL_LATEST), row(a, app.getString(R.string.UPD_CHANNEL)).value);
        TestSupport.screenshot(a.getWindow().getDecorView(), "ui-update-screen");
    }

    @Test
    public void buildSignedByAnotherKeySaysSo() throws Exception {
        AppUpdate.unsupportedHook = c -> AppUpdate.Unsupported.OTHER_SIGNER;
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        assertNull("no check, no modal", a.updateCheckerForTest());
        assertTrue(http.requests.isEmpty());
        a.goForTest(LauncherActivity.Screen.UPDATE);
        idle(300);
        Widgets.MenuRow r = row(a, app.getString(R.string.UPD_UNAVAILABLE));
        assertNotNull(r);
        assertEquals(app.getString(R.string.UPD_UNSUPPORTED_SIGNER), r.description);
    }
}
