package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayDeque;
import java.util.Locale;

/**
 * Where the game theme's picture comes from (docs/android/app-shell.md « Game theme »): the player's own game files
 * and saves, never the APK. In order:
 * 1. default.xbe's « $$XTIMAGE » section (the title image, TitleArt);
 * 2. default.xbe's « $$XSIMAGE » section (the save image);
 * 3. a loose TitleImage.xbx, then SaveImage.xbx, in the game folder (any case): its root first, then a bounded walk
 *    (depth up to MAX_DEPTH, at most MAX_ENTRIES entries looked at, files up to MAX_BYTES);
 * 4. <save dir>/UDATA/<title id, lower case>/TitleImage.xbx, which the game writes next to its saves (the dashboard's
 *    copy of $$XTIMAGE; on Android the save dir is <files dir>/save, shell_main.c).
 * Only default.xbe is read: dashupdate.xbe and update.xbe hold dashboard media, not the game's image. The walk and
 * file reads are I/O: call from a worker thread.
 */
final class GameArt {
    private GameArt() {}

    static final String SAVE_SECTION = "$$XSIMAGE";
    static final String TITLE_FILE = "TitleImage.xbx", SAVE_FILE = "SaveImage.xbx";
    static final int MAX_DEPTH = 3, MAX_ENTRIES = 2000;
    static final long MAX_BYTES = 4 << 20;

    /** A decoded picture and where it came from (« xbe:$$XTIMAGE », « file:Media/TitleImage.xbx », ...). */
    static final class Found {
        final int[] argb;
        final int w, h;
        final String source;

        Found(int[] argb, int w, int h, String source) {
            this.argb = argb;
            this.w = w;
            this.h = h;
            this.source = source;
        }
    }

    /** The game's title image from the first source that has a readable one; null when none has. */
    static Found titleImage(File gameDir, File saveDir, String titleId) {
        File xbe = GameStore.findXbe(gameDir);
        Found f = section(xbe, TitleArt.SECTION);
        if (f == null)
            f = section(xbe, SAVE_SECTION);
        if (f == null)
            f = loose(gameDir, TITLE_FILE);
        if (f == null)
            f = loose(gameDir, SAVE_FILE);
        if (f == null)
            f = udata(saveDir, titleId, TITLE_FILE);
        return f;
    }

    /** The game's save image: $$XSIMAGE, a loose SaveImage.xbx, or the one under UDATA; null when none. */
    static Found saveImage(File gameDir, File saveDir, String titleId) {
        Found f = section(GameStore.findXbe(gameDir), SAVE_SECTION);
        if (f == null)
            f = loose(gameDir, SAVE_FILE);
        if (f == null)
            f = udata(saveDir, titleId, SAVE_FILE);
        return f;
    }

    private static Found decode(byte[] xpr, String source) {
        if (xpr == null)
            return null;
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(xpr, wh);
        return px == null ? null : new Found(px, wh[0], wh[1], source);
    }

    static Found section(File xbe, String name) {
        return xbe == null ? null : decode(TitleArt.xbeSection(xbe, name), "xbe:" + name);
    }

    static Found udata(File saveDir, String titleId, String name) {
        if (saveDir == null || titleId == null)
            return null;
        File d = new File(new File(saveDir, "UDATA"), titleId.toLowerCase(Locale.ROOT));
        File f = child(d, name);
        return f == null ? null : decode(read(f), "save:UDATA/" + d.getName() + "/" + f.getName());
    }

    /** name (any case) in the game folder: the root first, then breadth first down to MAX_DEPTH. */
    static Found loose(File gameDir, String name) {
        File f = findLoose(gameDir, name);
        if (f == null)
            return null;
        String rel = gameDir.toURI().relativize(f.toURI()).getPath();
        return decode(read(f), "file:" + rel);
    }

    static File findLoose(File root, String name) {
        if (root == null || !root.isDirectory())
            return null;
        ArrayDeque<File> dirs = new ArrayDeque<>();
        ArrayDeque<Integer> depths = new ArrayDeque<>();
        dirs.add(root);
        depths.add(0);
        int seen = 0;
        while (!dirs.isEmpty()) {
            File d = dirs.poll();
            int depth = depths.poll();
            File[] list = d.listFiles();
            if (list == null)
                continue;
            java.util.Arrays.sort(list);   // the same answer on every device
            for (File f : list) {
                if (++seen > MAX_ENTRIES)
                    return null;
                if (f.getName().equalsIgnoreCase(name) && f.isFile() && f.length() <= MAX_BYTES)
                    return f;
            }
            if (depth < MAX_DEPTH) {
                for (File f : list) {
                    if (f.isDirectory()) {
                        dirs.add(f);
                        depths.add(depth + 1);
                    }
                }
            }
        }
        return null;
    }

    private static File child(File dir, String name) {
        File[] list = dir.listFiles();
        if (list == null)
            return null;
        for (File f : list) {
            if (f.getName().equalsIgnoreCase(name) && f.isFile() && f.length() <= MAX_BYTES)
                return f;
        }
        return null;
    }

    private static byte[] read(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = r.length();
            if (n <= 0 || n > MAX_BYTES)
                return null;
            byte[] b = new byte[(int)n];
            r.readFully(b);
            return b;
        } catch (IOException e) {
            return null;
        }
    }
}
