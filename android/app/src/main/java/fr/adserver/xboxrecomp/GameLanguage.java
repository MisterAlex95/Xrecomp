package fr.adserver.xboxrecomp;

import android.content.Context;

import java.io.File;
import java.util.Locale;

/**
 * The game's language (XBA-77): the console language the runtime reports to the title (RECOMP_LANGUAGE, read by
 * src/kernel/kernel_xbox.c for XC_LANGUAGE and the XC_MAX_OS block). A title picks its text and voices among the
 * languages on its disc, so a US disc is often English whatever is chosen.
 *
 * Which one applies, first found wins:
 * <ol>
 *   <li>the title's own choice (in-game menu › Settings › Language), a small file per title ID;</li>
 *   <li>Settings › Game › Language, a RECOMP_LANGUAGE line in recomp.cfg (or one written there by hand);</li>
 *   <li>the Android system language when it is one the Xbox has;</li>
 *   <li>English (the runtime's default when nothing is set).</li>
 * </ol>
 * {@link ShellActivity} sets the environment variable for 1 and 3 before the runtime starts; the environment wins over
 * recomp.cfg (recomp_cfg.c), so 1 is only set when the title has its own choice, and 3 only when recomp.cfg has none.
 */
final class GameLanguage {
    static final String ENV = "RECOMP_LANGUAGE";

    /** The Xbox languages in their XC_LANGUAGE order (1..9), as RECOMP_LANGUAGE codes. */
    static final String[] CODES = { "en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt" };

    /** Each language in its own words: a player looking for theirs reads it whatever the app's language. */
    static final String[] NAMES = { "English", "日本語", "Deutsch", "Français", "Español", "Italiano", "한국어", "中文",
                                    "Português" };

    /** The order the screens offer them: the European languages of PAL discs first, then the Asian ones. */
    static final int[] MENU_ORDER = { 0, 3, 2, 4, 5, 8, 1, 6, 7 };

    private static final String TITLE_FILE_PREFIX = "language_";

    private GameLanguage() {}

    /** The index in {@link #CODES} of a code (« fr », « FR », « fr-CA », « pt_BR ») or of 1..9, -1 when unknown. */
    static int indexOf(String code) {
        if (code == null)
            return -1;
        String s = code.trim().toLowerCase(Locale.ROOT);
        if (s.length() == 1 && s.charAt(0) >= '1' && s.charAt(0) <= '9')
            return s.charAt(0) - '1';
        if (s.length() < 2 || (s.length() > 2 && s.charAt(2) != '-' && s.charAt(2) != '_'))
            return -1;
        String two = s.substring(0, 2);
        for (int i = 0; i < CODES.length; i++) {
            if (CODES[i].equals(two))
                return i;
        }
        return -1;
    }

    /** The code for an Android locale when the Xbox has that language, else null. */
    static String fromLocale(Locale l) {
        if (l == null)
            return null;
        int i = indexOf(l.getLanguage());
        return i < 0 ? null : CODES[i];
    }

    static String name(String code) {
        int i = indexOf(code);
        return i < 0 ? code : NAMES[i];
    }

    /**
     * What to put in RECOMP_LANGUAGE before the runtime starts, or null to leave it unset (recomp.cfg, then English,
     * decide). titleChoice: the title's own choice; cfgValue: RECOMP_LANGUAGE in recomp.cfg; system: Android's.
     */
    static String resolve(String titleChoice, String cfgValue, Locale system) {
        if (indexOf(titleChoice) >= 0)
            return CODES[indexOf(titleChoice)];
        if (cfgValue != null)
            return null;
        return fromLocale(system);
    }

    /** The title's own choice (a code), or null when it follows Settings. */
    static String titleChoice(Context c, String titleId) {
        if (titleId == null || titleId.isEmpty())
            return null;
        String v = GameStore.readSettingFile(c, TITLE_FILE_PREFIX + titleId.toUpperCase(Locale.ROOT));
        return indexOf(v) < 0 ? null : CODES[indexOf(v)];
    }

    /** Sets the title's own choice; null goes back to following Settings. */
    static boolean setTitleChoice(Context c, String titleId, String code) {
        if (titleId == null || titleId.isEmpty())
            return false;
        return GameStore.writeSettingFile(c, TITLE_FILE_PREFIX + titleId.toUpperCase(Locale.ROOT),
                                          code == null ? "" : code);
    }

    /** The chosen game's title ID (8 hex digits), or null when there is no game or its XBE cannot be read. */
    static String currentTitleId(Context c) {
        String dir = GameStore.chosenDir(c);
        if (dir == null)
            return null;
        File xbe = GameStore.findXbe(new File(dir));
        return xbe == null ? null : Pack.xbeTitleId(xbe);
    }
}
