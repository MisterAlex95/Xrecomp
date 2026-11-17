package fr.adserver.xrecomp;

import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * The N64 shader warm-up's runner (ShaderWarmup), in its own process (":warmup", AndroidManifest.xml): the game's
 * Vulkan environment set as ShellActivity sets it (VkDrivers.runEnv, cacheEnv), libmain.so loaded, then
 * n64_rdp_vk_warmup on a background-priority thread; its record written, « Shaders prêts » when it compiled anything,
 * then the process ends (the driver it loaded and the buffers it took go with it). One warm-up per process: a new
 * request ends this one first (ShaderWarmup.request), and so does a game's start (ShellActivity). A GameCube title
 * (XBA-264) compiles its GX shader lists into program binaries instead (GcShaderWarmup.run).
 */
public class ShaderWarmupService extends Service {
    private Thread worker;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (worker != null)
            return START_NOT_STICKY;   // one per process (ShaderWarmup.request ends a running one before)
        String tid = intent == null ? null : intent.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID);
        worker = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);   // paraLLEl-RDP's threads inherit it
            run(tid);
        }, "ShaderWarmup");
        worker.start();
        return START_NOT_STICKY;
    }

    private void run(String tid) {
        if (GcShaderWarmup.isGc(tid)) {   // XBA-264: the GameCube's GX program binaries
            if (ShaderWarmup.gameRunning(this)) {
                ShaderWarmup.keepPending(this, tid);
                end(false);
                return;
            }
            int[] r = GcShaderWarmup.run(this, tid);
            end(r != null && r[0] == 0 && r[3] > 0);
            return;
        }
        ShaderWarmup.Target t = ShaderWarmup.target(this, tid);
        if (ShaderWarmup.gameRunning(this)) {   // a game started since the request: later
            Log.i(LauncherActivity.TAG, "warmup: " + t + ": a game runs, kept for later");
            ShaderWarmup.keepPending(this, tid);
            end(false);
            return;
        }
        List<String[]> env = new ArrayList<>(VkDrivers.runEnv(t.driver, getApplicationInfo().nativeLibraryDir));
        env.add(VkDrivers.cacheEnv(t.driver, getCacheDir()));
        env.add(new String[] { "RECOMP_N64_UPSCALE", null });   // the factor is passed as it is
        for (String[] kv : env) {
            try {
                if (kv[1] != null)
                    Os.setenv(kv[0], kv[1], true);
                else
                    Os.unsetenv(kv[0]);
            } catch (ErrnoException e) {
                Log.w(LauncherActivity.TAG, "warmup: cannot set " + kv[0], e);
            }
        }
        if (!WarmupNative.load()) {
            end(false);
            return;
        }
        Log.i(LauncherActivity.TAG, "warmup: " + t + " starting");
        int[] r = WarmupNative.nativeWarmup(t.factor);
        if (r == null || r.length < 7) {
            end(false);
            return;
        }
        Log.i(LauncherActivity.TAG, "warmup: " + t + " rc " + r[0] + ", " + r[1] + "x, " + r[2] + " start-up + " + r[3]
                                    + " VI pipelines, " + r[4] + " compiled, cache read " + r[5] + " KB, " + r[6]
                                    + " ms");
        if (r[0] == 0 || r[0] == 1) {
            File rec = ShaderWarmup.recordFile(getCacheDir(), t);
            ShaderWarmup.write(rec, ShaderWarmup.recordLine(ShaderWarmup.versionCode(this), r));
        }
        end(r[0] == 0 && r[4] > 0);
    }

    /** The toast (when it compiled something), then the process goes: a custom driver stays loaded in it otherwise. */
    private void end(boolean ready) {
        Handler main = new Handler(Looper.getMainLooper());
        main.post(() -> {
            if (ready)
                Toast.makeText(this, R.string.SHADERS_READY, Toast.LENGTH_SHORT).show();
            stopSelf();
        });
        main.postDelayed(() -> Process.killProcess(Process.myPid()), ready ? 3000 : 200);
    }
}
