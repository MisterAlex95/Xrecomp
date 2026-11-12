package fr.adserver.xboxrecomp;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

/**
 * Runs the library check (XBA-130): a foreground service in the launcher's process (":launcher"), so that the
 * process that drives the check outlives its screen while a game runs in front of it for three minutes, and a
 * CheckDriver ticking twice a second on its own thread. The notification says where the check is; a tap opens the
 * check screen (which a paused check needs to go on: Android lets the app start a game only from its screen).
 *
 * <p>ACTION_START: drive check.json (written by CheckActivity just before); EXTRA_FRESH false means the check was
 * left by a process that died (crash, reboot), so the driver recovers first. ACTION_PAUSE / ACTION_RESUME /
 * ACTION_STOP: the screen's buttons.
 */
public class CheckService extends Service {
    static final String TAG = "xboxrecomp";
    static final String ACTION_START = "fr.adserver.xboxrecomp.CHECK_START", ACTION_STOP = "fr.adserver.xboxrecomp.CHECK_STOP",
            ACTION_PAUSE = "fr.adserver.xboxrecomp.CHECK_PAUSE", ACTION_RESUME = "fr.adserver.xboxrecomp.CHECK_RESUME";
    static final String EXTRA_FRESH = "fresh";
    static final String CHANNEL = "check";
    static final int NOTIF_ID = 51;
    static final long TICK_MS = 500;
    /** The check holds the CPU at most this long (a whole library overnight). */
    static final long WAKE_LOCK_MS = 14L * 3600 * 1000;

    /** Tests replace the device. */
    interface PlatformFactory {
        CheckDriver.Platform make(Context c, Runnable onChange);
    }

    static volatile PlatformFactory platformOverride;
    private static volatile CheckDriver driver;

    private HandlerThread thread;
    private Handler handler;
    private PowerManager.WakeLock wakeLock;
    private NotificationManager nm;
    private long lastNotify;
    private String lastText = "";

    static Intent intent(Context c, String action) {
        return new Intent(c, CheckService.class).setAction(action);
    }

    /** The running check's driver in this process, or null. */
    static CheckDriver driver() {
        return driver;
    }

    android.os.Looper looperForTest() {
        return thread.getLooper();
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
            NotificationChannel ch = new NotificationChannel(CHANNEL, getString(R.string.CHECK_CHANNEL),
                                                             NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
        thread = new HandlerThread("xba-check");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        goForeground(notification(LibraryCheck.read(getFilesDir())));
        if (driver == null) {
            LibraryCheck.State s = LibraryCheck.read(getFilesDir());
            if (s == null || !s.active()) {
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf();
                return START_NOT_STICKY;
            }
            PlatformFactory f = platformOverride;
            Runnable changed = this::onChanged;
            CheckDriver.Platform p = f != null ? f.make(this, changed) : new CheckPlatform(this, changed);
            CheckDriver d = new CheckDriver(p, s);
            driver = d;
            boolean fresh = intent != null && intent.getBooleanExtra(EXTRA_FRESH, false);
            handler.post(() -> {
                if (!fresh)
                    d.recover();
                tick();
            });
            PowerManager pm = getSystemService(PowerManager.class);
            if (pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "xboxrecomp:check");
                wakeLock.setReferenceCounted(false);
                wakeLock.acquire(WAKE_LOCK_MS);
            }
            Log.i(TAG, "check: service started (" + (fresh ? "new check" : "carrying on") + "), "
                       + s.titles.size() + " titles, at " + s.index);
        }
        CheckDriver d = driver;
        if (ACTION_STOP.equals(action))
            handler.post(d::stop);
        else if (ACTION_PAUSE.equals(action))
            handler.post(d::pause);
        else if (ACTION_RESUME.equals(action))
            handler.post(d::resume);
        return START_NOT_STICKY;
    }

    private void tick() {
        CheckDriver d = driver;
        if (d == null)
            return;
        try {
            d.tick();
        } catch (RuntimeException e) {
            Log.w(TAG, "check: step failed", e);
        }
        if (!d.s.active()) {
            Log.i(TAG, "check: service ends (" + d.s.state + ")");
            driver = null;
            new Handler(getMainLooper()).post(this::finish);
            return;
        }
        handler.postDelayed(this::tick, TICK_MS);
    }

    private void finish() {
        if (nm != null) {
            LibraryCheck.State s = LibraryCheck.read(getFilesDir());
            stopForeground(STOP_FOREGROUND_REMOVE);
            if (s != null && LibraryCheck.DONE.equals(s.state))
                nm.notify(NOTIF_ID + 1, doneNotification(s));
        } else {
            stopForeground(STOP_FOREGROUND_REMOVE);
        }
        stopSelf();
    }

    private void onChanged() {
        CheckDriver d = driver;
        if (d == null || nm == null)
            return;
        String text = CheckActivity.statusLine(this, d.s);
        long now = SystemClock.uptimeMillis();
        if (text.equals(lastText) && now - lastNotify < 10_000)
            return;
        lastText = text;
        lastNotify = now;
        if (d.s.active())
            nm.notify(NOTIF_ID, notification(d.s));
    }

    @Override
    public void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        thread.quitSafely();
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        driver = null;
        super.onDestroy();
    }

    private void goForeground(Notification n) {
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIF_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIF_ID, n);
    }

    private PendingIntent openCheck() {
        Intent i = new Intent(this, CheckActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return PendingIntent.getActivity(this, 2, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private Notification notification(LibraryCheck.State s) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(getString(R.string.CHECK_TITLE))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(openCheck());
        if (Build.VERSION.SDK_INT >= 31)
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        if (s != null && !s.titles.isEmpty()) {
            b.setContentText(CheckActivity.statusLine(this, s));
            b.setProgress(s.titles.size(), Math.min(s.index, s.titles.size()), false);
        } else {
            b.setProgress(0, 0, true);
        }
        PendingIntent stop = PendingIntent.getService(this, 3, intent(this, ACTION_STOP),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        b.addAction(new Notification.Action.Builder(null, getString(R.string.CHECK_STOP), stop).build());
        return b.build();
    }

    private Notification doneNotification(LibraryCheck.State s) {
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle(getString(R.string.CHECK_DONE_TITLE, s.titles.size()))
                .setContentText(CheckActivity.summaryLine(this, s))
                .setAutoCancel(true)
                .setContentIntent(openCheck())
                .build();
    }
}
