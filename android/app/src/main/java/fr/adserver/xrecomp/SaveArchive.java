package fr.adserver.xrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Saves in and out of the app as a zip (docs/android/app-shell.md « Saves out and back »; XBA cloud saves build on it).
 *
 * The archive mirrors <files>/save: UDATA/<tid>/..., TDATA/<tid>/... for an Xbox title (the kernel's lower case
 * folders), <console>-<KEY>/... for a GBA, PS1 or N64 game (Storage.saves), and MANIFEST first (format, app version,
 * date, and per title its key, name and content hash). Only save folders: never the caches, packs or game data.
 *
 * Import restores per title: every entry is checked (no absolute path, no « .. », only a save folder of a valid key,
 * bounded sizes), the archive is unpacked under save/.import-<n> first, then for each title in it the current save
 * folders move to save/.backup/<key>/ (one generation, replaced by the next import of that title) and the archive's
 * take their place, by rename. A title the archive has not is untouched. Plain Java (org.json).
 */
final class SaveArchive {
    private SaveArchive() {}

    static final String MANIFEST = "xrecomp-saves.json";
    static final int FORMAT = 1;
    static final String IMPORT_DIR = ".import", BACKUP_DIR = ".backup";
    /** Bounds against a hostile archive: one file, all files, and the number of entries. */
    static final long MAX_FILE = 256L << 20, MAX_TOTAL = 2L << 30;
    static final int MAX_ENTRIES = 100_000;
    private static final String[] XBOX_AREAS = { "UDATA", "TDATA" };

    /** What an export wrote or an import restored. */
    static final class Summary {
        final List<String> titles = new ArrayList<>();
        int files;
        long bytes;
        /** Import: entries refused (not a save folder, unsafe path). */
        int skipped;
    }

    /** The save folders of tid that exist (Storage.saves keeps the ones that do not, for the Storage rows). */
    static List<File> existingSaves(Storage.Dirs d, String tid) {
        List<File> l = new ArrayList<>();
        for (File f : Storage.saves(d, tid)) {
            if (f.isDirectory())
                l.add(f);
        }
        return l;
    }

    /** True when tid has at least one file in its save folders. */
    static boolean hasSaves(Storage.Dirs d, String tid) {
        for (File f : existingSaves(d, tid)) {
            if (hasFile(f))
                return true;
        }
        return false;
    }

    private static boolean hasFile(File f) {
        if (f.isFile())
            return true;
        File[] l = f.listFiles();
        if (l != null) {
            for (File c : l) {
                if (hasFile(c))
                    return true;
            }
        }
        return false;
    }

    /** Every title with saves (Storage.titles' keys that have any), sorted. */
    static List<String> titlesWithSaves(Storage.Dirs d, Library.Games g) {
        List<String> l = new ArrayList<>();
        for (String t : Storage.titles(d, g)) {
            if (hasSaves(d, t))
                l.add(t);
        }
        return l;
    }

    /**
     * The content hash of tid's saves: SHA-256 over every file's path (relative to save/, the area in upper case and
     * the folder in lower case, as an import writes them), size and bytes, in path order. Empty folders do not count.
     * Null when the title has no save file. What cloud sync compares.
     */
    static String hash(Storage.Dirs d, String tid) throws IOException {
        Map<String, File> files = files(d, tid);
        if (files.isEmpty())
            return null;
        MessageDigest md = sha256();
        byte[] buf = new byte[64 * 1024];
        for (Map.Entry<String, File> e : files.entrySet()) {
            md.update(e.getKey().getBytes(StandardCharsets.UTF_8));
            md.update((byte)0);
            md.update(Long.toString(e.getValue().length()).getBytes(StandardCharsets.US_ASCII));
            md.update((byte)0);
            try (InputStream in = new FileInputStream(e.getValue())) {
                int n;
                while ((n = in.read(buf)) > 0)
                    md.update(buf, 0, n);
            }
        }
        return hex(md.digest());
    }

    /** tid's save files by archive path (sorted): the area in upper case, the title folder in lower case. */
    static Map<String, File> files(Storage.Dirs d, String tid) {
        Map<String, File> out = new LinkedHashMap<>();
        TreeSet<String> names = new TreeSet<>();
        Map<String, File> all = new java.util.HashMap<>();
        for (File root : existingSaves(d, tid)) {
            String base = archiveRoot(d, root);
            if (base == null)
                continue;
            walk(root, base, all);
        }
        names.addAll(all.keySet());
        for (String n : names)
            out.put(n, all.get(n));
        return out;
    }

    /** The archive path of a save folder: UDATA/<tid lower case>, TDATA/..., or <console>-<KEY>. */
    private static String archiveRoot(Storage.Dirs d, File root) {
        File parent = root.getParentFile();
        if (parent == null)
            return null;
        if (parent.equals(d.saveDir()))
            return root.getName();
        if (parent.getParentFile() != null && parent.getParentFile().equals(d.saveDir()))
            return parent.getName().toUpperCase(Locale.ROOT) + "/" + root.getName().toLowerCase(Locale.ROOT);
        return null;
    }

    private static void walk(File f, String path, Map<String, File> out) {
        if (f.isFile()) {
            out.put(path, f);
            return;
        }
        String[] l = f.list();
        if (l == null)
            return;
        Arrays.sort(l);
        for (String n : l)
            walk(new File(f, n), path + "/" + n, out);
    }

    /**
     * Writes the saves of tids (those that have any) to out as a zip. names: a display name per key (may be empty).
     * The stream is finished, not closed.
     */
    static Summary export(Storage.Dirs d, Collection<String> tids, Map<String, String> names, String appVersion,
                          OutputStream out) throws IOException {
        Summary s = new Summary();
        Map<String, Map<String, File>> per = new LinkedHashMap<>();
        for (String t : tids) {
            Map<String, File> f = files(d, t);
            if (!f.isEmpty())
                per.put(t, f);
        }
        JSONObject m = new JSONObject();
        try {
            m.put("format", FORMAT);
            m.put("app", "XRecomp");
            m.put("version", appVersion == null ? "" : appVersion);
            m.put("created", System.currentTimeMillis());
            JSONArray titles = new JSONArray();
            for (String t : per.keySet()) {
                JSONObject o = new JSONObject();
                o.put("key", t);
                String name = names != null ? names.get(t) : null;
                o.put("name", name == null ? "" : name);
                o.put("hash", hash(d, t));
                titles.put(o);
            }
            m.put("titles", titles);
        } catch (JSONException e) {
            throw new IOException(e);
        }
        ZipOutputStream z = new ZipOutputStream(out);
        z.putNextEntry(new ZipEntry(MANIFEST));
        z.write(m.toString().getBytes(StandardCharsets.UTF_8));
        z.closeEntry();
        byte[] buf = new byte[64 * 1024];
        for (Map.Entry<String, Map<String, File>> t : per.entrySet()) {
            s.titles.add(t.getKey());
            for (Map.Entry<String, File> e : t.getValue().entrySet()) {
                ZipEntry ze = new ZipEntry(e.getKey());
                ze.setTime(e.getValue().lastModified());
                z.putNextEntry(ze);
                try (InputStream in = new FileInputStream(e.getValue())) {
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        z.write(buf, 0, n);
                        s.bytes += n;
                    }
                }
                z.closeEntry();
                s.files++;
            }
        }
        z.finish();
        return s;
    }

    /**
     * The title key an archive path belongs to, or null when it is not inside a save folder or not safe: UDATA/<8 hex
     * digits>/... (the Xbox title ID in upper case) or <console>-<KEY>/... of a non-Xbox console.
     */
    static String keyOf(String path) {
        String[] p = parts(path);
        if (p == null || p.length < 2 || !SaveCaches.safeName(p[0]))
            return null;
        for (String area : XBOX_AREAS) {
            if (area.equalsIgnoreCase(p[0]))
                return p[1].length() == 8 && SaveCaches.safeName(p[1]) ? Pack.normTitleId(p[1]) : null;
        }
        return Console.ofKey(p[0]) != Console.XBOX ? p[0] : null;
    }

    /**
     * The path's components, or null when the path is absolute, has a backslash, a colon or NUL, or a component is
     * empty, « . » or « .. ». keyOf checks the save folder's own names.
     */
    private static String[] parts(String path) {
        if (path == null || path.isEmpty() || path.startsWith("/") || path.indexOf('\\') >= 0
            || path.indexOf(':') >= 0 || path.indexOf('\0') >= 0)
            return null;
        String[] p = path.split("/", -1);
        if (p.length > 0 && p[p.length - 1].isEmpty())   // a folder entry: « UDATA/4d530004/ »
            p = Arrays.copyOf(p, p.length - 1);
        if (p.length == 0)
            return null;
        for (int i = 0; i < p.length; i++) {
            String c = p[i];
            if (c.isEmpty() || c.equals(".") || c.equals("..") || c.length() > 255)
                return null;
        }
        return p;
    }

    /** The local path an archive path is written to under a root: UDATA/<tid lower case>/rest. */
    private static File target(File root, String path) {
        String[] p = parts(path);
        StringBuilder b = new StringBuilder();
        boolean xbox = false;
        for (String area : XBOX_AREAS) {
            if (area.equalsIgnoreCase(p[0])) {
                b.append(area).append('/').append(p[1].toLowerCase(Locale.ROOT));
                xbox = true;
            }
        }
        if (!xbox)
            b.append(p[0]).append('/').append(p[1]);
        for (int i = 2; i < p.length; i++)
            b.append('/').append(p[i]);
        return new File(root, b.toString());
    }

    /** The titles of an unpacked archive: its keys, with the manifest's names when it has them. */
    static final class Unpacked {
        final File dir;
        final Map<String, String> names = new LinkedHashMap<>();
        final Summary summary = new Summary();

        Unpacked(File dir) {
            this.dir = dir;
        }

        List<String> keys() {
            return new ArrayList<>(names.keySet());
        }
    }

    /**
     * Unpacks a save archive under a folder of its own, save/.import-<n>, checking every entry. Throws (and leaves
     * nothing) when the stream is not a zip with at least one save folder, or breaks a bound.
     */
    static Unpacked unpack(Storage.Dirs d, InputStream in) throws IOException {
        sweep(d);
        File dir = new File(d.saveDir(), IMPORT_DIR + "-" + Long.toHexString(System.nanoTime()));
        if (!dir.mkdirs())
            throw new IOException("cannot create " + dir);
        try {
            return unpackInto(dir, in);
        } catch (IOException | RuntimeException e) {
            GameStore.deleteTree(dir);
            throw e;
        }
    }

    /** Import folders left by a process killed in the middle (older than a day; never one in use). */
    private static void sweep(Storage.Dirs d) {
        File[] l = d.saveDir().listFiles();
        long old = System.currentTimeMillis() - 24 * 3600_000L;
        for (int i = 0; l != null && i < l.length; i++) {
            if (l[i].getName().startsWith(IMPORT_DIR) && l[i].lastModified() < old)
                GameStore.deleteTree(l[i]);
        }
    }

    private static Unpacked unpackInto(File dir, InputStream in) throws IOException {
        Unpacked u = new Unpacked(dir);
        Map<String, String> manifestNames = new java.util.HashMap<>();
        TreeSet<String> keys = new TreeSet<>();
        long total = 0;
        int entries = 0;
        byte[] buf = new byte[64 * 1024];
        ZipInputStream z = new ZipInputStream(in);
        ZipEntry e;
        while ((e = z.getNextEntry()) != null) {
            if (++entries > MAX_ENTRIES)
                throw new IOException("too many entries");
            String name = e.getName();
            if (MANIFEST.equals(name)) {
                readManifest(z, manifestNames);
                continue;
            }
            String key = keyOf(name);
            if (key == null) {
                u.summary.skipped++;
                continue;
            }
            File f = target(dir, name);
            if (!Storage.inside(f, dir))
                throw new IOException("bad entry " + name);
            keys.add(key);
            if (e.isDirectory()) {
                //noinspection ResultOfMethodCallIgnored
                f.mkdirs();
                continue;
            }
            File p = f.getParentFile();
            if (p != null && !p.isDirectory() && !p.mkdirs())
                throw new IOException("cannot create " + p);
            long size = 0;
            try (OutputStream out = new FileOutputStream(f)) {
                int n;
                while ((n = z.read(buf)) > 0) {
                    size += n;
                    total += n;
                    if (size > MAX_FILE || total > MAX_TOTAL)
                        throw new IOException("archive too large");
                    out.write(buf, 0, n);
                }
            }
            if (e.getTime() > 0)
                //noinspection ResultOfMethodCallIgnored
                f.setLastModified(e.getTime());
            u.summary.files++;
            u.summary.bytes += size;
        }
        if (keys.isEmpty())
            throw new IOException("no saves in the archive");
        for (String k : keys) {
            String n = manifestNames.get(k);
            u.names.put(k, n == null ? "" : n);
        }
        return u;
    }

    private static void readManifest(InputStream in, Map<String, String> names) throws IOException {
        byte[] buf = new byte[8192];
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        int n;
        while ((n = in.read(buf)) > 0) {
            b.write(buf, 0, n);
            if (b.size() > (4 << 20))
                throw new IOException("manifest too large");
        }
        try {
            JSONObject m = new JSONObject(b.toString("UTF-8"));
            if (m.optInt("format", 0) > FORMAT)
                throw new IOException("a newer save archive (format " + m.optInt("format") + ")");
            JSONArray t = m.optJSONArray("titles");
            for (int i = 0; t != null && i < t.length(); i++) {
                JSONObject o = t.optJSONObject(i);
                if (o != null && !o.optString("key").isEmpty())
                    names.put(o.optString("key"), o.optString("name"));
            }
        } catch (JSONException e) {
            // a manifest we cannot read: the entries still say the titles
        }
    }

    /**
     * Puts the unpacked titles (all of u's keys, or only those in keys when not null) in place of the current saves:
     * the current folders to save/.backup/<key>/, the archive's in their place, by rename. Then removes u.dir.
     */
    static Summary install(Storage.Dirs d, Unpacked u, Collection<String> keys) throws IOException {
        Summary s = new Summary();
        try {
            for (String k : u.keys()) {
                if (keys != null && !keys.contains(k))
                    continue;
                installOne(d, u.dir, k);
                s.titles.add(k);
            }
        } finally {
            discard(u);
        }
        s.files = u.summary.files;
        s.bytes = u.summary.bytes;
        s.skipped = u.summary.skipped;
        return s;
    }

    /** Removes what unpack left (the import folder). */
    static void discard(Unpacked u) {
        if (u != null)
            GameStore.deleteTree(u.dir);
    }

    private static void installOne(Storage.Dirs d, File from, String key) throws IOException {
        File save = d.saveDir();
        File backup = new File(new File(save, BACKUP_DIR), key);
        List<File> current = existingSaves(d, key);
        if (!current.isEmpty()) {
            GameStore.deleteTree(backup);
            for (File f : current) {
                String rel = archiveRoot(d, f);
                File to = new File(backup, rel);
                File p = to.getParentFile();
                if (p != null && !p.isDirectory() && !p.mkdirs())
                    throw new IOException("cannot create " + p);
                if (!f.renameTo(to))
                    throw new IOException("cannot move " + f + " to " + to);
            }
        }
        List<String> roots = new ArrayList<>();
        if (Console.ofKey(key) != Console.XBOX) {
            roots.add(key);
        } else {
            for (String area : XBOX_AREAS)
                roots.add(area + "/" + key.toLowerCase(Locale.ROOT));
        }
        for (String r : roots) {
            File src = new File(from, r);
            if (!src.exists())
                continue;
            File to = new File(save, r);
            File p = to.getParentFile();
            if (p != null && !p.isDirectory() && !p.mkdirs())
                throw new IOException("cannot create " + p);
            GameStore.deleteTree(to);   // moved to the backup above; only a leftover of the other case remains
            if (!src.renameTo(to))
                throw new IOException("cannot move " + src + " to " + to);
        }
    }

    /** A save archive's file name: xrecomp-saves-<key or « all »>-<yyyymmdd-hhmm>.zip. */
    static String fileName(String key, long now) {
        java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("yyyyMMdd-HHmm", Locale.ROOT);
        return "xrecomp-saves-" + (key == null ? "all" : key.toLowerCase(Locale.ROOT)) + "-"
               + f.format(new java.util.Date(now)) + ".zip";
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder(b.length * 2);
        for (byte x : b)
            s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return s.toString();
    }
}
