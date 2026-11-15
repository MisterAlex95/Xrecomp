package fr.adserver.xrecomp;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.BooleanSupplier;

/**
 * A GBA, PS1 or N64 game's own app (XBA-215, docs/android/app-shell.md « A console game's own app »): « Create an app »
 * of a console game puts the game's files (the ROM, or a disc: its image, or its .cue and tracks) and its prepared
 * pack, when it has one, into the generated APK as stored assets. The game's app takes them out into its own storage
 * at its first start (and after it is made again), then goes straight into the game: no XRecomp needed to play it,
 * no file access to ask for, no Prepare.
 *
 * In the APK: MANIFEST (JSON: format, key, title, rom: the file the game starts from, files: every game file's name and
 * size, pack: the pack's file names, pack.json first), GAME_ASSETS + each game file, PACK_ASSETS + each pack file. In
 * the game's app: files/game/ (the game files; a .cue's FILE lines rewritten to the tracks' paths there), the pack
 * where the Prepare would have put it (packs/&lt;console&gt;-&lt;key&gt;), and files/game/installed.json last, which
 * names the APK it came from (its last update time): another one takes the files out again.
 */
final class ConsoleApp {
    private ConsoleApp() {}

    static final String MANIFEST = "assets/xba-game.json";
    static final String GAME_ASSETS = "assets/xba-game/", PACK_ASSETS = "assets/xba-pack/";
    static final String DIR = "game", INSTALLED = "installed.json";
    static final int FORMAT = 1;
    /** Free space kept beyond the files while taking them out. */
    static final long SPACE_MARGIN = 32L << 20;

    /** The console game this app was made for (its library key), or null: XRecomp, or an Xbox game's app. */
    static String ownKey(Context c) {
        String k = TitleApk.normKey(TitleApp.ownTitleId(c));
        return k != null && Console.ofKey(k) != Console.XBOX ? k : null;
    }

    /** A file name that may be an entry's last part and a file of files/game: no separator, not . or .., not hidden. */
    static boolean safeName(String n) {
        return n != null && !n.isEmpty() && n.getBytes(StandardCharsets.UTF_8).length <= 200 && !n.startsWith(".")
               && n.indexOf('/') < 0 && n.indexOf('\\') < 0 && n.indexOf(0) < 0;
    }

    // ---- XRecomp's side: what goes into the APK -------------------------------------------------------------------

    /** One game file: its name in the game's folder, its size, how to read it. */
    static final class GameFile {
        final String name;
        final long size;
        final ApkZip.Opener opener;

        GameFile(String name, long size, ApkZip.Opener opener) {
            this.name = name;
            this.size = size;
            this.opener = opener;
        }
    }

    /**
     * The entries added to the APK of key's app: the manifest, files (files.get(0) is the one the game starts from),
     * then the pack in packDir when it holds a valid one (null: none). Throws when a name is not a safe one or comes
     * twice.
     */
    static List<ApkZip.Added> entries(String key, String title, List<GameFile> files, File packDir)
            throws IOException {
        if (files.isEmpty())
            throw new IOException("no game file");
        JSONObject m = new JSONObject();
        List<ApkZip.Added> out = new ArrayList<>();
        try {
            m.put("format", FORMAT).put("key", key).put("title", title == null ? "" : title);
            m.put("rom", files.get(0).name);
            JSONArray fa = new JSONArray();
            java.util.Set<String> seen = new java.util.HashSet<>();
            for (GameFile f : files) {
                if (!safeName(f.name) || !seen.add(f.name.toLowerCase(Locale.ROOT)) || f.name.equals(INSTALLED))
                    throw new IOException("game file name: " + f.name);
                fa.put(new JSONObject().put("name", f.name).put("size", f.size));
            }
            m.put("files", fa);
            JSONArray pa = new JSONArray();
            List<String> pack = packDir == null ? null : PackHandover.servableNames(packDir);
            if (pack != null)
                for (String n : pack)
                    pa.put(n);
            m.put("pack", pa);
            byte[] json = m.toString(1).getBytes(StandardCharsets.UTF_8);
            out.add(new ApkZip.Added(MANIFEST, json.length, () -> new java.io.ByteArrayInputStream(json)));
            for (GameFile f : files)
                out.add(new ApkZip.Added(GAME_ASSETS + f.name, f.size, f.opener));
            if (pack != null)
                for (String n : pack) {
                    File pf = new File(packDir, n);
                    out.add(new ApkZip.Added(PACK_ASSETS + n, pf.length(), () -> new FileInputStream(pf)));
                }
        } catch (JSONException e) {
            throw new IOException(e);
        }
        return out;
    }

    // ---- The game's app's side: taking them out -------------------------------------------------------------------

    /** The game as it is taken out: the file the game starts from, the title, the key. */
    static final class Installed {
        final String rom, title, key;

        Installed(String rom, String title, String key) {
            this.rom = rom;
            this.title = title;
            this.key = key;
        }
    }

    static File gameDir(File filesDir) {
        return new File(filesDir, DIR);
    }

    /** What filesDir holds of key's game, taken out of the APK stamped stamp; null when it must be taken out (again). */
    static Installed installed(File filesDir, String key, String stamp) {
        File dir = gameDir(filesDir);
        JSONObject o;
        try {
            o = new JSONObject(PrepareProtocol.readText(new File(dir, INSTALLED), 1 << 16));
        } catch (IOException | JSONException e) {
            return null;
        }
        String rom = o.optString("rom", "");
        if (!key.equals(o.optString("key", "")) || !stamp.equals(o.optString("apk", "")) || !safeName(rom)
            || !new File(dir, rom).isFile())
            return null;
        return new Installed(new File(dir, rom).getAbsolutePath(), o.optString("title", ""), key);
    }

    interface Progress {
        void update(long done, long total);
    }

    /** Taking out failed for want of space: need bytes. */
    static final class NoSpace extends IOException {
        final long need;

        NoSpace(long need) {
            super("not enough space: " + need + " bytes needed");
            this.need = need;
        }
    }

    /**
     * Takes key's game out of apk into filesDir (see the class comment): the game files into game.partial/, renamed
     * game/ once whole; the pack into packs/&lt;name&gt;.partial, published as the Prepare does; installed.json last.
     */
    static synchronized Installed install(File apk, File filesDir, String key, String stamp, Progress p,
                                          BooleanSupplier cancelled)
            throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(apk, "r")) {
            Map<String, ApkZip.Entry> entries = new HashMap<>();
            for (ApkZip.Entry e : ApkZip.entries(f))
                entries.put(e.name, e);
            ApkZip.Entry me = entries.get(MANIFEST);
            if (me == null)
                throw new IOException("this app has no game in it");
            JSONObject m;
            try {
                m = new JSONObject(new String(ApkZip.read(f, me), StandardCharsets.UTF_8));
            } catch (JSONException e) {
                throw new IOException(MANIFEST + ": " + e.getMessage());
            }
            if (m.optInt("format", -1) != FORMAT || !key.equals(m.optString("key", "")))
                throw new IOException(MANIFEST + ": made for " + m.optString("key", "?"));
            String rom = m.optString("rom", "");
            Map<String, ApkZip.Entry> game = new LinkedHashMap<>(), pack = new LinkedHashMap<>();
            JSONArray fa = m.optJSONArray("files"), pa = m.optJSONArray("pack");
            for (int i = 0; fa != null && i < fa.length(); i++) {
                String n = fa.optJSONObject(i) == null ? "" : fa.optJSONObject(i).optString("name", "");
                ApkZip.Entry e = entries.get(GAME_ASSETS + n);
                if (!safeName(n) || e == null || e.method != ApkZip.STORED)
                    throw new IOException("game file " + n);
                game.put(n, e);
            }
            for (int i = 0; pa != null && i < pa.length(); i++) {
                String n = pa.optString(i, "");
                ApkZip.Entry e = entries.get(PACK_ASSETS + n);
                if (!PackHandover.plainName(n) || e == null || e.method != ApkZip.STORED)
                    throw new IOException("pack file " + n);
                pack.put(n, e);
            }
            if (!game.containsKey(rom))
                throw new IOException("no " + rom);
            Console c = Console.ofKey(key);
            String packName = Console.packDirName(c, key.substring(c.id.length() + 1));
            if (packName == null)
                throw new IOException("key " + key);
            long total = 0;
            for (ApkZip.Entry e : game.values())
                total += e.size;
            for (ApkZip.Entry e : pack.values())
                total += e.size;
            if (!(filesDir.mkdirs() || filesDir.isDirectory()))
                throw new IOException("cannot create " + filesDir);
            if (filesDir.getUsableSpace() < total + SPACE_MARGIN)
                throw new NoSpace(total + SPACE_MARGIN);

            long[] done = { 0 };
            File dir = gameDir(filesDir), part = new File(filesDir, DIR + ".partial");
            GameStore.deleteTree(part);
            if (!part.mkdirs())
                throw new IOException("cannot create " + part);
            try {
                for (Map.Entry<String, ApkZip.Entry> e : game.entrySet())
                    extract(f, e.getValue(), new File(part, e.getKey()), done, total, p, cancelled);
                if (!pack.isEmpty()) {
                    File packs = Pack.packsDir(filesDir), pp = new File(packs, packName + ".partial");
                    if (!(packs.mkdirs() || packs.isDirectory()))
                        throw new IOException("cannot create " + packs);
                    PackHandover.recover(filesDir, packName);
                    GameStore.deleteTree(pp);
                    if (!pp.mkdirs())
                        throw new IOException("cannot create " + pp);
                    for (Map.Entry<String, ApkZip.Entry> e : pack.entrySet())   // pack.json first: written last below
                        if (!e.getKey().equals(Pack.JSON))
                            extract(f, e.getValue(), new File(pp, e.getKey()), done, total, p, cancelled);
                    ApkZip.Entry json = pack.get(Pack.JSON);
                    if (json != null)
                        extract(f, json, new File(pp, Pack.JSON), done, total, p, cancelled);
                    PackHandover.publish(filesDir, packName);
                }
            } catch (IOException | RuntimeException e) {
                GameStore.deleteTree(part);
                GameStore.deleteTree(new File(Pack.packsDir(filesDir), packName + ".partial"));
                throw e;
            }
            String start = new File(dir, rom).getAbsolutePath();
            if (rom.toLowerCase(Locale.ROOT).endsWith(".cue"))
                rewriteCue(new File(part, rom), dir);
            GameStore.deleteTree(dir);
            if (!part.renameTo(dir))
                throw new IOException("cannot rename " + part);
            String title = m.optString("title", "");
            try {
                JSONObject inst = new JSONObject().put("key", key).put("apk", stamp).put("rom", rom).put("title", title);
                File tmp = new File(dir, INSTALLED + ".tmp");
                try (FileOutputStream out = new FileOutputStream(tmp)) {
                    out.write(inst.toString().getBytes(StandardCharsets.UTF_8));
                    out.getFD().sync();
                }
                if (!tmp.renameTo(new File(dir, INSTALLED)))
                    throw new IOException("cannot rename " + tmp);
            } catch (JSONException e) {
                throw new IOException(e);
            }
            return new Installed(start, title, key);
        }
    }

    /** A .cue's FILE lines pointed at its tracks in dir (where the folder will be), whatever their case. */
    private static void rewriteCue(File cue, File dir) throws IOException {
        if (cue.length() > Ps1Cue.MAX_CUE)
            return;
        byte[] b = new byte[(int)cue.length()];
        try (FileInputStream in = new FileInputStream(cue)) {
            int n = 0, r;
            while (n < b.length && (r = in.read(b, n, b.length - n)) > 0)
                n += r;
        }
        String text = new String(b, StandardCharsets.ISO_8859_1);
        Map<String, String> paths = new HashMap<>();
        File[] ls = cue.getParentFile() == null ? null : cue.getParentFile().listFiles();
        if (ls != null)
            for (File t : ls)
                paths.put(t.getName().toLowerCase(Locale.ROOT), new File(dir, t.getName()).getAbsolutePath());
        String out = Ps1Cue.rewrite(text, paths);
        if (out == null)
            return;   // a track it names is not there: left as it is (the runtime says which)
        try (FileOutputStream o = new FileOutputStream(cue)) {
            o.write(out.getBytes(StandardCharsets.ISO_8859_1));
            o.getFD().sync();
        }
    }

    private static void extract(RandomAccessFile f, ApkZip.Entry e, File dst, long[] done, long total, Progress p,
                                BooleanSupplier cancelled) throws IOException {
        byte[] buf = new byte[1 << 20];
        java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        try (FileOutputStream out = new FileOutputStream(dst)) {
            f.seek(e.dataOffset);
            long left = e.size;
            while (left > 0) {
                if (cancelled != null && cancelled.getAsBoolean())
                    throw new InterruptedIOException("cancelled");
                int n = f.read(buf, 0, (int)Math.min(buf.length, left));
                if (n < 0)
                    throw new IOException(e.name + ": short read");
                out.write(buf, 0, n);
                crc.update(buf, 0, n);
                left -= n;
                done[0] += n;
                if (p != null)
                    p.update(done[0], total);
            }
            out.getFD().sync();
        }
        if (crc.getValue() != e.crc)
            throw new IOException(e.name + ": damaged (CRC)");
    }
}
