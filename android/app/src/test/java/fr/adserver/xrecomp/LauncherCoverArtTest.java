package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.os.Looper;
import android.widget.ScrollView;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.IOException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Cover art of a big network library (report launcher-lag RC1, RC4): a cover found shows on its card without the
 * library being built again, and the process has one cover worker that asks for each URL once, however many launcher
 * activities came and went. No network: a stand-in serves the covers.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherCoverArtTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static final int GAMES = 500, COVERS = 12;

    private Application app;

    /** 200 with a PNG for the URLs in found, 404 for the rest; each request counted; held while gate is closed. */
    static final class Http implements VkDriverIndex.Http {
        final Map<String, byte[]> found = new HashMap<>();
        final Map<String, Integer> asked = new HashMap<>();
        volatile CountDownLatch gate;

        @Override
        public VkDriverIndex.Response get(String url, Map<String, String> headers) throws IOException {
            CountDownLatch g = gate;
            if (g != null) {
                try {
                    g.await(20, TimeUnit.SECONDS);
                } catch (InterruptedException e) {
                    throw new IOException(e);
                }
            }
            synchronized (this) {
                asked.merge(url, 1, Integer::sum);
            }
            byte[] b = found.get(url);
            return VkDriverIndexTest.resp(b == null ? 404 : 200, b == null ? new byte[0] : b);
        }
    }

    private final Http http = new Http();

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE,
                                       android.Manifest.permission.INTERNET);
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        CoverFetcher.resetForTest();
        CoverFetcher.httpFactory = () -> http;
        CoverArt.setDownloadOn(app, true);
        // GAMES Game Boy Advance games on the network; the first COVERS have box art under their file's own name.
        List<Object> files = new ArrayList<>();
        for (int i = 0; i < GAMES; i++) {
            String name = String.format("Cover Game %03d (USA).gba", i);
            files.add(TorrentMetaTest.file(16L << 20, name));
            if (i < COVERS)
                http.found.put(CoverArt.url(Console.GBA, CoverArt.candidates(name).get(0)), CoverArtTest.png());
        }
        RemoteLibrary.addTorrent(app.getFilesDir(), TorrentMetaTest.torrent("Pocket", false, files), Console.GBA, 1);
    }

    @After
    public void tearDown() throws Exception {
        CountDownLatch g = http.gate;
        if (g != null)
            g.countDown();
        CoverFetcher.awaitIdleForTest(20_000);
        CoverFetcher.resetForTest();
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static int withArt(LauncherActivity a) {
        int n = 0;
        for (GameCard c : a.libraryCardsForTest())
            if (c.hasArtForTest())
                n++;
        return n;
    }

    private static int coverThreads() {
        int n = 0;
        for (Thread t : Thread.getAllStackTraces().keySet())
            if (CoverFetcher.THREAD.equals(t.getName()) && t.isAlive())
                n++;
        return n;
    }

    @Test
    public void aCoverShowsOnItsCardWithoutARebuild() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        assertEquals(LauncherActivity.Screen.LIBRARY, a.screenForTest());
        assertEquals(GAMES, a.libraryCardsForTest().size());
        ScrollView before = a.libraryScrollForTest();
        assertNotNull(before);
        assertTrue(CoverFetcher.awaitIdleForTest(30_000));
        idle(300);
        assertSame("the library is not built again for a cover", before, a.libraryScrollForTest());
        assertEquals(COVERS, withArt(a));
        assertEquals(GAMES, a.libraryCardsForTest().size());
    }

    @Test
    public void oneWorkerAndEachUrlOnceAcrossActivities() throws Exception {
        http.gate = new CountDownLatch(1);   // the first request waits: the second launcher comes while it runs
        ActivityController<LauncherActivity> first = Robolectric.buildActivity(LauncherActivity.class).setup();
        idle(300);
        first.pause().stop().destroy();
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        assertEquals(1, coverThreads());
        http.gate.countDown();
        assertTrue(CoverFetcher.awaitIdleForTest(30_000));
        idle(300);
        for (Map.Entry<String, Integer> e : http.asked.entrySet())
            assertEquals(e.getKey(), 1, (int)e.getValue());
        assertTrue(http.asked.size() >= GAMES);   // every game asked (a miss tries several names)
        assertEquals("the resumed launcher shows the covers", COVERS, withArt(a));
        // A third launcher asks for nothing: every game has its cover or its miss.
        int asked = http.asked.size();
        Robolectric.buildActivity(LauncherActivity.class).setup();
        idle(300);
        assertTrue(CoverFetcher.awaitIdleForTest(30_000));
        assertEquals(asked, http.asked.size());
    }
}
