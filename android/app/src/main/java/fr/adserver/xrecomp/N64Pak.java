package fr.adserver.xrecomp;

import android.content.Context;

/**
 * The accessory in an N64 game's controller (XBA-174): a controller pak (the default: its saves, pak1.mpk) or a rumble
 * pak (the pad vibrates when the game says so). One choice per game, from the N64 Home's second button, kept in the
 * game's own settings ({@link GameSettings}, RECOMP_N64_PAK). The runtime reads RECOMP_N64_PAK
 * (consoles/n64/runtime/n64_runtime.c): {@link ShellActivity} sets it to « rumble » or « controller » for a game with a
 * choice and unsets it otherwise, so a RECOMP_N64_PAK line of recomp.cfg, then the controller pak, decide.
 */
final class N64Pak {
    static final String ENV = "RECOMP_N64_PAK";
    static final String CONTROLLER = "controller";
    static final String RUMBLE = "rumble";

    private N64Pak() {}

    /** The game's own choice, CONTROLLER or RUMBLE, or null when it has none. */
    static String choice(Context c, String key) {
        if (key == null || key.isEmpty())
            return null;
        String v = GameSettings.raw(c, key, ENV);   // XBA-188: the game's file
        return RUMBLE.equals(v) ? RUMBLE : CONTROLLER.equals(v) ? CONTROLLER : null;
    }

    /** Sets the game's choice (CONTROLLER, RUMBLE, or null for none). */
    static boolean setChoice(Context c, String key, String v) {
        if (key == null || key.isEmpty())
            return false;
        return GameSettings.put(c, key, ENV, RUMBLE.equals(v) || CONTROLLER.equals(v) ? v : null);
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
