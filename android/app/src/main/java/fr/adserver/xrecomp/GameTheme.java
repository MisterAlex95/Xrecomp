package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The per-game launcher theme (docs/android/app-shell.md « Game theme »): the colours GamePalette derives from the
 * game's title image (GameArt), and the blurred hero backdrop, baked once and cached per title ID in app-private
 * storage, <files dir>/art/:
 * - <TID>.png the title image (as TitleArt caches it), <TID>.hero.png the hero (48 px, luminance-capped);
 * - <TID>.theme, text: v=<GamePalette.VERSION>, xbe=<length>:<lastModified>, source=..., hues=a,b,c, hero=0|1, then
 *   one TOKEN=AARRGGBB line per colour. Stale when v or the default.xbe fingerprint differs: computed again.
 * The player's choice per title (Settings › Launcher › Game theme): files/settings/theme_<TID>, « full » (art and
 * colours, the default), « colors » or « off ». The game process reads both (ShellActivity, tokens only).
 * Without a title ID there is no theme (stock colours).
 */
final class GameTheme {
    static final String TAG = LauncherActivity.TAG;

    enum Mode {
        FULL("full"), COLORS("colors"), OFF("off");

        final String key;

        Mode(String key) {
            this.key = key;
        }

        static Mode of(String key) {
            for (Mode m : values()) {
                if (m.key.equals(key))
                    return m;
            }
            return FULL;
        }
    }

    static final String MODE_FILE_PREFIX = "theme_";

    final String titleId;
    final Theme.Colors colors;
    /** Where the picture came from (GameArt.Found.source), or « none » (the title ID's hue). */
    final String source;
    /** The hero backdrop's PNG, or null (none baked, or the player chose « Colours only »). */
    final File heroFile;
    final Mode mode;
    /** The accent, backdrop and second-glow hues (NaN: no second glow). */
    final double accentHue, backdropHue, secondHue;

    private GameTheme(String titleId, Theme.Colors colors, String source, File heroFile, Mode mode, double[] hues) {
        this.titleId = titleId;
        this.colors = colors;
        this.source = source;
        this.heroFile = heroFile;
        this.mode = mode;
        this.accentHue = hues[0];
        this.backdropHue = hues[1];
        this.secondHue = hues[2];
    }

    // ---- Files ---------------------------------------------------------------------------------------------------

    static File artDir(Context c) {
        return new File(c.getFilesDir(), "art");
    }

    static File themeFile(Context c, String tid) {
        return new File(artDir(c), key(tid) + ".theme");
    }

    static File heroFile(Context c, String tid) {
        return new File(artDir(c), key(tid) + ".hero.png");
    }

    /** The game's saves (shell_main.c: <internal files dir>/save), where UDATA/<tid>/TitleImage.xbx may be. */
    static File saveDir(Context c) {
        return new File(c.getFilesDir(), "save");
    }

    private static String key(String tid) {
        return tid.toUpperCase(Locale.ROOT);
    }

    /** default.xbe's length and modification time: the cache is computed again when either changes. */
    static String fingerprint(File xbe) {
        return xbe == null ? "" : xbe.length() + ":" + xbe.lastModified();
    }

    // ---- The player's choice ---------------------------------------------------------------------------------------

    static Mode mode(Context c, String tid) {
        if (tid == null || tid.isEmpty())
            return Mode.FULL;
        return Mode.of(GameStore.readSettingFile(c, MODE_FILE_PREFIX + key(tid)));
    }

    static boolean setMode(Context c, String tid, Mode m) {
        if (tid == null || tid.isEmpty())
            return false;
        return GameStore.writeSettingFile(c, MODE_FILE_PREFIX + key(tid), m.key);
    }

    // ---- Cache -----------------------------------------------------------------------------------------------------

    /** The current game's theme from the cache (GameStore.chosenDir's default.xbe for the fingerprint). */
    static GameTheme load(Context c, String tid) {
        String dir = GameStore.chosenDir(c);
        return load(c, dir == null ? null : new File(dir), tid);
    }

    /**
     * The theme from the cache only (never decodes): null when there is none, it is stale, or the player turned it
     * off. The fingerprint is checked when gameDir has a default.xbe.
     */
    static GameTheme load(Context c, File gameDir, String tid) {
        if (tid == null)
            return null;
        Mode m = mode(c, tid);
        if (m == Mode.OFF)
            return null;
        return read(c, GameStore.findXbe(gameDir), tid, m);
    }

    /** The cached theme whatever the player's mode (Settings shows its source even when « Off »); null if none. */
    static GameTheme cached(Context c, File gameDir, String tid) {
        return tid == null ? null : read(c, GameStore.findXbe(gameDir), tid, mode(c, tid));
    }

    private static GameTheme read(Context c, File xbe, String tid, Mode m) {
        File f = themeFile(c, tid);
        if (!f.isFile())
            return null;
        Map<String, String> kv = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String l; (l = r.readLine()) != null;) {
                int eq = l.indexOf('=');
                if (eq > 0)
                    kv.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
            }
        } catch (IOException e) {
            return null;
        }
        if (!String.valueOf(GamePalette.VERSION).equals(kv.get("v")))
            return null;
        if (xbe != null && !fingerprint(xbe).equals(kv.get("xbe")))
            return null;
        try {
            int[] v = new int[GamePalette.COUNT];
            for (int i = 0; i < v.length; i++) {
                String s = kv.get(GamePalette.NAMES[i]);
                if (s == null)
                    return null;
                v[i] = (int)Long.parseLong(s, 16);
            }
            String[] h = String.valueOf(kv.get("hues")).split(",");
            double[] hues = new double[3];
            for (int i = 0; i < 3; i++)
                hues[i] = i < h.length && !h[i].equals("-") ? Double.parseDouble(h[i]) : Double.NaN;
            File hero = heroFile(c, tid);
            boolean hasHero = "1".equals(kv.get("hero")) && hero.isFile();
            return new GameTheme(tid, new Theme.Colors(v), String.valueOf(kv.get("source")),
                                 hasHero && m == Mode.FULL ? hero : null, m, hues);
        } catch (RuntimeException e) {   // a corrupt line: computed again
            return null;
        }
    }

    /**
     * The theme for the game in gameDir: from the cache, else decoded (GameArt), derived (GamePalette) and cached.
     * I/O and a few ms of work: a worker thread. null without a title ID, when the player turned the theme off (it is
     * still cached), or on any error (logged once; the launcher stays stock).
     */
    static GameTheme ensure(Context c, File gameDir, String tid, String title) {
        if (tid == null)
            return null;
        Mode m = mode(c, tid);
        File xbe = GameStore.findXbe(gameDir);
        GameTheme t = read(c, xbe, tid, m);
        if (t != null)
            return m == Mode.OFF ? null : t;
        try {
            t = compute(c, gameDir, xbe, tid, m);
            Log.i(TAG, t.logLine() + (title == null || title.isEmpty() ? "" : " (" + title + ")"));
            return m == Mode.OFF ? null : t;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "launcher: theme " + tid + ": failed, stock colours", e);
            return null;
        }
    }

    private static GameTheme compute(Context c, File gameDir, File xbe, String tid, Mode m) throws IOException {
        GameArt.Found art = GameArt.titleImage(gameDir, saveDir(c), tid);
        double fallbackHue = TitleArt.titleHue(tid);
        GamePalette.Palette p;
        GamePalette.Image hero = null;
        if (art != null) {
            p = GamePalette.build(GamePalette.analyse(art.argb, art.w, art.h), fallbackHue);
            hero = GamePalette.bakeHero(art.argb, art.w, art.h);
        } else {
            p = GamePalette.build(new GamePalette.Analysis(0, new java.util.ArrayList<>(), 0), fallbackHue);
        }
        File dir = artDir(c);
        if (!(dir.mkdirs() || dir.isDirectory()))
            throw new IOException("cannot create " + dir);
        if (art != null) {
            File png = TitleArt.cacheFile(c.getFilesDir(), key(tid));
            if (!png.isFile())
                writePng(Bitmap.createBitmap(art.argb, art.w, art.h, Bitmap.Config.ARGB_8888), png);
        }
        File heroPng = heroFile(c, tid);
        if (hero != null)
            writePng(Bitmap.createBitmap(hero.px, hero.w, hero.h, Bitmap.Config.ARGB_8888), heroPng);
        else
            heroPng.delete();
        String source = art != null ? art.source : "none";
        double[] hues = { p.accentHue, p.backdropHue, p.secondHue };
        StringBuilder sb = new StringBuilder();
        sb.append("v=").append(GamePalette.VERSION).append('\n');
        sb.append("xbe=").append(fingerprint(xbe)).append('\n');
        sb.append("source=").append(source).append('\n');
        sb.append("hues=").append(hue(hues[0])).append(',').append(hue(hues[1])).append(',').append(hue(hues[2]))
          .append('\n');
        sb.append("hero=").append(hero != null ? 1 : 0).append('\n');
        for (int i = 0; i < GamePalette.COUNT; i++)
            sb.append(GamePalette.NAMES[i]).append('=').append(String.format(Locale.ROOT, "%08X", p.colors[i]))
              .append('\n');
        File f = themeFile(c, tid), tmp = new File(f.getPath() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) {
            o.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (!tmp.renameTo(f))
            throw new IOException("cannot write " + f);
        return new GameTheme(tid, new Theme.Colors(p.colors), source,
                             hero != null && m == Mode.FULL ? heroPng : null, m, hues);
    }

    private static String hue(double h) {
        return Double.isNaN(h) ? "-" : String.format(Locale.ROOT, "%.1f", h);
    }

    private static void writePng(Bitmap b, File f) throws IOException {
        File tmp = new File(f.getPath() + ".tmp");
        try (OutputStream o = new FileOutputStream(tmp)) {
            if (!b.compress(Bitmap.CompressFormat.PNG, 100, o))
                throw new IOException("cannot encode " + f);
        }
        if (!tmp.renameTo(f))
            throw new IOException("cannot write " + f);
    }

    /** Deletes the title's cached art and theme (« Delete the imported copy »). The player's choice stays. */
    static void clear(Context c, String tid) {
        if (tid == null)
            return;
        String k = key(tid);
        for (String suffix : new String[] { ".png", ".hero.png", ".theme", ".last.jpg" })
            new File(artDir(c), k + suffix).delete();
    }

    /** « launcher: theme <TID>: source ..., accent #..., hues a/b/c, mode ... » (the device check asks for it). */
    String logLine() {
        return logLine(titleId, source, colors.get(GamePalette.ACCENT), accentHue, backdropHue, secondHue, mode);
    }

    static String logLine(String tid, String source, int accent, double ha, double hb, double h2, Mode mode) {
        return "launcher: theme " + tid + ": source " + source + ", accent " + GamePalette.hex(accent) + ", hues "
               + hue(ha) + "/" + hue(hb) + "/" + hue(h2) + ", mode " + mode.key;
    }
}
