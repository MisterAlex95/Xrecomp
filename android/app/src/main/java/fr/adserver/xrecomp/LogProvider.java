package fr.adserver.xrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * Hands the Prepare's log to another app (« Share log » on the launcher), read-only, through a temporary URI grant:
 * content://<app id>.logs/prepare/<TITLEID>/prepare.log; and the game's logcat dump (« Share log » on the companion
 * panel), content://<app id>.logs/game/game.log; and the log of the game's last run, crash included, dumped by the
 * launcher after the game's process ended (« Share the last game's log »), content://<app id>.logs/game/last-game.log.
 * Only files named prepare.log under <files>/prepare, <files>/logs/game.log and <files>/logs/last-game.log are served. A framework ContentProvider rather than AndroidX's FileProvider (the app has no AndroidX).
 * Never anything under <files>/lsfg (the frame generation shaders taken from the player's Lossless.dll,
 * docs/technical/framegen.md): {@link #file} refuses any path that resolves there.
 */
public class LogProvider extends ContentProvider {
    static String authority(Context c) {
        return c.getPackageName() + ".logs";
    }

    static Uri uriFor(Context c, String titleId) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath("prepare")
                .appendPath(titleId).appendPath("prepare.log").build();
    }

    /** The game's log (« Share log » on the companion panel): content://<app id>.logs/game/game.log. */
    static Uri gameUri(Context c) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath("game")
                .appendPath("game.log").build();
    }

    /** The last game's log (« Share the last game's log » in Settings): content://<app id>.logs/game/last-game.log. */
    static Uri lastGameUri(Context c) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath("game")
                .appendPath(LAST_GAME_LOG).build();
    }

    static final String LAST_GAME_LOG = "last-game.log";

    /** <files>/logs/last-game.log: the game process's last run, written by {@link #dumpLastGame}. */
    static File lastGameLogFile(Context c) {
        return new File(new File(c.getFilesDir(), "logs"), LAST_GAME_LOG);
    }

    /**
     * The library check's report (« Share report », XBA-130): content://<app id>.logs/check/compat-results.jsonl, the
     * file LibraryCheck.resultsFile names (one line per title: no game data).
     */
    static Uri reportUri(Context c) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath(LibraryCheck.DIR)
                .appendPath(LibraryCheck.RESULTS).build();
    }

    /** <files>/logs/game.log: the game process's logcat, dumped by {@link #dumpOwnLogcat}. */
    static File gameLogFile(Context c) {
        return new File(new File(c.getFilesDir(), "logs"), "game.log");
    }

    /**
     * This process's logcat (an app reads its own lines without a permission) into f, the last 20000 lines: the
     * runtime's stdout and stderr go to logcat (tag xboxrecomp). False when nothing could be written.
     */
    static boolean dumpOwnLogcat(File f) {
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return false;
        try {
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "20000",
                                           "--pid=" + android.os.Process.myPid())
                    .redirectErrorStream(true).redirectOutput(f).start();
            if (!p.waitFor(10, java.util.concurrent.TimeUnit.SECONDS))
                p.destroy();
        } catch (IOException e) {
            return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return f.isFile() && f.length() > 0;
    }

    /**
     * The game's last run into f, for « Share the last game's log » (the launcher runs in its own process, under the
     * same user id, so it still reads what the game's process logged once that process has died): how the system says
     * the process ended (ApplicationExitInfo: crash, native crash, not responding, killed…), the process's own lines
     * (logcat main, system and crash buffers, --pid of the dead process), then the crash buffer as a whole (the native
     * backtrace is logged by crash_dump, a process of its own under our user id), and the system's trace for a « not
     * responding » end (text; a native crash's trace is a tombstone protobuf, its backtrace is in the crash buffer).
     * False when nothing could be written.
     */
    static boolean dumpLastGame(Context c, File f) {
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return false;
        android.app.ApplicationExitInfo exit = lastGameExit(c);
        try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) {
            StringBuilder head = new StringBuilder("# XRecomp: the game's last run\n");
            head.append("# app ").append(c.getString(R.string.xba_version)).append(", Android ")
                .append(android.os.Build.VERSION.RELEASE).append(", ").append(android.os.Build.MANUFACTURER)
                .append(' ').append(android.os.Build.MODEL).append('\n');
            if (exit == null)
                head.append("# no record of how the game's process ended: the last lines of the app's log\n");
            else
                head.append(exitHeader(exit.getPid(), exit.getTimestamp(), reasonName(exit.getReason()),
                                       exit.getStatus(), exit.getDescription()));
            out.write(head.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
            if (exit != null) {
                section(out, "the game process's log (pid " + exit.getPid() + ")");
                logcat(out, "-b", "main,system,crash", "--pid=" + exit.getPid());
            } else {
                section(out, "the app's log");
                logcat(out, "-b", "main,system,crash", "-t", "20000");
            }
            section(out, "crash buffer");
            logcat(out, "-b", "crash");
            if (exit != null && exit.getReason() == android.app.ApplicationExitInfo.REASON_ANR) {
                section(out, "system trace");
                try (java.io.InputStream in = exit.getTraceInputStream()) {
                    if (in != null)
                        copy(in, out);
                } catch (IOException e) {
                    out.write(("(trace unreadable: " + e + ")\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
            }
        } catch (IOException e) {
            return false;
        }
        return f.isFile() && f.length() > 0;
    }

    /** The newest end of the game's process (the main one, named after the package), or null. */
    private static android.app.ApplicationExitInfo lastGameExit(Context c) {
        android.app.ActivityManager am = c.getSystemService(android.app.ActivityManager.class);
        if (am == null)
            return null;
        try {
            for (android.app.ApplicationExitInfo x : am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 16))
                if (c.getPackageName().equals(x.getProcessName()))
                    return x;   // newest first
        } catch (RuntimeException e) {
            // no record
        }
        return null;
    }

    /** The header lines of the dump: when and how the game's process ended. */
    static String exitHeader(int pid, long time, String reason, int status, String description) {
        java.text.SimpleDateFormat fmt = new java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", java.util.Locale.ROOT);
        return "# game process " + pid + " ended " + fmt.format(new java.util.Date(time)) + ": " + reason
               + (status != 0 ? ", status " + status : "")
               + (description == null || description.isEmpty() ? "" : " (" + description + ")") + "\n";
    }

    /** ApplicationExitInfo's reason, in words. */
    static String reasonName(int r) {
        switch (r) {
        case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
        case android.app.ApplicationExitInfo.REASON_CRASH: return "Java exception";
        case android.app.ApplicationExitInfo.REASON_ANR: return "not responding";
        case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "killed for memory";
        case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "exited";
        case android.app.ApplicationExitInfo.REASON_SIGNALED: return "killed by a signal";
        case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "stopped by the user";
        case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "killed for resource use";
        case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "failed to start";
        case android.app.ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "killed: permission change";
        case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "killed: a dependency died";
        default: return "reason " + r;
        }
    }

    private static void section(java.io.OutputStream out, String name) throws IOException {
        out.write(("\n# ---- " + name + " ----\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    /** logcat -d -v threadtime with these arguments, appended to out (an app reads its own user id's lines). */
    private static void logcat(java.io.OutputStream out, String... args) throws IOException {
        java.util.List<String> cmd = new java.util.ArrayList<>(java.util.Arrays.asList("logcat", "-d", "-v",
                                                                                       "threadtime"));
        cmd.addAll(java.util.Arrays.asList(args));
        Process p;
        try {
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        } catch (IOException e) {   // no logcat (a test host): said in the file, the rest is still written
            out.write(("(logcat unavailable: " + e.getMessage() + ")\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
            return;
        }
        try (java.io.InputStream in = p.getInputStream()) {
            copy(in, out);
        }
        try {
            if (!p.waitFor(10, java.util.concurrent.TimeUnit.SECONDS))
                p.destroy();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            p.destroy();
        }
    }

    private static void copy(java.io.InputStream in, java.io.OutputStream out) throws IOException {
        byte[] b = new byte[64 * 1024];
        for (int n; (n = in.read(b)) > 0; )
            out.write(b, 0, n);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The file behind a URI, or null when it is not a Prepare log or the game's log. */
    private File file(Uri uri) {
        Context c = getContext();
        if (c == null)
            return null;
        File f = servable(c, uri);
        if (f == null)
            return null;
        try {
            String path = f.getCanonicalPath();
            for (String own : new String[] {"lsfg", VkDrivers.DIR}) {   // shader cache, custom Vulkan drivers
                File d = new File(c.getFilesDir(), own).getCanonicalFile();
                if (path.startsWith(d.getPath() + File.separator))
                    return null;
            }
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    /**
     * A key a Prepare log can be under: an Xbox title ID, or « &lt;console&gt;-&lt;key&gt; » for a console that
     * prepares, with the key rule of Console.packDirName (a GameCube disc is « gc-GALE01-0 », « gc-GALE01-0-D2 »).
     * Derived from Console so a new console cannot be forgotten here; the canonical-path check still keeps the file
     * under files/prepare.
     */
    static boolean preparedKey(String tid) {
        if (tid == null)
            return false;
        if (tid.matches("[0-9A-Fa-f]{1,8}"))
            return true;
        Console con = Console.ofKey(tid);
        return con != Console.XBOX && con.prepares()
               && tid.substring(con.id.length() + 1).matches("[A-Za-z0-9_-]{1,40}");
    }

    private static File servable(Context c, Uri uri) {
        java.util.List<String> seg = uri.getPathSegments();
        if (seg.size() == 2 && seg.get(0).equals("game") && seg.get(1).equals("game.log"))
            return gameLogFile(c);
        if (seg.size() == 2 && seg.get(0).equals("game") && seg.get(1).equals(LAST_GAME_LOG))
            return lastGameLogFile(c);
        if (seg.size() == 2 && seg.get(0).equals(LibraryCheck.DIR) && seg.get(1).equals(LibraryCheck.RESULTS))
            return LibraryCheck.resultsFile(c.getFilesDir());
        if (seg.size() != 3 || !seg.get(0).equals("prepare") || !seg.get(2).equals("prepare.log"))
            return null;
        String tid = seg.get(1);
        if (!preparedKey(tid))
            return null;
        File f = new File(new File(new File(c.getFilesDir(), "prepare"), tid), "prepare.log");
        try {
            File root = new File(c.getFilesDir(), "prepare").getCanonicalFile();
            if (!f.getCanonicalPath().startsWith(root.getPath() + File.separator))
                return null;
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode))
            throw new SecurityException("read-only");
        File f = file(uri);
        if (f == null || !f.isFile())
            throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = file(uri);
        if (f == null || !f.isFile())
            return null;
        String[] cols = projection != null ? projection
                                           : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i]))
                row[i] = uri.getPathSegments().get(0).equals("game") ? uri.getPathSegments().get(1)
                         : uri.getPathSegments().get(0).equals(LibraryCheck.DIR) ? LibraryCheck.RESULTS
                         : "prepare-" + uri.getPathSegments().get(1) + ".log";
            else if (OpenableColumns.SIZE.equals(cols[i]))
                row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
