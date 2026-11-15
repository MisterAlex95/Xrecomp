package fr.adserver.xrecomp;

import android.content.Context;

import java.util.Locale;

/**
 * The console's widescreen and 480p settings per title (XBA-134), as {@link GameLanguage} does for the language: the
 * runtime reports them to the title from the EEPROM and the AV capabilities (RECOMP_WIDESCREEN, RECOMP_480P, read by
 * src/kernel/kernel_xbox.c, XBA-121). A title that draws a broken 16:9 or 480p picture can be set back to 4:3 or 480i
 * while the others keep Settings' choice, and the other way round.
 *
 * Which one applies, first found wins:
 * <ol>
 *   <li>the title's own choice (in-game menu › Settings › Widescreen / 480p: On or Off, or its Game settings), kept
 *       in the game's own settings ({@link GameSettings}, XBA-188);</li>
 *   <li>Settings › Graphics, a line in recomp.cfg (or one written there by hand);</li>
 *   <li>off (the runtime's default).</li>
 * </ol>
 * {@link ShellActivity} sets the variable to « 1 » or « 0 » for 1 before the runtime starts (the environment wins
 * over recomp.cfg, recomp_cfg.c) and unsets it otherwise, so recomp.cfg decides and nothing a previous game of the
 * same process chose is left behind.
 */
final class GameVideo {
    enum Setting {
        WIDESCREEN(RecompConfig.ENV_WIDESCREEN, "widescreen_"),
        P480(RecompConfig.ENV_480P, "480p_");

        final String env;
        final String filePrefix;

        Setting(String env, String filePrefix) {
            this.env = env;
            this.filePrefix = filePrefix;
        }
    }

    private GameVideo() {}

    /** The title's own choice: TRUE on, FALSE off, null when it follows Settings. */
    static Boolean titleChoice(Context c, String titleId, Setting s) {
        if (titleId == null || titleId.isEmpty())
            return null;
        String v = GameSettings.raw(c, titleId, s.env);   // XBA-188: the game's file, « » = off
        return v == null ? null : "1".equals(v) ? Boolean.TRUE : Boolean.FALSE;
    }

    /** Sets the title's own choice; null goes back to following Settings. */
    static boolean setTitleChoice(Context c, String titleId, Setting s, Boolean on) {
        if (titleId == null || titleId.isEmpty())
            return false;
        return GameSettings.put(c, titleId, s.env, on == null ? null : on ? "1" : "");
    }

    /** What to put in the variable before the runtime starts, or null to leave it unset (recomp.cfg decides). */
    static String resolve(Boolean titleChoice) {
        return titleChoice == null ? null : titleChoice ? "1" : "0";
    }

    /** The menu's cycle: as in Settings, On, Off, as in Settings. */
    static Boolean next(Boolean cur, int dir) {
        Boolean[] order = { null, Boolean.TRUE, Boolean.FALSE };
        int i = cur == null ? 0 : cur ? 1 : 2;
        return order[((i + dir) % 3 + 3) % 3];
    }
}
