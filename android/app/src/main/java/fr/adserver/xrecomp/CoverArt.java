package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLEncoder;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * Cover art for the games of the other consoles (GBA, PS1, N64), which carry no title image of their own the way an
 * XBE does (TitleArt). Two sources, both kept on the device only, in TitleArt's cache (<files dir>/art/<key>.png):
 *
 * 1. Downloaded (« Download cover art », on by default): the box art of libretro-thumbnails, the public collection
 *    RetroArch uses (github.com/libretro-thumbnails), named after the No-Intro / Redump name of the game. The name
 *    is guessed from the game's file name (candidates()), which is how dumps are usually named. Nothing is sent but
 *    the file's name in the URL. A miss is remembered (<key>.miss) and asked again after MISS_RETRY_MS.
 * 2. Captured: when there is no art after CAPTURE_AFTER_MS of play, the shell keeps a frame of the game
 *    (<key>.shot.png). A downloaded cover, when one comes later, wins over it.
 *
 * No image is in the APK or the repo.
 */
final class CoverArt {
    private CoverArt() {}

    static final String BASE = "https://raw.githubusercontent.com/libretro-thumbnails/";
    static final long MISS_RETRY_MS = 7L * 24 * 3600_000L;
    static final long CAPTURE_AFTER_MS = 45_000;
    static final int MAX_BYTES = 4 << 20;
    static final String SETTING_FILE = "cover_download";

    /** The libretro-thumbnails repository of a console, or null (the Xbox has its own art). */
    static String system(Console c) {
        switch (c) {
        case GBA: return "Nintendo_-_Game_Boy_Advance";
        case PS1: return "Sony_-_PlayStation";
        case N64: return "Nintendo_-_Nintendo_64";
        default: return null;
        }
    }

    /** libretro-thumbnails' file names: &*\/:`<>?\|" become _ (their README). */
    static String sanitize(String name) {
        StringBuilder b = new StringBuilder(name.length());
        for (char ch : name.toCharArray())
            b.append("&*/:`<>?\\|\"".indexOf(ch) >= 0 ? '_' : ch);
        return b.toString().trim();
    }

    /** The URL of the box art called name for console c. */
    static String url(Console c, String name) {
        String file;
        try {
            file = URLEncoder.encode(sanitize(name) + ".png", "UTF-8").replace("+", "%20");
        } catch (java.io.UnsupportedEncodingException e) {
            throw new AssertionError(e);
        }
        return BASE + system(c) + "/master/Named_Boxarts/" + file;
    }

    /** The file name without its folder and extension (« Crash Bandicoot (Europe) (EDC) »). */
    static String baseName(String fileName) {
        String n = fileName == null ? "" : fileName;
        int slash = Math.max(n.lastIndexOf('/'), n.lastIndexOf('\\'));
        if (slash >= 0)
            n = n.substring(slash + 1);
        int dot = n.lastIndexOf('.');
        if (dot > 0)
            n = n.substring(0, dot);
        return n.trim();
    }

    /**
     * The names to try, best first: the file's own name (a No-Intro / Redump dump), then without the bracketed tags
     * a dumper adds ([!], [b1]) and the tags libretro does not keep for the art ((EDC), (Track 1)), then the bare
     * title with the usual regions. Empty for no name.
     */
    static List<String> candidates(String fileName) {
        String base = baseName(fileName).replace('_', ' ').replaceAll("\\s+", " ").trim();
        if (base.isEmpty())
            return Collections.emptyList();
        Set<String> out = new LinkedHashSet<>();
        out.add(base);
        String noBrackets = base.replaceAll("\\s*\\[[^\\]]*\\]", "").trim();
        out.add(noBrackets);
        String noExtras = noBrackets.replaceAll("\\s*\\((EDC|Track \\d+|Rev \\d+|v\\d+(\\.\\d+)*)\\)", "").trim();
        out.add(noExtras);
        int paren = noExtras.indexOf(" (");
        String title = paren > 0 ? noExtras.substring(0, paren).trim() : noExtras;
        String disc = "";
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\((Disc \\d+)\\)").matcher(noExtras);
        if (m.find())
            disc = " (" + m.group(1) + ")";
        for (String region : new String[] { "USA", "Europe", "World", "Japan", "France", "Germany", "USA, Europe" })
            out.add(title + " (" + region + ")" + disc);
        out.add(title);
        out.remove("");
        return new ArrayList<>(out);
    }

    // ---- Files ------------------------------------------------------------------------------------------------------

    static File artFile(File filesDir, String key) {
        return TitleArt.cacheFile(filesDir, key);
    }

    static File shotFile(File filesDir, String key) {
        return new File(artFile(filesDir, key).getParentFile(), key + ".shot.png");
    }

    static File missFile(File filesDir, String key) {
        return new File(artFile(filesDir, key).getParentFile(), key + ".miss");
    }

    /** The game's cover: the downloaded one, else the captured frame; null when there is neither. */
    static Bitmap load(File filesDir, String key) {
        for (File f : new File[] { artFile(filesDir, key), shotFile(filesDir, key) }) {
            if (f.isFile()) {
                Bitmap b = BitmapFactory.decodeFile(f.getPath());
                if (b != null)
                    return b;
            }
        }
        return null;
    }

    static boolean downloadOn(Context c) {
        return !"0".equals(GameStore.readSmall(c, SETTING_FILE));
    }

    static boolean setDownloadOn(Context c, boolean on) {
        return GameStore.writeSmall(c, SETTING_FILE, on ? "1" : "0");
    }

    /** Whether to ask the network for this game now: no downloaded cover yet, and no recent miss. */
    static boolean wanted(File filesDir, String key, long now) {
        if (system(Console.ofKey(key)) == null || artFile(filesDir, key).isFile())
            return false;
        File miss = missFile(filesDir, key);
        return !miss.isFile() || now - miss.lastModified() > MISS_RETRY_MS;
    }

    // ---- Download ---------------------------------------------------------------------------------------------------

    /**
     * Looks for the game's box art (worker thread) and stores it as <key>.png. True when one was found; a miss writes
     * <key>.miss. A network error writes nothing, so the next scan tries again.
     */
    static boolean fetch(VkDriverIndex.Http http, File filesDir, String key, String fileName) {
        Console c = Console.ofKey(key);
        if (system(c) == null)
            return false;
        for (String name : candidates(fileName)) {
            byte[] png;
            try {
                png = get(http, url(c, name));
            } catch (IOException e) {
                Log.i(LauncherActivity.TAG, "launcher: cover art " + key + ": " + e.getMessage());
                return false;
            }
            if (png == null)
                continue;
            if (store(png, artFile(filesDir, key))) {
                Log.i(LauncherActivity.TAG, "launcher: cover art " + key + ": " + name);
                missFile(filesDir, key).delete();
                return true;
            }
        }
        Log.i(LauncherActivity.TAG, "launcher: cover art " + key + ": none for " + baseName(fileName));
        File miss = missFile(filesDir, key);
        try {
            File dir = miss.getParentFile();
            if (dir != null)
                dir.mkdirs();
            new FileOutputStream(miss).close();
            miss.setLastModified(System.currentTimeMillis());
        } catch (IOException ignored) {   // asked again next time
        }
        return false;
    }

    /** The body of a 200, null for a 404; IOException for anything else (offline, rate limited). */
    private static byte[] get(VkDriverIndex.Http http, String url) throws IOException {
        VkDriverIndex.Response r = http.get(url, Collections.singletonMap("User-Agent", "XRecomp"));
        try {
            if (r.code() == 404)
                return null;
            if (r.code() != 200)
                throw new IOException("HTTP " + r.code());
            try (InputStream in = r.body()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_BYTES)
                        throw new IOException("too large");
                }
                return out.toByteArray();
            }
        } finally {
            r.close();
        }
    }

    /** Writes an image that decodes (temporary file, then rename); false when it is not one. */
    static boolean store(byte[] data, File f) {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(data, 0, data.length, o);
        if (o.outWidth <= 0 || o.outHeight <= 0)
            return false;
        File dir = f.getParentFile();
        if (dir != null)
            dir.mkdirs();
        File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        return tmp.renameTo(f);
    }

    /** Keeps a frame of the game as its cover when it has none (the shell, CAPTURE_AFTER_MS into the game). */
    static boolean storeShot(Bitmap frame, File filesDir, String key) {
        if (frame == null || artFile(filesDir, key).isFile() || shotFile(filesDir, key).isFile())
            return false;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        if (!frame.compress(Bitmap.CompressFormat.PNG, 100, out))
            return false;
        return store(out.toByteArray(), shotFile(filesDir, key));
    }
}
