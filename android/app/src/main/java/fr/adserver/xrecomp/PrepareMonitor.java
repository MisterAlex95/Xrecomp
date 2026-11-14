package fr.adserver.xrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * Kind to the device while a Prepare runs (XBA-128, docs/android/prepare.md « The device log »). In the ":prepare"
 * process, beside the job: every {@link #SAMPLE_MS} it reads the thermal status (and the battery's temperature) and
 * appends one line to {@code <work>/device.jsonl}, which tools.prepare reads before each compile starts
 * ({@code --device-log}, tools/prepare/metrics.py) and sums up into pack.json's "prepare" object:
 *
 * <pre>{"t": 1731456000, "thermal": 3, "jobs": 1, "temp_c": 41.5}</pre>
 *
 * <p>jobs = {@link DeviceGuard#jobsFor}: all of them up to moderate, one when severe, none (hold) from critical. A
 * background Prepare (a self-healing round, the -O2 rebuild) also holds while the game runs: the game comes first.
 * The platform calls are behind {@link Probe} so the JVM tests drive it with numbers.
 */
final class PrepareMonitor {
    static final String TAG = "xboxrecomp";
    static final String LOG = "device.jsonl";
    /** A sample this often; tools.prepare reads the last one before each compile. */
    static final long SAMPLE_MS = 5_000;

    interface Probe {
        /** PowerManager's thermal status (0 none ... 6 shutdown). */
        int thermal();

        /** The battery's temperature in °C, or NaN. */
        float tempC();

        /** The game runs (the app's main process is alive). */
        boolean gameRunning();
    }

    private final File log;
    private final int jobs;
    private final boolean background;
    private final Probe probe;
    private volatile Thread thread;
    private volatile boolean stop;
    /** The last line written (tests, the status). */
    volatile int lastThermal = -1, lastJobs = -1;

    PrepareMonitor(File work, int jobs, boolean background, Probe probe) {
        this.log = new File(work, LOG);
        this.jobs = Math.max(1, jobs);
        this.background = background;
        this.probe = probe;
    }

    static File logFile(File work) {
        return new File(work, LOG);
    }

    /** The compiles allowed now: the device's heat, and a game that runs for a background Prepare. */
    int jobsNow(int thermal, boolean gameRunning) {
        return background && gameRunning ? 0 : DeviceGuard.jobsFor(thermal, jobs);
    }

    /** One sample: appended to the device log. Returns the jobs it allows. */
    int sample(long nowMs) {
        int th = probe.thermal();
        boolean game = background && probe.gameRunning();
        int j = jobsNow(th, game);
        float temp = probe.tempC();
        try {
            JSONObject o = new JSONObject().put("t", nowMs / 1000).put("thermal", th).put("jobs", j);
            if (!Float.isNaN(temp))
                o.put("temp_c", Math.round(temp * 10) / 10.0);
            if (game)
                o.put("game", true);
            append(o.toString() + "\n");
        } catch (JSONException | IOException e) {
            Log.w(TAG, "prepare: device log: " + e);
        }
        if (th != lastThermal || j != lastJobs)
            Log.i(TAG, "prepare: thermal status " + th + ", " + j + " compile job" + (j == 1 ? "" : "s")
                       + (game ? " (the game runs)" : ""));
        lastThermal = th;
        lastJobs = j;
        return j;
    }

    private void append(String line) throws IOException {
        try (FileOutputStream out = new FileOutputStream(log, true)) {
            out.write(line.getBytes(StandardCharsets.UTF_8));
        }
    }

    /** Starts sampling (a fresh log: this Prepare's samples only). */
    void start() {
        File d = log.getParentFile();
        if (d != null)
            //noinspection ResultOfMethodCallIgnored
            d.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        log.delete();
        sample(System.currentTimeMillis());
        stop = false;
        Thread t = new Thread(() -> {
            while (!stop) {
                try {
                    Thread.sleep(SAMPLE_MS);
                } catch (InterruptedException e) {
                    return;
                }
                if (!stop)
                    sample(System.currentTimeMillis());
            }
        }, "xba-prepare-monitor");
        t.setDaemon(true);
        thread = t;
        t.start();
    }

    void stop() {
        stop = true;
        Thread t = thread;
        if (t != null)
            t.interrupt();
        thread = null;
    }

    /** The device's own probes. */
    static Probe deviceProbe(Context c) {
        Context app = c.getApplicationContext();
        return new Probe() {
            @Override
            public int thermal() {
                PowerManager pm = app.getSystemService(PowerManager.class);
                return pm == null ? 0 : pm.getCurrentThermalStatus();
            }

            @Override
            public float tempC() {
                Intent b = app.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
                int t = b == null ? Integer.MIN_VALUE : b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
                return t == Integer.MIN_VALUE ? Float.NaN : t / 10f;
            }

            @Override
            public boolean gameRunning() {
                ActivityManager am = app.getSystemService(ActivityManager.class);
                List<ActivityManager.RunningAppProcessInfo> procs = am == null ? null : am.getRunningAppProcesses();
                if (procs == null)
                    return false;
                for (ActivityManager.RunningAppProcessInfo p : procs) {
                    if (p.processName.equals(app.getPackageName()))
                        return true;
                }
                return false;
            }
        };
    }
}
