package fr.adserver.xrecomp;

import android.content.Context;

import java.util.Locale;

/**
 * The accessory in an N64 game's controller (XBA-174): a controller pak (the default: its saves, pak1.mpk) or a rumble
 * pak (the pad vibrates when the game says so). One choice per game, from the N64 Home's second button, a file per
 * library key as {@link GameVideo} keeps its per-title choices. The runtime reads RECOMP_N64_PAK
 * (consoles/n64/runtime/n64_runtime.c): {@link ShellActivity} sets it to « rumble » or « controller » for a game with a
 * choice and unsets it otherwise, so a RECOMP_N64_PAK line of recomp.cfg, then the controller pak, decide.
 */
final class N64Pak {
    static final String ENV = "RECOMP_N64_PAK";
    static final String CONTROLLER = "controller";
    static final String RUMBLE = "rumble";
    private static final String FILE_PREFIX = "n64_pak_";

    private N64Pak() {}

    /** The game's own choice, CONTROLLER or RUMBLE, or null when it has none. */
    static String choice(Context c, String key) {
        if (key == null || key.isEmpty())
            return null;
        String v = GameStore.readSettingFile(c, FILE_PREFIX + key.toUpperCase(Locale.ROOT));
        return RUMBLE.equals(v) ? RUMBLE : CONTROLLER.equals(v) ? CONTROLLER : null;
    }

    /** Sets the game's choice (CONTROLLER, RUMBLE, or null for none). */
    static boolean setChoice(Context c, String key, String v) {
        if (key == null || key.isEmpty())
            return false;
        return GameStore.writeSettingFile(c, FILE_PREFIX + key.toUpperCase(Locale.ROOT),
                                          RUMBLE.equals(v) || CONTROLLER.equals(v) ? v : "");
    }

    /** Whether the game plays with the rumble pak: its choice, else recomp.cfg's line (cfg, may be null). */
    static boolean rumble(String choice, String cfg) {
        if (choice != null)
            return RUMBLE.equals(choice);
        return cfg != null && RUMBLE.equalsIgnoreCase(cfg.trim());
    }

    /** The Home's button: the other accessory from the one in use. */
    static String toggled(boolean rumble) {
        return rumble ? CONTROLLER : RUMBLE;
    }
}
