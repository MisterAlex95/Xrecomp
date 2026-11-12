package fr.adserver.xboxrecomp;

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
            File lsfg = new File(c.getFilesDir(), "lsfg").getCanonicalFile();
            if (f.getCanonicalPath().startsWith(lsfg.getPath() + File.separator))
                return null;
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    private static File servable(Context c, Uri uri) {
        java.util.List<String> seg = uri.getPathSegments();
        if (seg.size() == 2 && seg.get(0).equals("game") && seg.get(1).equals("game.log"))
            return gameLogFile(c);
        if (seg.size() != 3 || !seg.get(0).equals("prepare") || !seg.get(2).equals("prepare.log"))
            return null;
        String tid = seg.get(1);
        if (!tid.matches("[0-9A-Fa-f]{1,8}"))
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
