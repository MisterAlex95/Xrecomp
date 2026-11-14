package fr.adserver.xrecomp;

import android.content.Context;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The picture of a GBA, PS1 or N64 game (XBA-181): its scale on the screen (RECOMP_SCALE: aspect, integer, stretch)
 * and its filter (RECOMP_FILTER: nearest, linear), read by every console runtime (RECOMP_GBA_SCALE and the other
 * per-console variables still win there); and, for a PS1 or N64 game, its internal resolution (XBA-184,
 * RECOMP_UPSCALE: 1, 2, 4 or 8 times the console's; 8 is the N64 Vulkan renderer's alone, the PS1 stops at 4). Which
 * one applies, first found wins:
 * <ol>
 *   <li>the game's own choice (its Home › Options, or the in-game menu), a file per library key: scale_&lt;KEY&gt;,
 *       filter_&lt;KEY&gt;, upscale_&lt;KEY&gt;, as {@link ConsoleControls} keeps pad_map_&lt;KEY&gt;;</li>
 *   <li>Settings › Graphics, a line in recomp.cfg;</li>
 *   <li>the runtime's default (scale: aspect; filter: the console's own, GBA nearest, PS1 and N64 linear).</li>
 * </ol>
 * {@link ShellActivity} puts the result in the environment before the runtime starts, with the settings shared by
 * every console (vertical sync, audio buffer, rumble; {@link #SHARED_ENV}), and unsets what has no value, so nothing
 * a previous game of the same process chose stays. An Xbox game gets neither (its runtime ignores them).
 */
final class ConsoleVideo {
    static final String ENV_SCALE = "RECOMP_SCALE";
    static final String ENV_FILTER = "RECOMP_FILTER";
    static final String ENV_UPSCALE = "RECOMP_UPSCALE";
    static final String SCALE_ASPECT = "aspect", SCALE_INTEGER = "integer", SCALE_STRETCH = "stretch";
    static final String FILTER_NEAREST = "nearest", FILTER_LINEAR = "linear";
    static final String UPSCALE_NATIVE = "1", UPSCALE_2X = "2", UPSCALE_4X = "4", UPSCALE_8X = "8";

    /** Settings shared with the Xbox that the console runtimes read too (recomp.cfg's line, exported as it is). */
    static final String[] SHARED_ENV = { "RECOMP_PRESENT_VSYNC", "RECOMP_AUDIO_BUF_SAMPLES", "RECOMP_RUMBLE" };

    enum Setting {
        SCALE(ENV_SCALE, "scale_", new String[] { SCALE_ASPECT, SCALE_INTEGER, SCALE_STRETCH },
              new int[] { R.string.VAL_CSCALE_ASPECT, R.string.VAL_CSCALE_INTEGER, R.string.VAL_CSCALE_STRETCH }),
        FILTER(ENV_FILTER, "filter_", new String[] { FILTER_NEAREST, FILTER_LINEAR },
               new int[] { R.string.VAL_CFILTER_NEAREST, R.string.VAL_CFILTER_LINEAR }),
        /** XBA-184: PS1 and N64 games only ({@link #applies}). */
        UPSCALE(ENV_UPSCALE, "upscale_", new String[] { UPSCALE_NATIVE, UPSCALE_2X, UPSCALE_4X, UPSCALE_8X },
                new int[] { R.string.VAL_UPSCALE_NATIVE, R.string.VAL_UPSCALE_2X, R.string.VAL_UPSCALE_4X,
                            R.string.VAL_UPSCALE_8X });

        final String env;
        final String filePrefix;
        /** The values the runtime takes, in the order the rows cycle through them. */
        final String[] values;
        final int[] labels;

        Setting(String env, String filePrefix, String[] values, int[] labels) {
            this.env = env;
            this.filePrefix = filePrefix;
            this.values = values;
            this.labels = labels;
        }

        /** The index of v in values (case and spaces ignored), or -1. */
        int indexOf(String v) {
            if (v == null)
                return -1;
            String t = v.trim().toLowerCase(Locale.ROOT);
            for (int i = 0; i < values.length; i++)
                if (values[i].equals(t))
                    return i;
            return -1;
        }
    }

    private ConsoleVideo() {}

    /** Whether a game of console con has setting s: scale and filter every console game, the internal resolution the
     *  PS1 and the N64 (the GBA draws its own 240 x 160, nothing to upscale). */
    static boolean applies(Setting s, Console con) {
        if (con == null || con == Console.XBOX)
            return false;
        return s != Setting.UPSCALE || con == Console.PS1 || con == Console.N64;
    }

    private static String file(Setting s, String key) {
        return s.filePrefix + key.toUpperCase(Locale.ROOT);
    }

    /** The game's own choice (one of s.values), or null when it follows Settings. */
    static String gameChoice(Context c, String key, Setting s) {
        if (key == null || key.isEmpty())
            return null;
        int i = s.indexOf(GameStore.readSettingFile(c, file(s, key)));
        return i < 0 ? null : s.values[i];
    }

    /** Sets the game's own choice; null (or a value the setting does not have) goes back to following Settings. */
    static boolean setGameChoice(Context c, String key, Setting s, String v) {
        if (key == null || key.isEmpty())
            return false;
        int i = s.indexOf(v);
        return GameStore.writeSettingFile(c, file(s, key), i < 0 ? "" : s.values[i]);
    }

    /** The per-game cycle: as in Settings, then each value, then as in Settings again. */
    static String next(Setting s, String cur, int dir) {
        int n = s.values.length + 1, i = s.indexOf(cur) + 1;
        int j = ((i + dir) % n + n) % n;
        return j == 0 ? null : s.values[j - 1];
    }

    /** What goes in the variable: the game's choice, else recomp.cfg's line (trimmed, as written), else null (unset). */
    static String resolve(String gameChoice, String cfg) {
        if (gameChoice != null)
            return gameChoice;
        String t = cfg == null ? null : cfg.trim();
        return t == null || t.isEmpty() ? null : t;
    }

    static List<String[]> runEnv(Console con, String gameScale, String gameFilter, RecompConfig cfg) {
        return runEnv(con, gameScale, gameFilter, null, cfg);
    }

    /**
     * The environment for a game of console con: { variable, value or null to unset }. A console game gets its scale,
     * filter and internal resolution (gameScale / gameFilter / gameUpscale: its own choices, may be null; the
     * resolution's only for a PS1 or N64 game, {@link #applies}) and the shared settings from cfg (may be null); an
     * Xbox game gets the three unset (the shared ones stay recomp.cfg's, which the runtime reads).
     */
    static List<String[]> runEnv(Console con, String gameScale, String gameFilter, String gameUpscale,
                                 RecompConfig cfg) {
        List<String[]> out = new ArrayList<>();
        boolean console = con != null && con != Console.XBOX;
        out.add(new String[] { ENV_SCALE, console ? resolve(gameScale, cfg == null ? null : cfg.get(ENV_SCALE)) : null });
        out.add(new String[] { ENV_FILTER, console ? resolve(gameFilter, cfg == null ? null : cfg.get(ENV_FILTER))
                                                   : null });
        String up = applies(Setting.UPSCALE, con) ? gameUpscale : null;
        out.add(new String[] { ENV_UPSCALE, console ? resolve(up, cfg == null ? null : cfg.get(ENV_UPSCALE)) : null });
        if (console) {
            for (String k : SHARED_ENV)
                out.add(new String[] { k, resolve(null, cfg == null ? null : cfg.get(k)) });
        }
        return out;
    }

    /** The label of the game's choice: « As in Settings » or the value's. */
    static int label(Setting s, String choice) {
        int i = s.indexOf(choice);
        return i < 0 ? R.string.MENU_VIDEO_FOLLOW : s.labels[i];
    }
}
