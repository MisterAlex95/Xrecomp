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
import java.util.List;

/**
 * XRecomp's title packs, read-only, for a game's own app that PackHandover let in (XBA-176): content://<app id>.packs/
 * <TITLEID>/<file>, where file is pack.json, its lib or one of its images. Not exported: only a URI that
 * PackHandoverActivity granted (FLAG_GRANT_READ_URI_PERMISSION, one URI per file) is readable by another app. A
 * framework ContentProvider rather than AndroidX's FileProvider (the app has no AndroidX), like LogProvider.
 *
 * The authority is package-derived, so TitleApk moves it to fr.adserver.xrecomp.t<tid>.packs in a game's own app:
 * the copy's provider never clashes with XRecomp's (and serves nothing, as a game's app refuses to hand over).
 */
public class PackProvider extends ContentProvider {
    static final String AUTHORITY_SUFFIX = ".packs";

    /** Tests: the files directory to serve from instead of the context's. */
    static File filesDirOverride;

    static String authority(Context c) {
        return c.getPackageName() + AUTHORITY_SUFFIX;
    }

    static File filesDir(Context c) {
        return filesDirOverride != null ? filesDirOverride : c.getFilesDir();
    }

    static Uri uriFor(Context c, String titleId, String name) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath(titleId).appendPath(name).build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The file behind uri, or null when it is not a file of a valid pack (8 hex digits, a name its pack.json gives). */
    static File servable(File filesDir, Uri uri) {
        List<String> seg = uri.getPathSegments();
        if (seg.size() != 2)
            return null;
        String id = Pack.normTitleId(seg.get(0));
        if (id == null || !id.equals(seg.get(0)))
            return null;
        File dir = Pack.packDir(filesDir, id);
        List<String> names = PackHandover.servableNames(dir);
        if (names == null || !names.contains(seg.get(1)))
            return null;
        File f = new File(dir, seg.get(1));
        try {
            if (!f.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator))
                return null;
        } catch (IOException e) {
            return null;
        }
        return f.isFile() ? f : null;
    }

    private File file(Uri uri) {
        Context c = getContext();
        return c == null ? null : servable(filesDir(c), uri);
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode))
            throw new SecurityException("read-only");
        File f = file(uri);
        if (f == null)
            throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f = file(uri);
        if (f == null)
            return null;
        String[] cols = projection != null ? projection
                                           : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i]))
                row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i]))
                row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return uri.getLastPathSegment() != null && uri.getLastPathSegment().equals(Pack.JSON) ? "application/json"
                                                                                               : "application/octet-stream";
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
