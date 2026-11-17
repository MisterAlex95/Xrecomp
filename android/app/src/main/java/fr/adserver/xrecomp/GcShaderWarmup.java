package fr.adserver.xrecomp;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The GameCube shader warm-up (XBA-264, layer 3 of the GX shader plan; docs/android/prepare.md « GameCube shader
 * warm-up »): the TEV states a title's runs met (gx_shaders.txt, in its work folder and, when its Prepare put one
 * there, in its pack) compiled by this device's GLES driver into program binaries, gx_programs.bin, which the game
 * links at its start instead of compiling GLSL. Run by ShaderWarmupService in the ":warmup" process like the N64's
 * (never next to a game: ShellActivity ends it; a request while a game runs is kept, ShaderWarmup.keepPending).
 * <ul>
 *   <li>when: at the end of a GameCube Prepare (PrepareService); after an app update, for the last GameCube game
 *       played (a new APK may generate other shaders); no list yet: nothing to do (the first run writes it, and the
 *       game adds the programs it compiles to the cache itself);</li>
 *   <li>where: always the work folder, &lt;files&gt;/work/gc-&lt;KEY&gt;/gx_programs.bin, not the pack: a Prepare
 *       publishes the pack directory whole (replacing it), the work folder survives it, an interpreted title without a
 *       pack has one too, and the binaries are this device's and driver's, not part of what a pack carries;</li>
 *   <li>a driver update or another shader generator: the file's key differs, the game ignores it and rebuilds it,
 *       and so does the next warm-up. A warm-up whose states are all cached returns at once.</li>
 * </ul>
 */
final class GcShaderWarmup {
    private GcShaderWarmup() {}

    static final String LIST = "gx_shaders.txt", CACHE = "gx_programs.bin";

    static boolean isGc(String tid) {
        return tid != null && Console.ofKey(tid) == Console.GC;
    }

    // ---- Pure ------------------------------------------------------------------------------------------------------

    static File workDir(File filesDir, String tid) {
        return new File(new File(filesDir, "work"), tid);
    }

    /** The program binaries the game loads (gc_runtime: next to discovered.txt). */
    static File cacheFile(File filesDir, String tid) {
        return new File(workDir(filesDir, tid), CACHE);
    }

    /** The shader lists that exist: the work folder's (the runs'), then the pack's (its Prepare's copy). */
    static String[] lists(File filesDir, String tid) {
        List<String> out = new ArrayList<>();
        for (File f : new File[] { new File(workDir(filesDir, tid), LIST), new File(Pack.packDir(filesDir, tid), LIST) })
            if (f.isFile() && f.length() > 0)
                out.add(f.getPath());
        return out.toArray(new String[0]);
    }

    static ShaderWarmup.Decision decide(boolean hasList, boolean gameRunning) {
        if (!hasList)
            return ShaderWarmup.Decision.SKIP_NO_LIST;
        return gameRunning ? ShaderWarmup.Decision.PENDING : ShaderWarmup.Decision.RUN;
    }

    // ---- Android ---------------------------------------------------------------------------------------------------

    /** A request (ShaderWarmup.request for a GameCube title): started, kept for later, or skipped; logged. */
    static ShaderWarmup.Decision request(Context c, String tid, ShaderWarmup.Reason r) {
        File files = PrepareService.filesDir(c);
        boolean busy = ShaderWarmup.gameRunning(c)
                       || (r != ShaderWarmup.Reason.PREPARE && ShaderWarmup.processPid(c, ":prepare") != 0);
        ShaderWarmup.Decision d = decide(lists(files, tid).length > 0, busy);
        Log.i(LauncherActivity.TAG, "warmup: " + r.name().toLowerCase(Locale.ROOT) + " " + tid + " (GX programs): "
                                    + d.name().toLowerCase(Locale.ROOT));
        switch (d) {
        case RUN:
            ShaderWarmup.cancel(c);
            try {
                c.startService(new Intent(c, ShaderWarmupService.class).putExtra(ShaderWarmup.EXTRA_TITLE_ID, tid));
            } catch (IllegalStateException | SecurityException e) {   // background start refused: later
                Log.w(LauncherActivity.TAG, "warmup: cannot start now, kept for later (" + e.getMessage() + ")");
                ShaderWarmup.keepPending(c, tid);
            }
            break;
        case PENDING:
            ShaderWarmup.keepPending(c, tid);
            break;
        default:
            break;
        }
        return d;
    }

    /** In the ":warmup" process (ShaderWarmupService): the native warm-up; its result, null when it could not run. */
    static int[] run(Context c, String tid) {
        File files = PrepareService.filesDir(c);
        String[] lists = lists(files, tid);
        File cache = cacheFile(files, tid);
        if (lists.length == 0 || !WarmupNative.load())
            return null;
        //noinspection ResultOfMethodCallIgnored
        cache.getParentFile().mkdirs();
        Log.i(LauncherActivity.TAG, "warmup: " + tid + " GX programs from " + lists.length + " list(s) starting");
        int[] r = WarmupNative.nativeGcWarmup(lists, cache.getPath());
        if (r != null && r.length >= 6)
            Log.i(LauncherActivity.TAG, "warmup: " + tid + " GX programs rc " + r[0] + ", " + r[1] + " states, " + r[2]
                                        + " cached, " + r[3] + " compiled, " + r[4] + " failed, " + r[5] + " ms");
        return r;
    }
}
