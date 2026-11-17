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
 * panel), content://<app id>.logs/game/game.log. Only files named prepare.log under <files>/prepare, and
 * <files>/logs/game.log, are served. A framework ContentProvider rather than AndroidX's FileProvider (the app has no AndroidX).
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
                row[i] = uri.getPathSegments().get(0).equals("game") ? "game.log"
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
