package fr.adserver.xrecomp;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * A game's own settings (XBA-188, docs/android/app-shell.md « Settings per game »): any option of Settings
 * ({@link RecompConfig#OPTIONS}) can follow Settings (« As in Settings ») or have its own value for one game, Xbox
 * title or GBA / PS1 / N64 game alike, with the game's controls (RECOMP_PAD_MAP for an Xbox title, the console's
 * RECOMP_&lt;C&gt;_PAD_MAP) and hotkeys (RECOMP_HOTKEYS, XBA-190), the N64 controller pak and the launcher's game theme.
 *
 * <p>One file per game, files/settings/game/&lt;KEY&gt;.cfg (KEY: the Xbox title ID or the library key, upper case), in
 * recomp.cfg's syntax: a key the file does not set follows Settings; « KEY=VALUE » is the game's value; « KEY= » (an
 * empty value) is the runtime's default for this game even when recomp.cfg sets another (the « Off » / « Auto »
 * choices that write no line in recomp.cfg). An option of several keys (frame generation) is the game's own when the
 * file sets any of them; the keys it does not set keep Settings' values.
 *
 * <p>Which value a game gets, first found wins: the game's file, recomp.cfg, the runtime's default.
 * {@link ShellActivity} puts the result in the environment before the runtime starts ({@link #runEnv}): a game's
 * value is exported (the environment wins over recomp.cfg, src/platform/recomp_cfg.c), a game's « default » unsets
 * the variable and names it in RECOMP_CFG_SKIP (recomp.cfg's line is then ignored), anything else is unset so
 * recomp.cfg decides and nothing a previous game of the same process chose stays.
 *
 * <p>The per-game files of earlier versions (language_, widescreen_, 480p_, theme_, scale_, filter_, upscale_,
 * framegen_, pad_map_, n64_pak_ &lt;KEY&gt; in files/settings) are moved into the game's file the first time it is read
 * ({@link #migrate}), their values kept.
 */
final class GameSettings {
    static final String DIR = "game";
    static final String SUFFIX = ".cfg";
    static final String HEADER = "# This game's own settings (XRecomp, Game settings): a key not here follows Settings,"
                                 + " KEY= is the runtime's default.";
    /** The launcher's game theme (GameTheme.Mode key): never exported, the launcher and the in-game menu read it. */
    static final String THEME_KEY = "XRECOMP_THEME";
    /** recomp_cfg.h RECOMP_CFG_SKIP_ENV: recomp.cfg lines the runtime ignores for this game. */
    static final String SKIP_ENV = "RECOMP_CFG_SKIP";

    private GameSettings() {}

    // ---- Files ------------------------------------------------------------------------------------------------------

    static String norm(String key) {
        return key == null ? null : key.trim().toUpperCase(Locale.ROOT);
    }

    static File dir(File filesDir) {
        return new File(new File(filesDir, "settings"), DIR);
    }

    static File file(File filesDir, String key) {
        return new File(dir(filesDir), norm(key) + SUFFIX);
    }

    /** The game's settings (migrated first); an empty one for a null key (never saved). */
    static RecompConfig load(File filesDir, String key) {
        if (key == null || key.trim().isEmpty())
            return RecompConfig.loadFile(new File(dir(filesDir), "none" + SUFFIX), HEADER);
        migrate(filesDir);
        return RecompConfig.loadFile(file(filesDir, key), HEADER);
    }

    static RecompConfig load(Context c, String key) {
        return load(c.getFilesDir(), key);
    }

    /** Writes the game's file; false on an error (logged by the caller). */
    static boolean save(RecompConfig game) {
        try {
            game.save();
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /** One key of the game's file: value (« » for the runtime's default), or null to follow Settings again. */
    static boolean put(File filesDir, String key, String name, String value) {
        if (key == null || key.trim().isEmpty())
            return false;
        RecompConfig g = load(filesDir, key);
        g.set(name, value);
        return save(g);
    }

    static boolean put(Context c, String key, String name, String value) {
        return put(c.getFilesDir(), key, name, value);
    }

    /** The game's raw value for name: null when it follows Settings, « » for the runtime's default. */
    static String raw(Context c, String key, String name) {
        if (key == null || key.trim().isEmpty())
            return null;
        return load(c, key).get(name);
    }

    /** « Reset to Settings »: the game's file goes, every value follows Settings again. */
    static boolean reset(File filesDir, String key) {
        if (key == null || key.trim().isEmpty())
            return false;
        File f = file(filesDir, key);
        return !f.exists() || f.delete();
    }

    // ---- Migration --------------------------------------------------------------------------------------------------

    /** An earlier per-game file: its prefix in files/settings and the game key(s) it becomes. */
    private static final String[] OLD_PREFIXES = { "language_", "widescreen_", "480p_", "theme_", "scale_", "filter_",
                                                   "upscale_", "framegen_", "pad_map_", "n64_pak_" };

    /**
     * Moves the per-game files of earlier versions into each game's file (a key the game's file already sets wins),
     * then deletes them. Returns how many were moved. Safe to call often: it only lists files/settings.
     */
    static int migrate(File filesDir) {
        File settings = new File(filesDir, "settings");
        String[] names = settings.list();
        if (names == null)
            return 0;
        int moved = 0;
        for (String name : names) {
            String prefix = null;
            for (String p : OLD_PREFIXES) {
                if (name.startsWith(p) && name.length() > p.length() && !name.endsWith(".tmp")) {
                    prefix = p;
                    break;
                }
            }
            if (prefix == null)
                continue;
            File old = new File(settings, name);
            if (!old.isFile())
                continue;
            String key = norm(name.substring(prefix.length()));
            String v = readLine(old);
            RecompConfig g = RecompConfig.loadFile(file(filesDir, key), HEADER);
            for (String[] kv : migrated(prefix, key, v)) {
                if (!g.has(kv[0]))
                    g.set(kv[0], kv[1]);
            }
            if (save(g) && old.delete())
                moved++;
        }
        return moved;
    }

    /** What an earlier file (prefix, its game key, its first line or null) becomes: { key, value } pairs. */
    static List<String[]> migrated(String prefix, String key, String v) {
        List<String[]> out = new ArrayList<>();
        String t = v == null ? "" : v.trim();
        switch (prefix) {
        case "language_":
            if (GameLanguage.indexOf(t) >= 0)
                out.add(new String[] { GameLanguage.ENV, GameLanguage.CODES[GameLanguage.indexOf(t)] });
            break;
        case "widescreen_":
        case "480p_": {
            String env = prefix.equals("480p_") ? RecompConfig.ENV_480P : RecompConfig.ENV_WIDESCREEN;
            if (t.equals("1"))
                out.add(new String[] { env, "1" });
            else if (t.equals("0"))
                out.add(new String[] { env, "" });
            break;
        }
        case "theme_":
            if (!t.isEmpty())
                out.add(new String[] { THEME_KEY, GameTheme.Mode.of(t).key });
            break;
        case "scale_":
        case "filter_":
        case "upscale_": {
            ConsoleVideo.Setting s = prefix.equals("scale_") ? ConsoleVideo.Setting.SCALE
                                     : prefix.equals("filter_") ? ConsoleVideo.Setting.FILTER
                                                                : ConsoleVideo.Setting.UPSCALE;
            if (s.indexOf(t) >= 0)
                out.addAll(ConsoleVideo.gameValues(s, s.values[s.indexOf(t)]));
            break;
        }
        case "framegen_":
            if (ConsoleVideo.Setting.FRAMEGEN.indexOf(t) >= 0)
                out.addAll(ConsoleVideo.gameValues(ConsoleVideo.Setting.FRAMEGEN, t));
            break;
        case "pad_map_": {
            // The file names are upper case, Console.ofKey reads the library's lower-case prefix.
            String env = ConsoleControls.env(Console.ofKey(key.toLowerCase(Locale.ROOT)));
            if (env != null && t.startsWith("map:"))
                out.add(new String[] { env, t.substring(4).trim() });
            break;
        }
        case "n64_pak_":
            if (t.equals(N64Pak.RUMBLE) || t.equals(N64Pak.CONTROLLER))
                out.add(new String[] { N64Pak.ENV, t });
            break;
        default:
            break;
        }
        return out;
    }

    private static String readLine(File f) {
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String s = r.readLine();
            return s == null ? null : s.trim();
        } catch (IOException e) {
            return null;
        }
    }

    // ---- Options --------------------------------------------------------------------------------------------------

    /**
     * Whether option o means anything to a game of console c: the console window's picture options (scale, filter)
     * to the console games, rewind and its memory to every game (XBA-274: the Xbox's state service rewinds too), the internal resolution to the PS1, the N64 and the Xbox (XBA-237), the picture shader (XBA-195) to every game
     * but its LCD ghosting to the console games (the Xbox presenter draws no LCD grid, XBA-203), the others every
     * console reads (« All consoles ») to every game, the Xbox's own to Xbox titles; the shell's self-test (a
     * developer switch) to none.
     */
    static boolean appliesTo(RecompConfig.Option o, Console c) {
        if (c == null)
            c = Console.XBOX;
        if (o.title == R.string.OPT_SELFTEST)
            return false;
        if (o.title == R.string.OPT_CSCALE || o.title == R.string.OPT_CFILTER)
            return c != Console.XBOX;   // the console window's picture (the Xbox presenter has neither)
        if (o.title == R.string.OPT_REWIND || o.title == R.string.OPT_REWIND_MB)
            return true;   // XBA-194, XBA-274: every console; an Xbox title's through its state service (XBA-249)
        if (o.title == R.string.OPT_CSHADER_GHOST)
            return c != Console.XBOX;   // XBA-203: the LCD grid's ghosting; an Xbox game draws no LCD grid
        if (isShader(o))
            return true;                // XBA-195 / XBA-203: the console window's and the Xbox presenter's
        if (o.title == R.string.OPT_UPSCALE)   // XBA-237: the Xbox's GPU renderer too (RECOMP_GPU_SCALE wins)
            return c == Console.PS1 || c == Console.N64 || c == Console.GC || c == Console.XBOX;   // XBA-274: GX
        return o.shared || c == Console.XBOX;
    }

    /** XBA-195: the picture shader's options (the console window's and, XBA-203, the Xbox presenter's). */
    static boolean isShader(RecompConfig.Option o) {
        return o.title == R.string.OPT_CSHADER || o.title == R.string.OPT_CSHADER_STRENGTH
               || o.title == R.string.OPT_CSHADER_CURVE || o.title == R.string.OPT_CSHADER_GHOST;
    }

    /** The options a game of console c can have its own value for, in Settings' order (lsfg: built with it). */
    static List<RecompConfig.Option> options(Console c, boolean lsfg) {
        List<RecompConfig.Option> out = new ArrayList<>();
        for (RecompConfig.Option o : RecompConfig.shownOptions(lsfg)) {
            if (appliesTo(o, c))
                out.add(o);
        }
        return out;
    }

    /** True when the game's file sets any of the option's keys. */
    static boolean owns(RecompConfig game, RecompConfig.Option o) {
        for (String k : o.keys) {
            if (game.has(k))
                return true;
        }
        return false;
    }

    /** The value the game gets for key: the game's (« » = none), else recomp.cfg's, else null. */
    static String effective(RecompConfig global, RecompConfig game, String key) {
        String g = game == null ? null : game.get(key);
        if (g != null)
            return g.isEmpty() ? null : g;
        String v = global == null ? null : global.get(key);
        return v == null || v.isEmpty() ? null : v;
    }

    /** The choice the game plays with (its own, else Settings'), or -1 when no choice has those values. */
    static int effectiveIndex(RecompConfig global, RecompConfig game, RecompConfig.Option o) {
        for (int i = 0; i < o.choices.length; i++) {
            boolean match = true;
            for (int k = 0; k < o.keys.length && match; k++) {
                String want = o.choices[i].values[k], have = effective(global, game, o.keys[k]);
                match = want == null ? have == null : want.equals(have);
            }
            if (match)
                return i;
        }
        return -1;
    }

    /** Settings' choice for o (recomp.cfg), or -1 for a value Settings does not offer. */
    static int globalIndex(RecompConfig global, RecompConfig.Option o) {
        return effectiveIndex(global, null, o);
    }

    /**
     * The row's position: 0 « As in Settings », 1 + the choice's index for the game's own value, -1 for an own value
     * no choice has (written by hand).
     */
    static int position(RecompConfig global, RecompConfig game, RecompConfig.Option o) {
        if (!owns(game, o))
            return 0;
        int i = effectiveIndex(global, game, o);
        return i < 0 ? -1 : i + 1;
    }

    /** Sets the row's position (into game, not saved): 0 follows Settings, 1 + i the choice i. */
    static void setPosition(RecompConfig game, RecompConfig.Option o, int pos) {
        for (int k = 0; k < o.keys.length; k++) {
            String v = pos <= 0 ? null : o.choices[pos - 1].values[k];
            game.set(o.keys[k], pos <= 0 ? null : v == null ? "" : v);
        }
    }

    /** The next position, as a row's left / right step: « As in Settings », each choice, around. */
    static int step(int pos, RecompConfig.Option o, int dir) {
        int n = o.choices.length + 1;
        return Math.floorMod(Math.max(pos, 0) + dir, n);
    }

    /** True when the game has its own value for o and it differs from Settings' (the row's marker). */
    static boolean differs(RecompConfig global, RecompConfig game, RecompConfig.Option o) {
        if (!owns(game, o))
            return false;
        for (String k : o.keys) {
            if (!java.util.Objects.equals(effective(global, game, k), effective(global, null, k)))
                return true;
        }
        return false;
    }

    /** The per-console keys the game's file may set besides the options: its controls (and the N64's pak). */
    static List<String> extraKeys(Console c) {
        // XBA-190: the hotkeys (RECOMP_HOTKEYS) for every console: an Xbox title reads its menu chord.
        if (c == null || c == Console.XBOX)
            return Arrays.asList(PadMapping.KEY, Hotkeys.KEY);
        if (c == Console.N64)
            return Arrays.asList(ConsoleControls.env(c), N64Pak.ENV, Hotkeys.KEY);
        return Arrays.asList(ConsoleControls.env(c), Hotkeys.KEY);
    }

    /** How many of these options the game gives a value other than Settings' (the rows with a « This game » badge). */
    static int ownCount(RecompConfig global, RecompConfig game, List<RecompConfig.Option> opts) {
        int n = 0;
        for (RecompConfig.Option o : opts) {
            if (differs(global, game, o))
                n++;
        }
        return n;
    }

    /** How many of its console's other keys the game sets: its controls, hotkeys, N64 pak, and its theme. */
    static int ownExtras(RecompConfig game, Console c) {
        int n = game.has(THEME_KEY) ? 1 : 0;
        for (String k : extraKeys(c)) {
            if (game.has(k))
                n++;
        }
        return n;
    }

    /** Everything that is the game's own (its Home's « Game settings » count): options and other keys. */
    static int ownTotal(RecompConfig global, RecompConfig game, Console c, boolean lsfg) {
        return ownCount(global, game, options(c, lsfg)) + ownExtras(game, c);
    }

    /** Every key a game of any console may export: the options' and the controls'. */
    static List<String> allExportedKeys() {
        List<String> out = new ArrayList<>();
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            for (String k : o.keys) {
                if (!out.contains(k))
                    out.add(k);
            }
        }
        for (Console c : new Console[] { Console.XBOX, Console.GBA, Console.PS1, Console.N64, Console.GC }) {
            for (String k : extraKeys(c)) {
                if (!out.contains(k))
                    out.add(k);
            }
        }
        return out;
    }

    /** How many of the game's values differ from Settings' (options shown for console c, its controls, its pak). */
    static List<String> overridden(RecompConfig global, RecompConfig game, Console c, boolean lsfg) {
        List<String> out = new ArrayList<>();
        for (RecompConfig.Option o : options(c, lsfg)) {
            for (String k : o.keys) {
                if (game.has(k))
                    out.add(k);
            }
        }
        for (String k : extraKeys(c)) {
            if (game.has(k))
                out.add(k);
        }
        return out;
    }

    // ---- The game's environment -------------------------------------------------------------------------------------

    /** What ShellActivity sets before the runtime starts. */
    static final class Env {
        /** { variable, value or null to unset }, every key a game may export (so a previous game's goes). */
        final List<String[]> vars = new ArrayList<>();
        /** The keys the game's own file decides (its values and its « default »s), for the log line. */
        final List<String> overridden = new ArrayList<>();

        String value(String key) {
            for (String[] kv : vars) {
                if (kv[0].equals(key))
                    return kv[1];
            }
            return null;
        }

        /** « shell: game settings KEY: K=V, K2=(default) ... » or « ... none (as in Settings) ». */
        String logLine(String key) {
            StringBuilder b = new StringBuilder("shell: game settings " + key + ": ");
            if (overridden.isEmpty())
                return b.append("none (as in Settings)").toString();
            for (int i = 0; i < overridden.size(); i++) {
                String k = overridden.get(i), v = value(k);
                b.append(i == 0 ? "" : ", ").append(k).append('=').append(v != null ? v : "(default)");
            }
            return b.toString();
        }
    }

    /**
     * The environment for a game of console con whose own settings are game (may be empty), recomp.cfg global (may be
     * null), system the Android language (RECOMP_LANGUAGE: unset everywhere, it is the Android language when the Xbox
     * has it, GameLanguage). Keys that do not apply to con are unset, whatever the game's file says.
     */
    static Env runEnv(Console con, RecompConfig global, RecompConfig game, Locale system, boolean lsfg) {
        if (con == null)
            con = Console.XBOX;
        Env env = new Env();
        List<String> own = new ArrayList<>(), skip = new ArrayList<>();
        for (RecompConfig.Option o : options(con, lsfg))
            for (String k : o.keys)
                own.add(k);
        own.addAll(extraKeys(con));
        for (String k : allExportedKeys()) {
            String v = game != null && own.contains(k) ? game.get(k) : null;
            if (k.equals(GameLanguage.ENV)) {
                String cfg = global == null ? null : global.get(k);
                String lang;
                if (v == null) {
                    lang = con == Console.XBOX ? GameLanguage.resolve(null, cfg, system) : null;
                } else {
                    // The game's language, or its « System »: the Android language (English when the Xbox has none).
                    lang = v.isEmpty() ? GameLanguage.fromLocale(system) : GameLanguage.resolve(v, null, system);
                    if (cfg != null)
                        skip.add(k);
                    env.overridden.add(k);
                }
                env.vars.add(new String[] { k, lang });
                continue;
            }
            if (v == null) {
                env.vars.add(new String[] { k, null });
                continue;
            }
            env.overridden.add(k);
            if (v.isEmpty()) {
                env.vars.add(new String[] { k, null });
                if (global != null && global.has(k))
                    skip.add(k);
            } else {
                env.vars.add(new String[] { k, v });
            }
        }
        env.vars.add(new String[] { SKIP_ENV, skip.isEmpty() ? null : String.join(",", skip) });
        return env;
    }
}
