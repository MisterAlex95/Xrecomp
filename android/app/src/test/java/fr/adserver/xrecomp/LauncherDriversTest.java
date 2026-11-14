package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

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

import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * Settings › Graphics › Vulkan driver and the Drivers screen (WP5): the row's value, the screen's rows (system,
 * installed, to download from a fake GitHub, refresh, import), choosing and deleting with the pad, the offline and
 * limit states with the cached list. Screenshots in build/screenshots/ui-drivers-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherDriversTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private VkDriverIndexTest.FakeHttp http;
    private long now = java.time.Instant.parse("2026-11-14T10:00:00Z").toEpochMilli();

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        shadowOf(app).grantPermissions(android.Manifest.permission.INTERNET);
        http = new VkDriverIndexTest.FakeHttp();
        File cache = tmp.newFolder("cache");
        LauncherActivity.vkdIndexFactory = c -> new VkDriverIndex(
                new File(c.getFilesDir(), "vk_drivers/index"), cache, http, () -> now, true, VkDriverIndex.SOURCES);
    }

    @After
    public void tearDown() {
        LauncherActivity.vkdIndexFactory = null;
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

    private static Widgets.MenuRow rowStarting(LauncherActivity a, String prefix) {
        for (Widgets.MenuRow r : rows(a.getWindow().getDecorView(), new ArrayList<>()))
            if (r.title.startsWith(prefix))
                return r;
        return null;
    }

    private VkDrivers drivers() {
        return VkDrivers.of(app);
    }

    private VkDrivers.Driver install() throws Exception {
        return drivers().install(new ByteArrayInputStream(VkDriversTest.zip(VkDriversTest.good())),
                                 VkDrivers.Origin.manual());
    }

    /** Both sources answer with their recorded releases pages. */
    private void serveLists() throws Exception {
        String p = VkDriverIndexTest.fixture("purple-turnip.json"), k = VkDriverIndexTest.fixture("AdrenoToolsDrivers.json");
        http.on(VkDriverIndex.SOURCES.get(0).releasesUrl(),
                r -> VkDriverIndexTest.resp(200, p.getBytes(StandardCharsets.UTF_8), "ETag", "\"p\""));
        http.on(VkDriverIndex.SOURCES.get(1).releasesUrl(),
                r -> VkDriverIndexTest.resp(200, k.getBytes(StandardCharsets.UTF_8), "ETag", "\"k\""));
    }

    private void waitList(LauncherActivity a) throws Exception {
        Thread t = a.vkdRefresherForTest();
        if (t != null)
            t.join(10_000);
        idle(400);
    }

    private LauncherActivity openGraphics() {
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_R1);
        assertEquals(LauncherActivity.TAB_GRAPHICS, a.settingsTabForTest());
        return a;
    }

    // ---- Settings › Graphics -----------------------------------------------------------------------------------

    @Test
    public void graphicsRowShowsTheActiveDriver() throws Exception {
        LauncherActivity a = openGraphics();
        Widgets.MenuRow r = row(a, app.getString(R.string.OPT_VK_DRIVER));
        assertNotNull("Settings › Graphics › Vulkan driver", r);
        assertEquals(app.getString(R.string.VKD_SYSTEM_SHORT), r.value);
        assertTrue("says it is the N64 renderer for now", r.description.contains("Nintendo 64"));

        VkDrivers.Driver d = install();
        drivers().setActive(d.id);
        a = openGraphics();
        r = row(a, app.getString(R.string.OPT_VK_DRIVER));
        assertEquals("Turnip Test T1 (26.3.0-test)", r.value);
        // The last start failed: the description says so.
        Files.write(new File(drivers().root(), VkDrivers.LAST_START).toPath(),
                    (d.id + " not_loaded\n").getBytes(StandardCharsets.UTF_8));
        a = openGraphics();
        r = row(a, app.getString(R.string.OPT_VK_DRIVER));
        r.requestFocusFromTouch();
        idle(200);
        assertTrue(a.descriptionForTest().contains(app.getString(R.string.VKD_LAST_NOT_LOADED)));
        // A opens the Drivers screen.
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(LauncherActivity.Screen.DRIVERS, a.screenForTest());
        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertEquals(LauncherActivity.Screen.SETTINGS, a.screenForTest());
        assertEquals("back on Graphics", LauncherActivity.TAB_GRAPHICS, a.settingsTabForTest());
    }

    /** The row ends « All consoles » (before « Xbox only »), and its description says a game can override it. */
    @Test
    public void graphicsRowIsInAllConsoles() throws Exception {
        LauncherActivity a = openGraphics();
        List<Widgets.MenuRow> all = rows(a.getWindow().getDecorView(), new ArrayList<>());
        Widgets.MenuRow vk = row(a, app.getString(R.string.OPT_VK_DRIVER));
        assertNotNull(vk);
        int vkAt = all.indexOf(vk), firstXbox = -1, lastShared = -1;
        for (RecompConfig.Option o : RecompConfig.tabOptions(RecompConfig.shownOptions(false),
                                                             RecompConfig.Section.GRAPHICS)) {
            Widgets.MenuRow r = row(a, app.getString(o.title));
            if (r == null)
                continue;
            if (o.shared)
                lastShared = Math.max(lastShared, all.indexOf(r));
            else if (firstXbox < 0)
                firstXbox = all.indexOf(r);
        }
        assertTrue("after the shared options", vkAt > lastShared);
        assertTrue("before the Xbox-only options", firstXbox < 0 || vkAt < firstXbox);
        assertTrue(vk.description, vk.description.contains("Options"));
    }

    // ---- The Drivers screen ------------------------------------------------------------------------------------

    @Test
    public void driversScreenListsSelectsAndDeletes() throws Exception {
        VkDrivers.Driver d = install();
        serveLists();
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.DRIVERS);
        idle(300);
        // No copy yet: the list is asked once by itself.
        waitList(a);
        assertEquals(2, http.requests.size());
        Widgets.MenuRow sys = row(a, app.getString(R.string.VKD_SYSTEM));
        assertNotNull(sys);
        assertEquals(app.getString(R.string.VKD_ACTIVE), sys.badge);
        Widgets.MenuRow inst = row(a, d.name);
        assertNotNull(inst);
        assertEquals("26.3.0-test", inst.value);
        assertNull(inst.badge);
        Widgets.MenuRow t30 = rowStarting(a, "turnip_mrpurple_T30");
        assertNotNull("T30 from purple-turnip listed", t30);
        assertEquals(android.text.format.Formatter.formatShortFileSize(app, 3_713_730), t30.value);
        Widgets.MenuRow refresh = row(a, app.getString(R.string.VKD_REFRESH));
        assertTrue(refresh.value, refresh.value.startsWith(app.getString(R.string.VKD_LIST_FROM, "").trim()));
        assertNotNull(row(a, app.getString(R.string.VKD_IMPORT)));
        for (Widgets.MenuRow r : rows(a.getWindow().getDecorView(), new ArrayList<>())) {
            assertFalse("no Qualcomm build listed: " + r.title, r.title.toLowerCase().contains("qualcomm"));
            assertFalse("no .zip / .adpkg in a name: " + r.title, r.title.contains(".zip") || r.title.contains(".adpkg"));
        }
        TestSupport.screenshot(a.getWindow().getDecorView(), "ui-drivers-en-1080");

        // A on the installed driver: it becomes the active one.
        inst.requestFocusFromTouch();
        idle(200);
        assertTrue(a.descriptionForTest().contains(app.getString(R.string.VKD_ORIGIN_MANUAL)));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals(d.id, drivers().activeId());
        assertEquals(app.getString(R.string.VKD_ACTIVE), row(a, d.name).badge);
        assertNull(row(a, app.getString(R.string.VKD_SYSTEM)).badge);
        assertEquals("the focus stays on it", d.name, ((Widgets.MenuRow)a.focusForTest()).title);

        // X on the system row does nothing but say how; X twice on the driver deletes it, back to the system.
        row(a, app.getString(R.string.VKD_SYSTEM)).requestFocusFromTouch();
        idle(200);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals(app.getString(R.string.VKD_DELETE_WHICH), a.noteForTest());
        row(a, d.name).requestFocusFromTouch();
        idle(200);
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertEquals(app.getString(R.string.VKD_DELETE_BADGE), row(a, d.name).badge);
        assertNotNull(drivers().read(d.id));
        key(a, KeyEvent.KEYCODE_BUTTON_X);
        assertNull(drivers().read(d.id));
        assertNull(drivers().activeId());
        assertNull(row(a, d.name));
        assertEquals(app.getString(R.string.VKD_ACTIVE), row(a, app.getString(R.string.VKD_SYSTEM)).badge);

        // A on the system row: chosen again.
        drivers();
        key(a, KeyEvent.KEYCODE_BUTTON_B);
    }

    @Test
    public void offlineAndLimitKeepTheCachedList() throws Exception {
        serveLists();
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.DRIVERS);
        idle(300);
        waitList(a);
        assertNotNull(rowStarting(a, "turnip_mrpurple_T30"));

        // Y: Refresh, offline: the cached list stays, the state says why.
        http.on(VkDriverIndex.SOURCES.get(0).releasesUrl(), r -> {
            throw new java.net.UnknownHostException("api.github.com");
        });
        http.on(VkDriverIndex.SOURCES.get(1).releasesUrl(), r -> {
            throw new java.net.UnknownHostException("api.github.com");
        });
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        waitList(a);
        assertNotNull("cached list kept", rowStarting(a, "turnip_mrpurple_T30"));
        Widgets.MenuRow refresh = row(a, app.getString(R.string.VKD_REFRESH));
        assertEquals(app.getString(R.string.VKD_LIST_OFFLINE), refresh.value);
        assertTrue(a.noteForTest().startsWith(app.getString(R.string.VKD_LIST_OFFLINE)));

        // GitHub's limit: said, with the cached list.
        long reset = now / 1000 + 1800;
        for (VkDriverIndex.Source s : VkDriverIndex.SOURCES)
            http.on(s.releasesUrl(), r -> VkDriverIndexTest.resp(403, "{}".getBytes(StandardCharsets.UTF_8),
                                                                  "x-ratelimit-remaining", "0",
                                                                  "x-ratelimit-reset", "" + reset));
        key(a, KeyEvent.KEYCODE_BUTTON_Y);
        waitList(a);
        refresh = row(a, app.getString(R.string.VKD_REFRESH));
        assertEquals("short value", app.getString(R.string.VKD_LIST_LIMITED), refresh.value);
        refresh.requestFocusFromTouch();
        idle(200);
        assertTrue(a.descriptionForTest(), a.descriptionForTest().contains(
                app.getString(R.string.VKD_LIST_LIMITED_UNTIL, VkDriverStore.time(reset * 1000))));
        assertNotNull(rowStarting(a, "turnip_mrpurple_T30"));
        TestSupport.screenshot(a.getWindow().getDecorView(), "ui-drivers-limited-en-1080");

        RuntimeEnvironment.setQualifiers("fr-w1920dp-h1080dp-land-mdpi");
        a = launch();
        a.goForTest(LauncherActivity.Screen.DRIVERS);
        idle(300);
        TestSupport.screenshot(a.getWindow().getDecorView(), "ui-drivers-fr-1080");
        assertNotNull(row(a, app.getString(R.string.VKD_SYSTEM)));
    }

    @Test
    public void noListAndNoNetworkStillShowsInstalledAndImport() throws Exception {
        VkDrivers.Driver d = install();
        for (VkDriverIndex.Source s : VkDriverIndex.SOURCES)
            http.on(s.releasesUrl(), r -> {
                throw new java.net.SocketTimeoutException("timeout");
            });
        LauncherActivity a = launch();
        a.goForTest(LauncherActivity.Screen.DRIVERS);
        idle(300);
        waitList(a);
        assertNotNull(row(a, app.getString(R.string.VKD_SYSTEM)));
        assertNotNull(row(a, d.name));
        assertNotNull(row(a, app.getString(R.string.VKD_IMPORT)));
        assertEquals(app.getString(R.string.VKD_LIST_OFFLINE), row(a, app.getString(R.string.VKD_REFRESH)).value);
    }

    /** Every new string has its French. */
    @Test
    public void frenchForEveryDriverString() throws Exception {
        String en = new String(Files.readAllBytes(new File("src/main/res/values/strings.xml").toPath()),
                               StandardCharsets.UTF_8);
        String fr = new String(Files.readAllBytes(new File("src/main/res/values-fr/strings.xml").toPath()),
                               StandardCharsets.UTF_8);
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("name=\"((?:VKD|OPT_VK)_[A-Z_]+)\"").matcher(en);
        int n = 0;
        while (m.find()) {
            n++;
            assertTrue(m.group(1) + " in French", fr.contains("name=\"" + m.group(1) + "\""));
        }
        assertTrue(n > 40);
    }
}
