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
 * content://<app id>.logs/prepare/<TITLEID>/prepare.log. Only files named prepare.log under <files>/prepare are
 * served. A framework ContentProvider rather than AndroidX's FileProvider (the app has no AndroidX).
 */
public class LogProvider extends ContentProvider {
    static String authority(Context c) {
        return c.getPackageName() + ".logs";
    }

    static Uri uriFor(Context c, String titleId) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath("prepare")
                .appendPath(titleId).appendPath("prepare.log").build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The file behind a URI, or null when it is not a Prepare log. */
    private File file(Uri uri) {
        Context c = getContext();
        if (c == null)
            return null;
        java.util.List<String> seg = uri.getPathSegments();
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
                row[i] = "prepare-" + uri.getPathSegments().get(1) + ".log";
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
