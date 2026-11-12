package fr.adserver.xboxrecomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * Scans the game library folder (docs: « Game library »): disc images (*.iso, *.xiso) and extracted game folders (a
 * directory holding default.xbe), into a LibraryIndex. Each game's default.xbe is copied once into the XBE cache
 * (<files>/library/xbe/<TID>-<sha8>/default.xbe; identical XBEs share one copy): out of an image through ImagePeek
 * (XisoNative.extractFile, a few MB read, nothing else extracted), out of a folder by a plain copy. Everything that
 * reads an XBE by path (title, art, theme, hash, the Prepare's source before import) then works on that copy.
 *
 * Walk: at most MAX_DEPTH levels below the folder and MAX_ENTRIES entries; names starting with « . » and the Android
 * directory are skipped; a game folder is not descended. A rescan reuses every item whose location, size and
 * modification time are unchanged without opening it (a stat-only rescan). Failures stay in the index with their
 * status (LibraryIndex.NOT_XISO, NO_XBE, BAD_XBE, UNREADABLE): the « Other files » line.
 *
 * The walk is plain Java over Tree and ImagePeek, so the JVM tests drive it with fakes. Production trees: PathTree
 * (java.io.File, with All files access) and DocTree (the Storage Access Framework). Runs on a background thread.
 */
final class LibraryScanner {
    private LibraryScanner() {}

    static final String TAG = "xboxrecomp";
    static final int MAX_DEPTH = 4, MAX_ENTRIES = 5000;
    /** The largest default.xbe taken (real ones: a few MB). */
    static final long MAX_XBE = 32L << 20;

    /** A file or directory of the library folder. loc: « file:/abs/path » or « doc:<documentId> ». */
    static final class Node {
        final String loc, name;
        final boolean dir;
        final long size, mtime;

        Node(String loc, String name, boolean dir, long size, long mtime) {
            this.loc = loc;
            this.name = name;
            this.dir = dir;
            this.size = size;
            this.mtime = mtime;
        }
    }

    /** An open file: a seekable fd for ImagePeek, or a stream. Closing it closes both. */
    interface Opened extends Closeable {
        int fd();

        InputStream stream() throws IOException;
    }

    interface Tree {
        Node root() throws IOException;

        List<Node> list(Node dir) throws IOException;

        Opened open(Node file) throws IOException;
    }

    /** Copies one file out of a disc image (XisoNative.extractFile); 0 or an XISO_E* code. */
    interface ImagePeek {
        int extractFile(int fd, String pathInImage, File out, long maxBytes);
    }

    /** Called for each game found (new or not), with its cached XBE: the title art and theme caches. */
    interface Art {
        void onGame(File xbe, String titleId, String title);
    }

    interface Listener {
        void onItem(LibraryIndex.Item item);
    }

    interface Cancel {
        boolean cancelled();
    }

    static final class Result {
        LibraryIndex index;
        /** Directory entries seen, images opened, items reused unchanged. */
        int entries, peeks, reused;
        boolean cancelled;
        long ms;

        int games() {
            return index.games().size();
        }
    }

    static Result scan(Tree t, LibraryIndex previous, ImagePeek peek, File xbeCacheDir, Cancel c) {
        return scan(t, previous, peek, xbeCacheDir, null, null, c);
    }

    /**
     * Scans t. previous (may be null) gives the fingerprints to reuse. art and listener may be null. On cancel the
     * result holds what was found so far, marked cancelled: the caller keeps its previous index.
     */
    static Result scan(Tree t, LibraryIndex previous, ImagePeek peek, File xbeCacheDir, Art art, Listener listener,
                       Cancel c) {
        long t0 = System.nanoTime();
        Walk w = new Walk(t, previous == null ? new LibraryIndex() : previous, peek, xbeCacheDir, art, listener, c);
        w.r.index = new LibraryIndex();
        try {
            Node root = t.root();
            w.r.index.root = root.loc;
            w.dir(root, 0);
        } catch (IOException e) {
            warn("launcher: library: cannot read the folder: " + e.getMessage());
        }
        w.r.index.scannedAt = System.currentTimeMillis();
        w.r.ms = (System.nanoTime() - t0) / 1_000_000;
        return w.r;
    }

    private static final class Walk {
        final Tree t;
        final LibraryIndex prev;
        final ImagePeek peek;
        final File cache;
        final Art art;
        final Listener listener;
        final Cancel cancel;
        final Result r = new Result();

        Walk(Tree t, LibraryIndex prev, ImagePeek peek, File cache, Art art, Listener listener, Cancel cancel) {
            this.t = t;
            this.prev = prev;
            this.peek = peek;
            this.cache = cache;
            this.art = art;
            this.listener = listener;
            this.cancel = cancel;
        }

        boolean stop() {
            if (!r.cancelled && cancel != null && cancel.cancelled())
                r.cancelled = true;
            return r.cancelled || r.index.truncated;
        }

        void dir(Node d, int depth) {
            if (stop())
                return;
            List<Node> kids;
            try {
                kids = t.list(d);
            } catch (IOException e) {
                warn("launcher: library: cannot list " + d.loc + ": " + e.getMessage());
                return;
            }
            Node xbe = null;
            for (Node k : kids) {
                if (!k.dir && k.name.equalsIgnoreCase("default.xbe"))
                    xbe = k;
            }
            if (xbe != null) {   // a game folder: not descended
                r.entries++;
                add(folder(d, xbe));
                return;
            }
            for (Node k : kids) {
                if (stop())
                    return;
                if (++r.entries > MAX_ENTRIES) {
                    r.index.truncated = true;
                    return;
                }
                if (k.name.startsWith(".") || k.name.equalsIgnoreCase("Android"))
                    continue;
                if (k.dir) {
                    if (depth + 1 < MAX_DEPTH)
                        dir(k, depth + 1);
                } else if (isImage(k.name)) {
                    add(image(k));
                }
            }
        }

        void add(LibraryIndex.Item i) {
            r.index.items.add(i);
            if (i.ok() && art != null) {
                try {
                    art.onGame(xbeOf(i), i.tid, i.title);
                } catch (RuntimeException e) {
                    warn("launcher: library: art of " + i.tid + ": " + e);
                }
            }
            if (listener != null)
                listener.onItem(i);
        }

        /** The cached XBE of an ok item (its xbe path is relative to the cache's parent, <files>/library). */
        File xbeOf(LibraryIndex.Item i) {
            return i.xbe.isEmpty() ? null : new File(cache.getParentFile(), i.xbe);
        }

        /** The previous item for this fingerprint, when its XBE copy is still there. */
        LibraryIndex.Item reuse(String loc, long size, long mtime) {
            LibraryIndex.Item p = prev.find(loc);
            if (p == null || !p.sameFingerprint(loc, size, mtime))
                return null;
            if (p.ok()) {
                File x = xbeOf(p);
                if (x == null || !x.isFile())
                    return null;
            }
            r.reused++;
            return p;
        }

        LibraryIndex.Item image(Node n) {
            LibraryIndex.Item old = reuse(n.loc, n.size, n.mtime);
            if (old != null)
                return old;
            LibraryIndex.Item i = item(n.loc, LibraryIndex.KIND_ISO, n.name, n.size, n.mtime);
            File tmp = tmpFile();
            if (tmp == null) {
                i.status = LibraryIndex.UNREADABLE;
                return i;
            }
            int rc;
            try (Opened o = t.open(n)) {
                r.peeks++;
                rc = peek.extractFile(o.fd(), "default.xbe", tmp, MAX_XBE);
            } catch (IOException | RuntimeException e) {
                warn("launcher: library: cannot open " + n.loc + ": " + e);
                rc = 1;
            }
            switch (rc) {
            case 0:
                cacheXbe(i, tmp, stripExtension(n.name));
                break;
            case XisoNative.ENOENT:
                i.status = LibraryIndex.NO_XBE;
                break;
            case XisoNative.EINVAL:
                i.status = LibraryIndex.BAD_XBE;   // larger than any real default.xbe
                break;
            case XisoNative.ENOTXISO:
            case XisoNative.ECORRUPT:
                i.status = LibraryIndex.NOT_XISO;
                break;
            default:
                i.status = LibraryIndex.UNREADABLE;
                break;
            }
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return i;
        }

        LibraryIndex.Item folder(Node d, Node xbe) {
            LibraryIndex.Item old = reuse(d.loc, xbe.size, xbe.mtime);
            if (old != null)
                return old;
            LibraryIndex.Item i = item(d.loc, LibraryIndex.KIND_FOLDER, d.name, xbe.size, xbe.mtime);
            File tmp = tmpFile();
            if (tmp == null) {
                i.status = LibraryIndex.UNREADABLE;
                return i;
            }
            try (Opened o = t.open(xbe); InputStream in = o.stream(); OutputStream out = new FileOutputStream(tmp)) {
                byte[] buf = new byte[1 << 16];
                long total = 0;
                int k;
                while ((k = in.read(buf)) > 0) {
                    total += k;
                    if (total > MAX_XBE)
                        throw new TooBig();
                    out.write(buf, 0, k);
                }
            } catch (TooBig e) {
                i.status = LibraryIndex.BAD_XBE;
            } catch (IOException | RuntimeException e) {
                warn("launcher: library: cannot read " + xbe.loc + ": " + e);
                i.status = LibraryIndex.UNREADABLE;
            }
            if (i.ok())
                cacheXbe(i, tmp, d.name);
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return i;
        }

        /** Parses tmp, hashes it and moves it to <cache>/<TID>-<sha8>/default.xbe (or drops it: already there). */
        void cacheXbe(LibraryIndex.Item i, File tmp, String fallbackTitle) {
            XbeInfo info = XbeInfo.parse(tmp);
            if (info == null) {
                i.status = LibraryIndex.BAD_XBE;
                return;
            }
            String sha;
            try {
                sha = GameStore.sha256(tmp);
            } catch (IOException e) {
                i.status = LibraryIndex.UNREADABLE;
                return;
            }
            String name = info.titleId + "-" + sha.substring(0, 8);
            File dir = new File(cache, name), dst = new File(dir, "default.xbe");
            if (!dst.isFile()) {
                if ((!(dir.mkdirs() || dir.isDirectory())) || !tmp.renameTo(dst)) {
                    i.status = LibraryIndex.UNREADABLE;
                    return;
                }
            }
            i.status = LibraryIndex.OK;
            i.tid = info.titleId;
            i.title = info.title.isEmpty() ? fallbackTitle : info.title;
            i.region = info.region;
            i.disc = info.disc;
            i.version = info.version;
            i.xbeSha256 = sha;
            i.xbe = cache.getName() + "/" + name + "/default.xbe";
        }

        File tmpFile() {
            if (!(cache.mkdirs() || cache.isDirectory()))
                return null;
            File f = new File(cache, ".scan-" + Thread.currentThread().getId() + ".tmp");
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            return f;
        }
    }

    private static final class TooBig extends IOException {
    }

    /** Log.w, also on a plain JVM (the tests), where android.util.Log is a stub that throws. */
    static void warn(String msg) {
        try {
            Log.w(TAG, msg);
        } catch (RuntimeException ignored) {
        }
    }

    private static LibraryIndex.Item item(String loc, String kind, String name, long size, long mtime) {
        LibraryIndex.Item i = new LibraryIndex.Item();
        i.loc = loc;
        i.kind = kind;
        i.name = name;
        i.size = size;
        i.mtime = mtime;
        return i;
    }

    static boolean isImage(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".iso") || n.endsWith(".xiso");
    }

    static String stripExtension(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    /**
     * Where a game's active source stands, for GameState: a folder seen through the Storage Access Framework must be
     * copied; anything listed is unreadable when the folder is not accessible (accessible: All files access in path
     * mode, the persisted permission in SAF mode).
     */
    static GameState.Source source(LibraryIndex.Item i, boolean accessible) {
        if (i == null)
            return GameState.Source.MISSING;
        if (!accessible)
            return GameState.Source.NO_ACCESS;
        if (LibraryIndex.KIND_FOLDER.equals(i.kind) && LibraryIndex.locFile(i.loc) == null)
            return GameState.Source.NEEDS_COPY;
        return GameState.Source.PRESENT;
    }

    // ---- Trees --------------------------------------------------------------------------------------------------

    /** The folder read as files (All files access): java.io.File, images opened with ParcelFileDescriptor. */
    static class PathTree implements Tree {
        final File root;

        PathTree(File root) {
            this.root = root;
        }

        static Node node(File f) {
            boolean dir = f.isDirectory();
            return new Node(LibraryIndex.fileLoc(f), f.getName(), dir, dir ? 0 : f.length(), f.lastModified());
        }

        @Override
        public Node root() throws IOException {
            if (!root.isDirectory())
                throw new IOException(root + " is not a readable folder");
            return node(root);
        }

        @Override
        public List<Node> list(Node dir) throws IOException {
            File d = LibraryIndex.locFile(dir.loc);
            File[] l = d == null ? null : d.listFiles();
            if (l == null)
                throw new IOException("cannot list " + dir.loc);
            Arrays.sort(l, (a, b) -> a.getName().compareTo(b.getName()));
            List<Node> out = new ArrayList<>(l.length);
            for (File f : l)
                out.add(node(f));
            return out;
        }

        @Override
        public Opened open(Node file) throws IOException {
            File f = LibraryIndex.locFile(file.loc);
            if (f == null)
                throw new IOException("not a file: " + file.loc);
            return openFile(f);
        }

        /** Tests substitute a stream-only opener (no ParcelFileDescriptor on a plain JVM). */
        Opened openFile(File f) throws IOException {
            return parcel(ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY));
        }
    }

    /** The folder through the Storage Access Framework (no All files access): one child query per directory. */
    static final class DocTree implements Tree {
        private static final String[] COLS = { DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED };
        final ContentResolver cr;
        final Uri tree;

        DocTree(ContentResolver cr, Uri tree) {
            this.cr = cr;
            this.tree = tree;
        }

        @Override
        public Node root() throws IOException {
            String id;
            try {
                id = DocumentsContract.getTreeDocumentId(tree);
            } catch (IllegalArgumentException e) {
                throw new IOException(e);
            }
            int colon = id.lastIndexOf(':'), slash = id.lastIndexOf('/');
            String name = id.substring(Math.max(colon, slash) + 1);
            return new Node(LibraryIndex.docLoc(id), name, true, 0, 0);
        }

        @Override
        public List<Node> list(Node dir) throws IOException {
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId(dir));
            List<Node> out = new ArrayList<>();
            try (Cursor c = cr.query(children, COLS, null, null, null)) {
                if (c == null)
                    throw new IOException("cannot list " + dir.loc);
                while (c.moveToNext()) {
                    String name = c.getString(1);
                    if (name == null)
                        continue;
                    boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                    out.add(new Node(LibraryIndex.docLoc(c.getString(0)), name, isDir,
                                     c.isNull(3) ? 0 : c.getLong(3), c.isNull(4) ? 0 : c.getLong(4)));
                }
            } catch (RuntimeException e) {   // SecurityException: the permission is gone
                throw new IOException(e);
            }
            out.sort((a, b) -> a.name.compareTo(b.name));
            return out;
        }

        @Override
        public Opened open(Node file) throws IOException {
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, docId(file));
            ParcelFileDescriptor p;
            try {
                p = cr.openFileDescriptor(doc, "r");
            } catch (RuntimeException e) {
                throw new IOException(e);
            }
            if (p == null)
                throw new IOException("cannot open " + file.loc);
            return parcel(p);
        }

        private static String docId(Node n) throws IOException {
            if (!n.loc.startsWith("doc:"))
                throw new IOException("not a document: " + n.loc);
            return n.loc.substring(4);
        }
    }

    static Opened parcel(ParcelFileDescriptor p) {
        return new Opened() {
            @Override
            public int fd() {
                return p.getFd();
            }

            @Override
            public InputStream stream() {
                return new FileInputStream(p.getFileDescriptor());
            }

            @Override
            public void close() throws IOException {
                p.close();
            }
        };
    }

    // ---- The app's scan -----------------------------------------------------------------------------------------

    /** The library folder's tree for this app: files with All files access and a path, else SAF; null when none. */
    static Tree folderTree(Context c) {
        return folderTree(c, Environment.isExternalStorageManager());
    }

    static Tree folderTree(Context c, boolean allFilesAccess) {
        String path = Library.folderPath(c);
        if (path != null && allFilesAccess)
            return new PathTree(new File(path));
        String tree = Library.folderTree(c);
        return tree == null ? null : new DocTree(c.getContentResolver(), Uri.parse(tree));
    }

    /** The production ImagePeek: libxba_import.so. */
    static final ImagePeek NATIVE_PEEK = (fd, path, out, max) ->
            XisoNative.extractFile(fd, path, out.getAbsolutePath(), max);

    /** The production art hook: TitleArt's PNG cache, decoded once per title. */
    static Art titleArt(File filesDir) {
        return (xbe, tid, title) -> {
            if (xbe != null && !TitleArt.cacheFile(filesDir, tid).isFile())
                TitleArt.load(filesDir, xbe, tid);
        };
    }

    /**
     * The launcher's art hook: titleArt(), plus the title's game theme (GameTheme.ensure from the cached XBE, so the
     * library's glow and the game's Home have its colours before it is imported).
     */
    static Art titleArtAndTheme(Context c) {
        Art art = titleArt(c.getFilesDir());
        return (xbe, tid, title) -> {
            art.onGame(xbe, tid, title);
            if (xbe != null && !GameTheme.themeFile(c, tid).isFile())
                GameTheme.ensure(c, xbe.getParentFile(), tid, title);
        };
    }

    /**
     * Scans the app's library folder (on the calling thread: a worker) and, unless cancelled, writes index.json and
     * the scan time. Returns null when no folder is set.
     */
    static Result scanLibrary(Context c, Listener listener, Cancel cancel) {
        return scanLibrary(c, folderTree(c), listener, cancel);
    }

    static Result scanLibrary(Context c, Tree t, Listener listener, Cancel cancel) {
        if (t == null)
            return null;
        File files = c.getFilesDir();
        File indexFile = Library.indexFile(files);
        LibraryIndex prev = LibraryIndex.read(indexFile);
        long start = SystemClock.elapsedRealtime();
        Result r = scan(t, prev, NATIVE_PEEK, Library.xbeCacheDir(files), titleArtAndTheme(c), listener, cancel);
        if (!r.cancelled) {
            try {
                LibraryIndex.write(indexFile, r.index);
                Library.setScannedAt(c, r.index.scannedAt);
            } catch (IOException e) {
                warn("launcher: library: cannot write " + indexFile + ": " + e);
            }
        }
        Log.i(TAG, "launcher: library: scan " + r.index.items.size() + " items, " + r.games() + " games, "
                   + (SystemClock.elapsedRealtime() - start) + " ms (" + r.peeks + " opened, " + r.reused
                   + " unchanged" + (r.index.truncated ? ", entry cap reached" : "")
                   + (r.cancelled ? ", cancelled" : "") + ")");
        return r;
    }
}
