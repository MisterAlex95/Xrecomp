package fr.adserver.xrecomp;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The on-device Prepare's tools (docs/android/prepare.md, « The app side »): what the APK carries and where it is
 * installed.
 *
 * - In nativeLibraryDir (extracted at install, so they can be exec'd): libxbapython.so (the CPython launcher),
 *   libxbacc.so (clang + lld, one multicall binary), libpython3.13.so, libcapstone.so; and libmain.so, the shell.
 * - In the APK's assets, three zips: the Python standard library, the clang sysroot, and the toolkit (tools/, with a
 *   VERSION file). They are extracted once per version into <files>/python-<ver>, <files>/sysroot-<ver> and
 *   <files>/toolkit-<ver>, where <ver> is the first 12 hex digits of the zip's SHA-256; older versions are deleted.
 *   An optional sidecar <zip>.sha256 is checked against the zip before extracting.
 *
 * Without the two executables or the three zips (a shell built without the toolchain), the Prepare feature is hidden
 * and the launcher behaves as before.
 */
final class Toolchain {
    private Toolchain() {}

    static final String TAG = "xboxrecomp";
    static final String PYTHON = "libxbapython.so", CC = "libxbacc.so", SHELL_LIB = "libmain.so";

    /** The three asset kinds, the directory prefix each one is installed under, and how its zip is recognised. */
    enum Kind {
        PYTHON("python", "python-stdlib.zip", "stdlib"),
        SYSROOT("sysroot", "sysroot.zip", "sysroot"),
        TOOLKIT("toolkit", "toolkit.zip", "toolkit");

        final String dirPrefix, assetName, keyword;

        Kind(String dirPrefix, String assetName, String keyword) {
            this.dirPrefix = dirPrefix;
            this.assetName = assetName;
            this.keyword = keyword;
        }
    }

    /** The asset folder the zips are looked for in first (then the assets' root). */
    static final String ASSET_DIR = "prepare";
    static final String COMPLETE = ".xba-complete";

    /** Where the APK's assets come from: AssetManager, or a test's files. */
    interface AssetSource {
        String[] list(String dir) throws IOException;

        InputStream open(String path) throws IOException;
    }

    // Test seams (JVM unit tests): a fake nativeLibraryDir and fake assets.
    static volatile String nativeDirOverride;
    static volatile AssetSource assetsOverride;

    static File nativeDir(Context c) {
        String d = nativeDirOverride != null ? nativeDirOverride : c.getApplicationInfo().nativeLibraryDir;
        return d == null ? null : new File(d);
    }

    static AssetSource assets(Context c) {
        if (assetsOverride != null)
            return assetsOverride;
        AssetManager am = c.getAssets();
        return new AssetSource() {
            @Override
            public String[] list(String dir) throws IOException {
                return am.list(dir);
            }

            @Override
            public InputStream open(String path) throws IOException {
                return am.open(path, AssetManager.ACCESS_STREAMING);
            }
        };
    }

    static boolean libsPresent(Context c) {
        File d = nativeDir(c);
        return d != null && new File(d, PYTHON).isFile() && new File(d, CC).isFile();
    }

    /** The asset path of a kind's zip, or null: prepare/<name>, else any zip in prepare/ or the root naming it. */
    static String findAsset(AssetSource a, Kind k) {
        for (String dir : new String[] { ASSET_DIR, "" }) {
            String[] names;
            try {
                names = a.list(dir);
            } catch (IOException e) {
                continue;
            }
            if (names == null)
                continue;
            String found = null;
            for (String n : names) {
                String l = n.toLowerCase(Locale.ROOT);
                if (l.equals(k.assetName)) {
                    found = n;
                    break;
                }
                if (found == null && l.endsWith(".zip") && l.contains(k.keyword))
                    found = n;
            }
            if (found != null)
                return dir.isEmpty() ? found : dir + "/" + found;
        }
        return null;
    }

    static boolean assetsPresent(Context c) {
        AssetSource a = assets(c);
        for (Kind k : Kind.values()) {
            if (findAsset(a, k) == null)
                return false;
        }
        return true;
    }

    private static volatile String digestCache;
    /** The assetsOverride the cached digest was read with (null: the APK's own assets). */
    private static volatile AssetSource digestFrom;

    /**
     * The toolkit digest of this APK (XBA-127): the 64 hex digits of <toolkit zip>.id, which android/app/build.gradle
     * writes next to toolkit.zip (tools/prepare/toolkit.py, the same value as tools/TOOLKIT_SHA256 inside it); "" when
     * the APK has no toolkit or no digest (then no pack is judged by its toolkit). Read once per process.
     */
    static String toolkitDigest(Context c) {
        AssetSource over = assetsOverride, a = assets(c);
        String d = digestCache;
        if (d != null && digestFrom == over)
            return d;
        d = "";
        String zip = findAsset(a, Kind.TOOLKIT);
        if (zip != null) {
            try (InputStream in = a.open(zip + ".id")) {
                byte[] b = new byte[128];
                int n = in.read(b);
                String s = n <= 0 ? "" : new String(b, 0, n, StandardCharsets.US_ASCII).trim().toLowerCase(Locale.ROOT);
                if (LibraryScreen.isSha256(s))
                    d = s;
            } catch (IOException ignored) {
            }
        }
        digestFrom = over;
        digestCache = d;
        return d;
    }

    /** The Prepare feature is offered: the executables and the three zips are in this APK. */
    static boolean available(Context c) {
        return libsPresent(c) && assetsPresent(c);
    }

    // ---- Device facts -------------------------------------------------------------------------------------------

    static long totalRam(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null)
            return 0;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        return mi.totalMem;
    }

    static long availableRam(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        if (am == null)
            return 0;
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(mi);
        return mi.availMem;
    }

    /**
     * Parallel compile jobs, the formula of docs/android/prepare.md (« Measurements »): 512 MB per job after a 3 GB
     * reserve, (MemAvailable - 3 GB) / 512 MB, between 1 and min(5, cores). Passed explicitly (--jobs), so that the
     * retry after a kill can halve it.
     */
    static int jobs(Context c) {
        return jobsFor(availableRam(c), Runtime.getRuntime().availableProcessors());
    }

    static int jobsFor(long memAvailable, int cores) {
        long byRam = (memAvailable - (3L << 30)) / (512L << 20);
        return (int)Math.max(1, Math.min(Math.min(5, Math.max(1, cores)), byRam));
    }

    // ---- Install ------------------------------------------------------------------------------------------------

    static final class Installed {
        File python, sysroot, toolkit;
        String toolkitVersion = "";
        /** How many zips were extracted this time (0 on a second start). */
        int extracted;
    }

    interface Progress {
        /** bytes of the zips read so far of total; total 0 while nothing has to be extracted. */
        void onProgress(long done, long total, String what);

        boolean cancelled();
    }

    static final class MissingAssetException extends IOException {
        MissingAssetException(String m) {
            super(m);
        }
    }

    static final class HashMismatchException extends IOException {
        HashMismatchException(String m) {
            super(m);
        }
    }

    static final class CancelledException extends IOException {
        CancelledException() {
            super("cancelled");
        }
    }

    private static String stampKey(Context c, String asset) {
        long t = 0;
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            t = pi.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException | RuntimeException ignored) {
        }
        return asset + "@" + t;
    }

    /**
     * The zips' SHA-256, cached per APK install (lastUpdateTime): a second start hashes nothing. A new APK (update)
     * hashes again, and so extracts only the zips that changed.
     */
    private static String assetSha(Context c, AssetSource a, String asset) throws IOException {
        SharedPreferences p = c.getSharedPreferences("prepare_assets", Context.MODE_PRIVATE);
        String key = stampKey(c, asset);
        String sha = p.getString(key, null);
        if (sha != null && sha.matches("[0-9a-f]{64}"))
            return sha;
        try (InputStream in = a.open(asset)) {
            sha = GameStore.sha256(in);
        }
        SharedPreferences.Editor e = p.edit();
        String install = key.substring(key.lastIndexOf('@'));
        for (String k : p.getAll().keySet()) {
            if (!k.endsWith(install))
                e.remove(k);   // the stamps of older installs
        }
        e.putString(key, sha).commit();
        return sha;
    }

    private static String sidecarSha(AssetSource a, String asset) {
        try (InputStream in = a.open(asset + ".sha256")) {
            byte[] b = new byte[256];
            int n = in.read(b);
            if (n <= 0)
                return null;
            String s = new String(b, 0, n, StandardCharsets.US_ASCII).trim();
            int sp = s.indexOf(' ');
            if (sp > 0)
                s = s.substring(0, sp);
            return s.toLowerCase(Locale.ROOT);
        } catch (IOException e) {
            return null;
        }
    }

    static File installedDir(File filesDir, Kind k, String sha) {
        return new File(filesDir, k.dirPrefix + "-" + sha.substring(0, 12));
    }

    /** Extracts what is missing (on the calling thread; PrepareService's worker), and returns the three dirs. */
    static Installed install(Context c, File filesDir, Progress progress) throws IOException {
        AssetSource a = assets(c);
        Installed out = new Installed();
        Kind[] kinds = Kind.values();
        String[] assetPaths = new String[kinds.length];
        String[] shas = new String[kinds.length];
        File[] dirs = new File[kinds.length];
        for (int i = 0; i < kinds.length; i++) {
            assetPaths[i] = findAsset(a, kinds[i]);
            if (assetPaths[i] == null)
                throw new MissingAssetException("no " + kinds[i].assetName + " in this APK");
            shas[i] = assetSha(c, a, assetPaths[i]);
            String want = sidecarSha(a, assetPaths[i]);
            if (want != null && !want.equals(shas[i]))
                throw new HashMismatchException(assetPaths[i] + ": sha256 " + shas[i] + ", expected " + want);
            dirs[i] = installedDir(filesDir, kinds[i], shas[i]);
        }
        // What has to be extracted, for one progress over all of it.
        List<Integer> todo = new ArrayList<>();
        for (int i = 0; i < kinds.length; i++) {
            if (!isComplete(dirs[i], shas[i]))
                todo.add(i);
        }
        long total = 0;
        if (!todo.isEmpty()) {
            for (int i : todo)
                total += assetLength(a, assetPaths[i]);
        }
        long[] done = { 0 };
        for (int i : todo) {
            Log.i(TAG, "prepare: installing " + assetPaths[i] + " into " + dirs[i]);
            extract(a, assetPaths[i], dirs[i], shas[i], done, total, kinds[i], progress);
            out.extracted++;
        }
        for (int i = 0; i < kinds.length; i++)
            deleteOtherVersions(filesDir, kinds[i], dirs[i]);
        out.python = dirs[0];
        out.sysroot = dirs[1];
        out.toolkit = dirs[2];
        File v = new File(out.toolkit, "tools/VERSION");
        if (!v.isFile())
            v = new File(out.toolkit, "VERSION");
        if (v.isFile()) {
            try {
                out.toolkitVersion = PrepareProtocol.readText(v, 4096).trim();
            } catch (IOException ignored) {
            }
        }
        return out;
    }

    static boolean isComplete(File dir, String sha) {
        File stamp = new File(dir, COMPLETE);
        if (!stamp.isFile())
            return false;
        try {
            return PrepareProtocol.readText(stamp, 4096).trim().equals(sha);
        } catch (IOException e) {
            return false;
        }
    }

    private static long assetLength(AssetSource a, String path) {
        long n = 0;
        try (InputStream in = a.open(path)) {
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) > 0)
                n += r;
        } catch (IOException ignored) {
        }
        return n;
    }

    private static void extract(AssetSource a, String asset, File dir, String sha, long[] done, long total, Kind kind,
                                Progress progress) throws IOException {
        File tmp = new File(dir.getPath() + ".tmp");
        GameStore.deleteTree(tmp);
        GameStore.deleteTree(dir);
        if (!tmp.mkdirs())
            throw new IOException("cannot create " + tmp);
        String root = tmp.getCanonicalPath() + File.separator;
        byte[] buf = new byte[1 << 16];
        try (InputStream raw = a.open(asset);
             CountingStream counted = new CountingStream(new BufferedInputStream(raw, 1 << 16));
             ZipInputStream zip = new ZipInputStream(counted)) {
            ZipEntry e;
            long base = done[0];
            while ((e = zip.getNextEntry()) != null) {
                if (progress != null && progress.cancelled()) {
                    GameStore.deleteTree(tmp);
                    throw new CancelledException();
                }
                File f = new File(tmp, e.getName());
                String canon = f.getCanonicalPath();
                if (!canon.startsWith(root) && !(canon + File.separator).equals(root))
                    throw new IOException(asset + ": entry outside the target: " + e.getName());
                if (e.isDirectory()) {
                    if (!(f.mkdirs() || f.isDirectory()))
                        throw new IOException("cannot create " + f);
                    continue;
                }
                File parent = f.getParentFile();
                if (parent != null && !(parent.mkdirs() || parent.isDirectory()))
                    throw new IOException("cannot create " + parent);
                try (OutputStream o = new FileOutputStream(f)) {
                    int n;
                    while ((n = zip.read(buf)) > 0)
                        o.write(buf, 0, n);
                }
                done[0] = base + counted.count;
                if (progress != null)
                    progress.onProgress(done[0], total, kind.dirPrefix);
            }
            done[0] = base + counted.count;
        } catch (IOException e) {
            GameStore.deleteTree(tmp);
            throw e;
        }
        try (OutputStream o = new FileOutputStream(new File(tmp, COMPLETE))) {
            o.write(sha.getBytes(StandardCharsets.US_ASCII));
        }
        if (!tmp.renameTo(dir)) {
            GameStore.deleteTree(tmp);
            throw new IOException("cannot rename " + tmp + " to " + dir);
        }
    }

    private static void deleteOtherVersions(File filesDir, Kind k, File keep) {
        File[] list = filesDir.listFiles();
        if (list == null)
            return;
        for (File f : list) {
            String n = f.getName();
            if (f.isDirectory() && n.startsWith(k.dirPrefix + "-") && !f.equals(keep)) {
                Log.i(TAG, "prepare: removing the old " + f);
                GameStore.deleteTree(f);
            }
        }
    }

    private static final class CountingStream extends FilterInputStream {
        long count;

        CountingStream(InputStream in) {
            super(in);
        }

        @Override
        public int read() throws IOException {
            int b = super.read();
            if (b >= 0)
                count++;
            return b;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int n = super.read(b, off, len);
            if (n > 0)
                count += n;
            return n;
        }

        @Override
        public long skip(long n) throws IOException {
            long s = super.skip(n);
            count += s;
            return s;
        }
    }
}
