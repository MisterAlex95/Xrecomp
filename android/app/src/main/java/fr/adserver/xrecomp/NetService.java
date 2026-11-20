package fr.adserver.xrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.List;

/**
 * The network library's downloads and magnet metadata (XBA-321, docs « Network library »), in the launcher's process
 * (one writer of downloads.json and sources.json with the launcher, under their locks), as a foreground service while
 * there is something to do: a worker ticks NetRunner every second, a second thread fetches the metadata of magnets
 * that have none (one at a time, {@link #META_TIMEOUT_S} each). It ends when no download is queued or runs and no
 * metadata is missing; a download that waits for Wi-Fi stays queued, and the launcher starts the service again when
 * the library opens.
 *
 * Intents: {@link #kick} (start or go on), {@link #cancel} (one download), {@link #removeSource}. Only on Wi-Fi unless
 * « Download on mobile data » is on ({@link #mobileData}).
 */
public final class NetService extends Service {
    private static final String TAG = "XRecomp";
    static final String CHANNEL = "net";
    static final int NOTIF_ID = 0x4e45;
    static final String ACTION_KICK = "fr.adserver.xrecomp.net.KICK", ACTION_CANCEL = "fr.adserver.xrecomp.net.CANCEL",
            ACTION_REMOVE_SOURCE = "fr.adserver.xrecomp.net.REMOVE_SOURCE";
    static final String EXTRA_KEY = "key", EXTRA_SOURCE = "source";
    static final int META_TIMEOUT_S = 60;
    static final String PREFS = "net", PREF_MOBILE_DATA = "mobile_data";

    private NotificationManager nm;
    private NetEngine engine;
    private NetRunner runner;
    private Thread worker, fetcher;
    private volatile boolean ending;
    /** Work handed over by intents, run on the worker (the engine has one thread). */
    private final java.util.concurrent.ConcurrentLinkedQueue<Runnable> todo =
            new java.util.concurrent.ConcurrentLinkedQueue<>();

    static void kick(Context c) {
        start(c, new Intent(c, NetService.class).setAction(ACTION_KICK));
    }

    static void cancel(Context c, String key) {
        start(c, new Intent(c, NetService.class).setAction(ACTION_CANCEL).putExtra(EXTRA_KEY, key));
    }

    static void removeSource(Context c, String id) {
        start(c, new Intent(c, NetService.class).setAction(ACTION_REMOVE_SOURCE).putExtra(EXTRA_SOURCE, id));
    }

    private static void start(Context c, Intent i) {
        try {
            c.startForegroundService(i);
        } catch (RuntimeException e) {   // not allowed from the background now: the next library opening kicks it
            Log.w(TAG, "net: cannot start the service: " + e);
        }
    }

    static boolean mobileData(Context c) {
        return c.getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_MOBILE_DATA, false);
    }

    static void setMobileData(Context c, boolean on) {
        c.getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_MOBILE_DATA, on).apply();
    }

    /** Downloading is allowed on the network the device uses now (Wi-Fi, Ethernet; mobile data when allowed). */
    static boolean networkAllowed(Context c) {
        ConnectivityManager cm = c.getSystemService(ConnectivityManager.class);
        if (cm == null)
            return false;
        Network n = cm.getActiveNetwork();
        NetworkCapabilities nc = n == null ? null : cm.getNetworkCapabilities(n);
        if (nc == null || !nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET))
            return false;
        return mobileData(c) || nc.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
    }

    /** The sources whose metadata is missing (magnets not fetched yet, nor failed). */
    static List<RemoteLibrary.Source> missingMeta(File filesDir) {
        List<RemoteLibrary.Source> l = new java.util.ArrayList<>();
        for (RemoteLibrary.Source s : RemoteLibrary.sources(filesDir))
            if (!s.magnet.isEmpty() && s.metaError.isEmpty() && RemoteLibrary.meta(filesDir, s.id) == null)
                l.add(s);
        return l;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.NET_CHANNEL),
                                                             NotificationManager.IMPORTANCE_LOW);
            ch.setDescription(getString(R.string.NET_CHANNEL_DESC));
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Notification n = notification(null);
            if (Build.VERSION.SDK_INT >= 34)
                startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else
                startForeground(NOTIF_ID, n);
        } catch (RuntimeException e) {
            Log.w(TAG, "net: cannot go to the foreground: " + e);
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        String action = intent == null ? ACTION_KICK : intent.getAction();
        File downloads = LibraryScanner.netDir(this);
        if (ACTION_CANCEL.equals(action)) {
            String key = intent.getStringExtra(EXTRA_KEY);
            todo.add(() -> {
                try {
                    if (runner != null)
                        runner.cancel(key);
                    else if (downloads != null)
                        NetRunner.cancelFiles(getFilesDir(), downloads, key);
                    Log.i(TAG, "net: download of " + key + " cancelled");
                } catch (IOException e) {
                    Log.w(TAG, "net: cancel " + key + ": " + e);
                }
            });
        } else if (ACTION_REMOVE_SOURCE.equals(action)) {
            String id = intent.getStringExtra(EXTRA_SOURCE);
            todo.add(() -> {
                try {
                    NetRunner.removeSource(engine, getFilesDir(), downloads, id);
                    Log.i(TAG, "net: source " + id + " removed");
                } catch (IOException e) {
                    Log.w(TAG, "net: remove source " + id + ": " + e);
                }
            });
        }
        if (downloads == null) {
            Log.w(TAG, "net: no external storage: nothing can be downloaded");
            runTodoAndEnd();
            return START_NOT_STICKY;
        }
        ensureWorker(downloads);
        return START_NOT_STICKY;
    }

    private synchronized void ensureWorker(File downloads) {
        ending = false;
        if (worker != null && worker.isAlive())
            return;
        if (engine == null)
            engine = new LibtorrentEngine(new File(getCacheDir(), "net-meta"));
        if (runner == null)
            runner = new NetRunner(engine, getFilesDir(), downloads);
        worker = new Thread(this::loop, "xba-net");
        worker.start();
    }

    private void runTodoAndEnd() {
        Runnable r;
        while ((r = todo.poll()) != null)
            r.run();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    private void loop() {
        long lastNotif = 0;
        boolean more = true;
        while (!ending) {
            Runnable r;
            while ((r = todo.poll()) != null)
                r.run();
            NetRunner.Conditions c = new NetRunner.Conditions();
            c.network = networkAllowed(this);
            runner.downloadDir.mkdirs();
            c.free = runner.downloadDir.getUsableSpace();
            c.now = System.currentTimeMillis();
            try {
                more = runner.step(c);
            } catch (IOException | RuntimeException | LinkageError e) {
                Log.w(TAG, "net: tick failed", e);
                more = false;
            }
            boolean fetching = startFetches(c.network);
            if (!more && !fetching && todo.isEmpty())
                break;
            long now = SystemClock.elapsedRealtime();
            if (now - lastNotif >= 1000 && nm != null) {
                lastNotif = now;
                nm.notify(NOTIF_ID, notification(currentJob()));
            }
            SystemClock.sleep(1000);
        }
        Log.i(TAG, "net: nothing more to do, the service ends");
        engine.stop();
        synchronized (this) {
            worker = null;
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    /** Starts fetching the next missing metadata (one at a time); true while one is being fetched. */
    private boolean startFetches(boolean network) {
        if (fetcher != null && fetcher.isAlive())
            return true;
        if (!network)
            return false;
        List<RemoteLibrary.Source> miss = missingMeta(getFilesDir());
        if (miss.isEmpty())
            return false;
        RemoteLibrary.Source s = miss.get(0);
        fetcher = new Thread(() -> {
            Log.i(TAG, "net: fetching the metadata of " + s.id + " (" + s.name + ")");
            String why;
            try {
                byte[] b = engine.fetchMetadata(s.magnet, META_TIMEOUT_S);
                why = b == null ? NetRunner.E_NO_PEERS : saveMeta(s.id, b) ? "" : "bad metadata";
            } catch (IOException | RuntimeException | LinkageError e) {
                why = e.getMessage() == null ? e.toString() : e.getMessage();
            }
            try {
                if (!why.isEmpty())
                    RemoteLibrary.setMetaError(getFilesDir(), s.id, why);
            } catch (IOException ignored) {
                // shown as still fetching; the next start tries again
            }
            Log.i(TAG, "net: metadata of " + s.id + (why.isEmpty() ? " fetched" : " not fetched: " + why));
        }, "xba-net-meta");
        fetcher.start();
        return true;
    }

    private boolean saveMeta(String id, byte[] b) throws IOException {
        try {
            return RemoteLibrary.saveMetadata(getFilesDir(), id, b);
        } catch (IOException e) {
            // A bare info dictionary: wrap it as a .torrent ("d4:info" + info + "e").
            TorrentMeta.parseInfo(b);
            byte[] t = new byte[b.length + 8];
            System.arraycopy("d4:info".getBytes(java.nio.charset.StandardCharsets.US_ASCII), 0, t, 0, 7);
            System.arraycopy(b, 0, t, 7, b.length);
            t[t.length - 1] = 'e';
            return RemoteLibrary.saveMetadata(getFilesDir(), id, t);
        }
    }

    private NetDownloads.Job currentJob() {
        List<NetDownloads.Job> p = NetDownloads.pending(NetDownloads.read(getFilesDir()));
        return p.isEmpty() ? null : p.get(0);
    }

    private Notification notification(NetDownloads.Job j) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(PendingIntent.getActivity(this, 0, new Intent(this, LauncherActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), PendingIntent.FLAG_IMMUTABLE));
        if (Build.VERSION.SDK_INT >= 31)
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        if (j == null) {
            b.setContentTitle(getString(R.string.NET_NOTIF_TITLE_PLAIN)).setProgress(0, 0, true);
            return b.build();
        }
        b.setContentTitle(getString(R.string.NET_NOTIF_TITLE, j.title));
        if (NetRunner.E_WIFI.equals(j.error))
            b.setContentText(getString(R.string.NET_WAIT_WIFI));
        else if (NetDownloads.RUNNING.equals(j.state))
            b.setContentText(getString(R.string.NET_PROGRESS, j.percent(), android.text.format.Formatter.formatShortFileSize(this, j.rate),
                                       j.peers))
             .setProgress(100, j.percent(), false);
        else
            b.setContentText(getString(R.string.NET_CHIP_WAITING)).setProgress(0, 0, true);
        PendingIntent cancel = PendingIntent.getService(this, 1, new Intent(this, NetService.class)
                .setAction(ACTION_CANCEL).putExtra(EXTRA_KEY, j.key),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        b.addAction(new Notification.Action.Builder(null, getString(R.string.CANCEL), cancel).build());
        return b.build();
    }

    @Override
    public void onDestroy() {
        ending = true;
        super.onDestroy();
    }
}
