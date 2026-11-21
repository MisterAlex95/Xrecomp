package fr.adserver.xrecomp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.view.View;

import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;

/**
 * The launcher's game art (library cards, Home's tile, the bottom screen): decoded off the UI thread, at the size it is
 * drawn, in a cache bounded in bytes and shared by the process (report-launcher-lag, RC2).
 *
 * get() never blocks: it answers with the bitmap or null, and on a miss queues a load on a small executor (one per
 * key at a time). When the load ends, the views that watch the key (a GameCard while attached) are invalidated and the
 * callback, if any, is run, both on the UI thread. A key whose load found nothing is remembered as such, so a game
 * without art costs no file access per draw.
 *
 * invalidate(key) (a cover arrived, a game's files went) and invalidateAll() (a scan ended, a new launcher activity)
 * make the next get() load again; the bitmap already cached is still answered meanwhile, so nothing flashes.
 *
 * Tests run the loads in place (setExecutorForTest(Runnable::run), TestSupport.StockTheme): get() then answers with
 * the bitmap at once.
 */
final class ArtCache {
    private ArtCache() {}

    /** Reads one game's art, on the executor; px: the side, in real pixels, it is drawn at (decode no smaller). */
    interface Loader {
        Bitmap load(int px);
    }

    /** A load queued by get() ended: its bitmap, null for none. On the UI thread. */
    interface Callback {
        void art(Bitmap b);
    }

    /** The cache's size: an eighth of the heap, at most MAX_BYTES (a card's cover is 0.3 to 1 MB). */
    static final int MAX_BYTES = 64 << 20;

    private static final class Art {
        final Bitmap bitmap;
        final int px, gen;

        Art(Bitmap bitmap, int px, int gen) {
            this.bitmap = bitmap;
            this.px = px;
            this.gen = gen;
        }
    }

    static final Executor LOADER = java.util.concurrent.Executors.newFixedThreadPool(2, r -> {
        Thread t = new Thread(r, "xba-art");
        t.setPriority(Thread.NORM_PRIORITY - 1);
        t.setDaemon(true);
        return t;
    });
    private static Executor executor = LOADER;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private static final int maxBytes = (int)Math.min(MAX_BYTES, Runtime.getRuntime().maxMemory() / 8);
    private static final LruCache<String, Art> cache = new LruCache<String, Art>(maxBytes) {
        @Override
        protected int sizeOf(String key, Art a) {
            return a.bitmap.getAllocationByteCount();
        }
    };
    /** Keys whose last load found no art, with the generation it was found in. Guarded by ArtCache.class. */
    private static final Map<String, Integer> none = new HashMap<>();
    /** Keys loading now, with the callbacks to run after. Guarded by ArtCache.class. */
    private static final Map<String, List<Callback>> pending = new HashMap<>();
    /** The views drawing a key (UI thread only). */
    private static final Map<String, Set<View>> watchers = new HashMap<>();
    /** Bumped by invalidateAll(): an entry of an older generation is answered but loaded again. */
    private static int gen;
    private static File boundTo;
    /** The stage's scale (Widgets.Stage): design pixels to real ones. */
    private static float scale = 1;

    /**
     * The bitmap for key, or null; a miss (or an entry made stale by invalidate) queues loader, and cb runs when that
     * load ends (not when the answer is already here). px: the side drawn, in real pixels.
     */
    static Bitmap get(String key, Loader loader, int px, Callback cb) {
        if (key == null || loader == null)
            return null;
        Art a = cache.get(key);
        boolean fresh;
        int g;
        synchronized (ArtCache.class) {
            g = gen;
            Integer miss = none.get(key);
            if (a == null && miss != null && miss == gen)
                return null;   // known to have none
            fresh = a != null && a.gen == gen && a.px >= px;
            if (!fresh) {
                List<Callback> l = pending.get(key);
                if (l != null) {   // loading already
                    if (cb != null)
                        l.add(cb);
                    return a == null ? null : a.bitmap;
                }
                pending.put(key, new ArrayList<>());
            }
        }
        if (fresh)
            return a.bitmap;
        executor.execute(() -> load(key, loader, px, g));
        synchronized (ArtCache.class) {
            List<Callback> l = pending.get(key);
            if (l != null) {   // loading: the stale bitmap, if any, until it ends
                if (cb != null)
                    l.add(cb);
                return a == null ? null : a.bitmap;
            }
        }
        Art now = cache.get(key);   // ended already (in place in tests): its answer
        return now == null ? null : now.bitmap;
    }

    /** The bitmap cached for key (stale or not), or null; never loads. */
    static Bitmap peek(String key) {
        Art a = key == null ? null : cache.get(key);
        return a == null ? null : a.bitmap;
    }

    private static void load(String key, Loader loader, int px, int g) {
        Bitmap b = null;
        try {
            b = loader.load(px);
        } catch (RuntimeException | OutOfMemoryError e) {
            android.util.Log.w("xba", "art: " + key + ": " + e);
        }
        List<Callback> cbs;
        synchronized (ArtCache.class) {
            if (b != null) {
                none.remove(key);
                cache.put(key, new Art(b, px, g));
            } else {
                cache.remove(key);
                none.put(key, g);
            }
            cbs = pending.remove(key);
        }
        Bitmap result = b;
        MAIN.post(() -> {
            repaint(key);
            if (cbs != null)
                for (Callback cb : cbs)
                    cb.art(result);
        });
    }

    /** Invalidates the views drawing key (UI thread). */
    private static void repaint(String key) {
        Set<View> s = watchers.get(key);
        if (s != null)
            for (View v : new ArrayList<>(s))
                v.invalidate();
    }

    private static void onMain(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper())
            r.run();
        else
            MAIN.post(r);
    }

    /**
     * key's art changed on disk (a cover downloaded, a game's files or image gone): the next get() loads it again, and
     * the views drawing it repaint (so they ask). Any thread.
     */
    static void invalidate(String key) {
        if (key == null)
            return;
        synchronized (ArtCache.class) {
            none.remove(key);
            Art a = cache.get(key);
            if (a != null)
                cache.put(key, new Art(a.bitmap, a.px, -1));
        }
        onMain(() -> repaint(key));
    }

    /** Every key loads again when next asked (a scan ended, a new activity: covers and captured frames may be new). */
    static void invalidateAll() {
        synchronized (ArtCache.class) {
            gen++;
            none.clear();
        }
        onMain(() -> {
            for (String k : new ArrayList<>(watchers.keySet()))
                repaint(k);
        });
    }

    /** Keys found without art are asked again when next drawn (a scan ended: it may have cached title images). */
    static void forgetMisses() {
        synchronized (ArtCache.class) {
            none.clear();
        }
        onMain(() -> {
            for (String k : new ArrayList<>(watchers.keySet()))
                repaint(k);
        });
    }

    /** v draws key (a GameCard while attached): it is invalidated when key's art loads or changes. UI thread. */
    static void watch(String key, View v) {
        if (key == null)
            return;
        Set<View> s = watchers.get(key);
        if (s == null)
            watchers.put(key, s = new HashSet<>(2));
        s.add(v);
    }

    static void unwatch(String key, View v) {
        if (key == null)
            return;
        Set<View> s = watchers.get(key);
        if (s != null && s.remove(v) && s.isEmpty())
            watchers.remove(key);
    }

    /** The stage's scale, design to real pixels (Widgets.Stage.onMeasure). */
    static void setScale(float s) {
        if (s > 0)
            scale = s;
    }

    /** A side in design pixels as real ones, at the stage's scale. */
    static int px(int design) {
        return Math.max(1, Math.round(design * scale));
    }

    /**
     * The cache belongs to one files folder: another one (a test's) starts it empty. LauncherActivity.onCreate, which
     * also makes everything load again (invalidateAll), as each activity decoded its own art before.
     */
    static void bind(File filesDir) {
        synchronized (ArtCache.class) {
            if (boundTo != null && !boundTo.equals(filesDir))
                clear();
            boundTo = filesDir;
        }
        invalidateAll();
    }

    static synchronized void clear() {
        cache.evictAll();
        none.clear();
        gen++;
    }

    static Executor setExecutorForTest(Executor e) {
        Executor was = executor;
        executor = e == null ? LOADER : e;
        return was;
    }

    /** The bytes the cache holds now, and its bound. */
    static int bytes() {
        return cache.size();
    }

    static int maxBytes() {
        return cache.maxSize();
    }

    static int countForTest() {
        return cache.snapshot().size();
    }

    // ---- Decoding ---------------------------------------------------------------------------------------------------

    /**
     * The largest power of two that keeps a w x h image's shorter side no smaller than px once divided by it (the art
     * is cropped to a square, so the shorter side fills it): 512x384 for 288 px is 1, 1024x768 is 2.
     */
    static int sampleSize(int w, int h, int px) {
        int s = 1;
        if (w <= 0 || h <= 0 || px <= 0)
            return s;
        int shorter = Math.min(w, h);
        while (shorter / (s * 2) >= px)
            s *= 2;
        return s;
    }

    /** f decoded so that its shorter side is at least px (or whole when smaller); null when it is not an image. */
    static Bitmap decode(File f, int px) {
        if (f == null || !f.isFile())
            return null;
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(f.getPath(), o);
        if (o.outWidth <= 0 || o.outHeight <= 0)
            return null;
        BitmapFactory.Options d = new BitmapFactory.Options();
        d.inSampleSize = sampleSize(o.outWidth, o.outHeight, px);
        return BitmapFactory.decodeFile(f.getPath(), d);
    }
}
