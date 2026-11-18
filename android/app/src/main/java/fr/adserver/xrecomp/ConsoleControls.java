package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.util.Locale;

/**
 * Where a GBA, PS1 or N64 mapping ({@link ConsolePadMap}) is kept and how the runtime gets it (XBA-158 / XBA-168 /
 * XBA-174), and the console's names for its buttons.
 *
 * <ul>
 *   <li>The console's mapping, for all its games: recomp.cfg's RECOMP_GBA_PAD_MAP / RECOMP_PS1_PAD_MAP /
 *       RECOMP_N64_PAD_MAP (no line: the usual one), which the runtime reads at the game's start
 *       (src/input/console_pad.h).</li>
 *   <li>A game's own mapping, over the console's: the RECOMP_&lt;C&gt;_PAD_MAP key of its own settings
 *       (GameSettings, XBA-188), the text (empty: the console's usual mapping for this game). {@link ShellActivity} puts it in the environment
 *       under the same name before the runtime starts, where it wins over recomp.cfg's line; without one the variable
 *       is unset, so recomp.cfg decides.</li>
 * </ul>
 */
final class ConsoleControls {

    private ConsoleControls() {}

    /** The environment variable (and recomp.cfg key) of the console's mapping, or null for the Xbox. */
    static String env(Console c) {
        switch (c) {
        case GBA:
            return "RECOMP_GBA_PAD_MAP";
        case PS1:
            return "RECOMP_PS1_PAD_MAP";
        case N64:
            return "RECOMP_N64_PAD_MAP";
        case GC:
            return "RECOMP_GC_PAD_MAP";
        default:
            return null;
        }
    }

    static ConsolePadMap loadConsole(RecompConfig cfg, Console c) {
        return ConsolePadMap.parse(c, cfg == null ? null : cfg.get(env(c)));
    }

    /** Into cfg (not saved): the line, or none for the usual mapping. */
    static void storeConsole(RecompConfig cfg, ConsolePadMap m) {
        String t = m.format();
        cfg.set(env(m.console), t.isEmpty() ? null : t);
    }

    /** The game's own mapping text ("" for the usual mapping), or null when it follows the console's. */
    static String gameText(Context c, String key) {
        if (key == null || key.isEmpty())
            return null;
        String env = env(Console.ofKey(key));
        return env == null ? null : GameSettings.raw(c, key, env);   // XBA-188: the game's file
    }

    static boolean hasGameMap(Context c, String key) {
        return gameText(c, key) != null;
    }

    /** The game's own mapping, or null when it follows the console's. */
    static ConsolePadMap loadGame(Context c, String key) {
        String t = gameText(c, key);
        return t == null ? null : ConsolePadMap.parse(Console.ofKey(key), t);
    }

    /** The game gets its own mapping (m), or follows the console's again (null). */
    static boolean storeGame(Context c, String key, ConsolePadMap m) {
        if (key == null || key.isEmpty())
            return false;
        String env = env(Console.ofKey(key));
        return env != null && GameSettings.put(c, key, env, m == null ? null : m.format());
    }

    /** What the game plays with: its own mapping, else the console's (recomp.cfg in ext, may be null). */
    static ConsolePadMap effective(Context c, File ext, String key) {
        ConsolePadMap g = loadGame(c, key);
        if (g != null)
            return g;
        return loadConsole(ext == null ? null : RecompConfig.load(ext), Console.ofKey(key));
    }

    /**
     * The environment for the game about to start (ShellActivity): { variable, value } pairs for every console, the
     * launched game's own text for its console when it has one, null (unset) otherwise, so a previous game's value
     * never stays in this process.
     */
    static String[][] runEnv(Context c, String key) {
        Console[] all = { Console.GBA, Console.PS1, Console.N64, Console.GC };   // XBA-274: the GameCube's too
        String[][] out = new String[all.length][];
        Console mine = key == null ? Console.XBOX : Console.ofKey(key);
        for (int i = 0; i < all.length; i++)
            out[i] = new String[] { env(all[i]), all[i] == mine ? gameText(c, key) : null };
        return out;
    }

    // ---- Names --------------------------------------------------------------------------------------------------

    /** The console's name for its target t (console_pad.h's order). */
    static int targetLabel(Console c, int t) {
        switch (c) {
        case GBA: {
            int[] ids = { R.string.CC_A, R.string.CC_B, R.string.CC_L, R.string.CC_R, R.string.CC_START,
                          R.string.CC_SELECT, R.string.XB_DPAD_UP, R.string.XB_DPAD_DOWN, R.string.XB_DPAD_LEFT,
                          R.string.XB_DPAD_RIGHT };
            return ids[t];
        }
        case PS1: {
            int[] ids = { R.string.CC_CROSS, R.string.CC_CIRCLE, R.string.CC_SQUARE, R.string.CC_TRIANGLE,
                          R.string.CC_L1, R.string.CC_R1, R.string.CC_L2, R.string.CC_R2, R.string.CC_L3, R.string.CC_R3,
                          R.string.CC_START, R.string.CC_SELECT, R.string.XB_DPAD_UP, R.string.XB_DPAD_DOWN,
                          R.string.XB_DPAD_LEFT, R.string.XB_DPAD_RIGHT };
            return ids[t];
        }
        case GC: {   // XBA-274: GC_T_*'s order (ConsolePadMap.targets)
            int[] ids = { R.string.CC_A, R.string.CC_B, R.string.CC_X, R.string.CC_Y, R.string.CC_Z, R.string.CC_L,
                          R.string.CC_R, R.string.CC_START, R.string.XB_DPAD_UP, R.string.XB_DPAD_DOWN,
                          R.string.XB_DPAD_LEFT, R.string.XB_DPAD_RIGHT };
            return ids[t];
        }
        default: {
            int[] ids = { R.string.CC_A, R.string.CC_B, R.string.CC_Z, R.string.CC_L, R.string.CC_R, R.string.CC_START,
                          R.string.XB_DPAD_UP, R.string.XB_DPAD_DOWN, R.string.XB_DPAD_LEFT, R.string.XB_DPAD_RIGHT,
                          R.string.CC_C_UP, R.string.CC_C_DOWN, R.string.CC_C_LEFT, R.string.CC_C_RIGHT };
            return ids[t];
        }
        }
    }

    /** The screen's groups: a label and the targets under it. */
    static int[] groupLabels(Console c) {
        if (c == Console.N64)
            return new int[] { R.string.CTL_GROUP_BUTTONS, R.string.CTL_GROUP_DPAD, R.string.CC_GROUP_C };
        return new int[] { R.string.CTL_GROUP_BUTTONS, R.string.CTL_GROUP_DPAD };
    }

    static int[][] groups(Console c) {
        switch (c) {
        case GBA:
            return new int[][] { { 0, 1, 2, 3, 4, 5 }, { 6, 7, 8, 9 } };
        case PS1:
            return new int[][] { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 12, 13, 14, 15 } };
        case GC:
            return new int[][] { { 0, 1, 2, 3, 4, 5, 6, 7 }, { 8, 9, 10, 11 } };
        default:
            return new int[][] { { 0, 1, 2, 3, 4, 5 }, { 6, 7, 8, 9 }, { 10, 11, 12, 13 } };
        }
    }

    /** The console's name for its stick i. */
    static int stickLabel(Console c, int i) {
        if (c == Console.N64)
            return R.string.CC_STICK;
        if (c == Console.GC)
            return i == 0 ? R.string.CC_MAIN_STICK : R.string.CC_C_STICK;
        return i == 0 ? R.string.XB_LSTICK : R.string.XB_RSTICK;
    }

    /** « A + Y », « Not used »: the player's controls a target takes. */
    static String sourcesLabel(Context c, ConsolePadMap m, int t) {
        PadMapping.Source[] s = m.sourcesOf(t);
        if (s.length == 0)
            return c.getString(R.string.SRC_NONE);
        StringBuilder b = new StringBuilder();
        for (PadMapping.Source x : s)
            b.append(b.length() == 0 ? "" : " + ").append(c.getString(ControlsLabels.source(x)));
        return b.toString();
    }

    static String consoleName(Context c, Console k) {
        switch (k) {
        case XBOX:
            return "Xbox";   // XBA-188: an Xbox title's Controls scope (« All Xbox games »)
        case GBA:
            return c.getString(R.string.CC_CONSOLE_GBA);
        case PS1:
            return c.getString(R.string.CC_CONSOLE_PS1);
        case GC:
            return c.getString(R.string.CC_CONSOLE_GC);
        default:
            return c.getString(R.string.CC_CONSOLE_N64);
        }
    }

    static String rangeLabel(Context c, int v) {
        return v < 0 ? c.getString(R.string.CC_RUNTIME_DEFAULT) : String.valueOf(v);
    }

    static String deadzoneLabel(Context c, int v) {
        return v < 0 ? c.getString(R.string.CC_RUNTIME_DEFAULT) : c.getString(R.string.MENU_PERCENT, v);
    }
}
