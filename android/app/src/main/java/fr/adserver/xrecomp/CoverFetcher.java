package fr.adserver.xrecomp;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Supplier;

/**
 * The cover art downloads (CoverArt.fetch), one worker for the whole process (report-launcher-lag RC4): each launcher
 * activity hands it the games it shows (enqueue), and it fetches each key once, however many activities asked. It
 * holds no activity: a cover found is told on the main thread to the listener the resumed launcher set (setListener),
 * else kept for the next one (takeUndelivered). Which games still want a cover (CoverArt.wanted, two stats each) is
 * worked out on the worker, not on the main thread.
 */
final class CoverFetcher {
    private CoverFetcher() {}

    /** A cover was stored for key (main thread). */
    interface Listener {
        void onCover(String key);
    }

    /** Tests: stands in for the network. */
    static Supplier<VkDriverIndex.Http> httpFactory = VkDriverIndex.UrlHttp::new;

    static final String THREAD = "cover-art";

    private static final class Item {
        final File files;
        final String key, name;

        Item(File files, String key, String name) {
            this.files = files;
            this.key = key;
            this.name = name;
        }
    }

    private static final Object LOCK = new Object();
    /** Lists of candidates to sort out (first), then the games to look for, in order. */
    private static final ArrayDeque<List<Item>> plans = new ArrayDeque<>();
    private static final ArrayDeque<Item> queue = new ArrayDeque<>();
    /** Keys queued or being fetched: a second enqueue of them adds nothing. */
    private static final Set<String> known = new HashSet<>();
    private static Thread worker;

    // Main thread only.
    private static Listener listener;
    private static final Set<String> undelivered = new java.util.LinkedHashSet<>();

    /**
     * The games (key, file name) that may want a cover: those that do (CoverArt.wanted) and are not queued yet are
     * looked for, one after the other, after what is queued already.
     */
    static void enqueue(File filesDir, List<String[]> candidates) {
        List<Item> plan = new ArrayList<>(candidates.size());
        for (String[] c : candidates)
            plan.add(new Item(filesDir, c[0], c[1]));
        synchronized (LOCK) {
            plans.add(plan);
            if (worker == null) {
                worker = new Thread(CoverFetcher::work, THREAD);
                worker.setDaemon(true);
                worker.start();
            }
        }
    }

    private static void work() {
        VkDriverIndex.Http http = null;
        while (true) {
            List<Item> plan;
            Item item;
            synchronized (LOCK) {
                plan = plans.poll();
                item = plan == null ? queue.poll() : null;
                if (plan == null && item == null) {
                    worker = null;
                    LOCK.notifyAll();
                    return;
                }
            }
            if (plan != null) {
                sortOut(plan);
                continue;
            }
            boolean found = false;
            try {
                if (http == null)
                    http = httpFactory.get();
                found = CoverArt.wanted(item.files, item.key, System.currentTimeMillis())
                        && CoverArt.fetch(http, item.files, item.key, item.name);
            } catch (RuntimeException e) {
                Log.w(LauncherActivity.TAG, "launcher: cover art " + item.key + ": " + e);
            }
            synchronized (LOCK) {
                known.remove(item.key);   // its file (cover or miss) is written: CoverArt.wanted says no now
            }
            if (found) {
                String key = item.key;
                new Handler(Looper.getMainLooper()).post(() -> deliver(key));
            }
        }
    }

    private static void sortOut(List<Item> plan) {
        long now = System.currentTimeMillis();
        List<Item> want = new ArrayList<>();
        for (Item i : plan) {
            synchronized (LOCK) {
                if (known.contains(i.key))
                    continue;
            }
            if (CoverArt.wanted(i.files, i.key, now))
                want.add(i);
        }
        int added = 0;
        synchronized (LOCK) {
            for (Item i : want)
                if (known.add(i.key)) {
                    queue.add(i);
                    added++;
                }
        }
        if (added > 0)
            Log.i(LauncherActivity.TAG, "launcher: cover art: looking for " + added + " game(s)");
    }

    private static void deliver(String key) {
        if (listener != null)
            listener.onCover(key);
        else
            undelivered.add(key);
    }

    /** The resumed launcher's listener (main thread); null or clearListener when it goes. */
    static void setListener(Listener l) {
        listener = l;
    }

    /** Takes l off, if it is the one set (main thread). */
    static void clearListener(Listener l) {
        if (listener == l)
            listener = null;
    }

    /** The covers found while no launcher listened, in order (main thread); emptied. */
    static List<String> takeUndelivered() {
        List<String> out = new ArrayList<>(undelivered);
        undelivered.clear();
        return out;
    }

    /** Tests: waits until nothing is left to fetch; false on timeout. */
    static boolean awaitIdleForTest(long ms) throws InterruptedException {
        long end = System.currentTimeMillis() + ms;
        synchronized (LOCK) {
            while (worker != null || !plans.isEmpty() || !queue.isEmpty()) {
                long left = end - System.currentTimeMillis();
                if (left <= 0)
                    return false;
                LOCK.wait(left);
            }
        }
        return true;
    }

    /** Tests: forgets what was queued and told (after awaitIdleForTest), and the network stand-in. */
    static void resetForTest() {
        synchronized (LOCK) {
            plans.clear();
            queue.clear();
            known.clear();
        }
        listener = null;
        undelivered.clear();
        httpFactory = VkDriverIndex.UrlHttp::new;
    }
}
