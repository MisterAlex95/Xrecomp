package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.graphics.Bitmap;
import android.os.Looper;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

/** ArtCache: the sample size, the decode at card size, the byte bound, misses, stale answers while reloading. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ArtCacheTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    /** Runs nothing until run(): a load in flight, as on the executor. */
    private final List<Runnable> queued = new ArrayList<>();

    @Before
    public void setUp() {
        ArtCache.clear();
        ArtCache.setExecutorForTest(Runnable::run);
    }

    @After
    public void tearDown() {
        ArtCache.setExecutorForTest(null);
        ArtCache.clear();
    }

    private void runQueued() {
        List<Runnable> l = new ArrayList<>(queued);
        queued.clear();
        for (Runnable r : l)
            r.run();
        shadowOf(Looper.getMainLooper()).idle();
    }

    private static Bitmap bitmap(int w, int h) {
        return Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    }

    @Test
    public void sampleSizeKeepsTheShorterSideAtLeastTheCard() {
        assertEquals(1, ArtCache.sampleSize(512, 384, 288));
        assertEquals(2, ArtCache.sampleSize(1024, 768, 288));
        assertEquals(4, ArtCache.sampleSize(2048, 1536, 288));
        assertEquals(2, ArtCache.sampleSize(768, 1024, 288));   // portrait box art: the width is the shorter side
        assertEquals(1, ArtCache.sampleSize(128, 128, 288));   // a title image smaller than the card: whole
        assertEquals(2, ArtCache.sampleSize(1024, 768, 384));
        assertEquals(1, ArtCache.sampleSize(1024, 768, 385));
        assertEquals(1, ArtCache.sampleSize(0, 0, 288));
    }

    @Test
    public void decodesAtCardSize() throws Exception {
        File f = tmp.newFile("cover.png");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        bitmap(1024, 768).compress(Bitmap.CompressFormat.PNG, 100, out);
        TestSupport.write(f, out.toByteArray());
        Bitmap b = ArtCache.decode(f, 288);
        assertEquals(512, b.getWidth());
        assertEquals(384, b.getHeight());
        assertEquals(1024, ArtCache.decode(f, 800).getWidth());
        assertNull(ArtCache.decode(new File(tmp.getRoot(), "none.png"), 288));
        File junk = tmp.newFile("junk.png");
        TestSupport.write(junk, new byte[] { 1, 2, 3 });
        assertNull(ArtCache.decode(junk, 288));
    }

    @Test
    public void staysUnderItsByteBound() {
        int each = 512 * 512 * 4, n = ArtCache.maxBytes() / each * 3;
        for (int i = 0; i < n; i++)
            assertNotNull(ArtCache.get("k" + i, px -> bitmap(512, 512), 288, null));
        assertTrue(ArtCache.bytes() <= ArtCache.maxBytes());
        assertTrue(ArtCache.maxBytes() <= ArtCache.MAX_BYTES);
        assertEquals(ArtCache.maxBytes() / each, ArtCache.countForTest());
        AtomicInteger loads = new AtomicInteger();
        ArtCache.get("k" + (n - 1), px -> { loads.incrementAndGet(); return bitmap(1, 1); }, 288, null);
        assertEquals("the latest is still there", 0, loads.get());
        ArtCache.get("k0", px -> { loads.incrementAndGet(); return bitmap(1, 1); }, 288, null);
        assertEquals("the oldest was evicted", 1, loads.get());
    }

    @Test
    public void missIsRememberedUntilInvalidated() {
        AtomicInteger loads = new AtomicInteger();
        ArtCache.Loader none = px -> {
            loads.incrementAndGet();
            return null;
        };
        assertNull(ArtCache.get("gba-x", none, 288, null));
        assertNull(ArtCache.get("gba-x", none, 288, null));
        assertEquals(1, loads.get());
        ArtCache.forgetMisses();
        assertNull(ArtCache.get("gba-x", none, 288, null));
        assertEquals(2, loads.get());
        ArtCache.invalidate("gba-x");
        Bitmap cover = bitmap(4, 4);
        assertSame("a cover arrived", cover, ArtCache.get("gba-x", px -> cover, 288, null));
    }

    @Test
    public void loadsOffTheCallerOnceAndAnswersTheStaleBitmapMeanwhile() {
        ArtCache.setExecutorForTest(queued::add);
        Bitmap first = bitmap(4, 4), second = bitmap(8, 8);
        List<Bitmap> called = new ArrayList<>();
        assertNull(ArtCache.get("ps1-a", px -> first, 288, called::add));
        assertNull(ArtCache.get("ps1-a", px -> first, 288, called::add));
        assertEquals("one load per key in flight", 1, queued.size());
        runQueued();
        assertEquals(2, called.size());
        assertSame(first, called.get(0));
        assertSame(first, ArtCache.get("ps1-a", px -> first, 288, null));
        assertEquals(0, queued.size());

        ArtCache.invalidate("ps1-a");
        assertSame("the old cover until the new one has loaded", first,
                   ArtCache.get("ps1-a", px -> second, 288, null));
        runQueued();
        assertSame(second, ArtCache.get("ps1-a", px -> second, 288, null));

        // A larger side than the one cached (Home's tile after a card) loads again, the smaller one answering meanwhile.
        assertSame(second, ArtCache.get("ps1-a", px -> first, 400, null));
        assertEquals(1, queued.size());
        runQueued();
        assertSame(first, ArtCache.get("ps1-a", px -> second, 288, null));
        assertEquals(0, queued.size());

        ArtCache.invalidateAll();   // a new activity: loaded again when next asked
        assertSame(first, ArtCache.get("ps1-a", px -> second, 288, null));
        assertEquals(1, queued.size());
    }

    /**
     * The library's rows do not clip, so every card is drawn: only those near the ScrollView's viewport ask for their
     * art, the others when scrolled to (report-launcher-lag RC2: 1 600 covers decoded at each launch before).
     */
    @Test
    public void onlyCardsNearTheViewportLoad() {
        android.content.Context c = org.robolectric.RuntimeEnvironment.getApplication();
        android.widget.ScrollView scroll = LibraryScreen.scroll(c);
        android.widget.LinearLayout col = (android.widget.LinearLayout)scroll.getChildAt(0);
        List<String> loaded = new ArrayList<>();
        List<GameCard> cards = new ArrayList<>();
        for (int i = 0; i < 60; i++) {
            String key = "gba-" + i;
            cards.add(new GameCard(c).setGame(key, px -> {
                loaded.add(key);
                return bitmap(8, 8);
            }, key, "Game " + i, "", 0));
        }
        LibraryScreen.addRows(c, col, cards, 5);   // 12 rows of 416 px
        int w = 5 * GameCard.W, h = 2 * GameCard.H;
        scroll.measure(android.view.View.MeasureSpec.makeMeasureSpec(w, android.view.View.MeasureSpec.EXACTLY),
                       android.view.View.MeasureSpec.makeMeasureSpec(h, android.view.View.MeasureSpec.EXACTLY));
        scroll.layout(0, 0, w, h);
        android.graphics.Canvas canvas = new android.graphics.Canvas(bitmap(w, h));
        scroll.draw(canvas);
        assertEquals("the two rows shown and one past them", 15, loaded.size());
        assertTrue(loaded.contains("gba-0") && !loaded.contains("gba-15"));
        assertTrue(cards.get(0).hasArtForTest());

        scroll.scrollTo(0, 8 * GameCard.H);
        shadowOf(Looper.getMainLooper()).idle();
        scroll.draw(canvas);
        assertTrue("scrolled to: rows 7 to 10", loaded.contains("gba-40") && loaded.contains("gba-35"));
        assertTrue(!loaded.contains("gba-20"));
        assertTrue(ArtCache.countForTest() < 60);
    }

    @Test
    public void watchingViewRepaintsWhenItsArtLoads() {
        ArtCache.setExecutorForTest(queued::add);
        AtomicInteger inval = new AtomicInteger();
        android.view.View v = new android.view.View(org.robolectric.RuntimeEnvironment.getApplication()) {
            @Override
            public void invalidate() {
                inval.incrementAndGet();
                super.invalidate();
            }
        };
        ArtCache.watch("gc-b", v);
        ArtCache.get("gc-b", px -> bitmap(2, 2), 288, null);
        runQueued();
        assertEquals(1, inval.get());
        ArtCache.invalidate("gc-b");
        assertEquals("a cover arrived: the card asks again", 2, inval.get());
        ArtCache.unwatch("gc-b", v);
        ArtCache.invalidate("gc-b");
        assertEquals(2, inval.get());
    }
}
