package fr.adserver.xboxrecomp;

import android.content.Context;

import java.util.Locale;

/**
 * The on-screen pad's settings (XBA-42), Controls › Touch controls: when it shows (Auto: no controller connected;
 * Always; Off), its opacity and its size. One line in files/settings/touch_controls ("AUTO 50 100"), a small file
 * like the bottom screen's (GameStore): the launcher writes it, the game's process reads it at each onStart.
 */
final class TouchSettings {
    enum Mode { AUTO, ON, OFF }

    static final String FILE = "touch_controls";
    static final int[] OPACITIES = { 30, 50, 70, 100 };
    static final int[] SIZES = { 80, 100, 125 };

    Mode mode = Mode.AUTO;
    int opacity = 50;   // percent
    int size = 100;     // percent

    static TouchSettings parse(String line) {
        TouchSettings s = new TouchSettings();
        if (line == null)
            return s;
        String[] f = line.trim().split("\\s+");
        try {
            if (f.length > 0)
                s.mode = Mode.valueOf(f[0].toUpperCase(Locale.ROOT));
            if (f.length > 1)
                s.opacity = nearest(OPACITIES, Integer.parseInt(f[1]));
            if (f.length > 2)
                s.size = nearest(SIZES, Integer.parseInt(f[2]));
        } catch (IllegalArgumentException e) {
            // keep what was read so far; the rest stays at its default
        }
        return s;
    }

    String format() {
        return mode.name() + " " + opacity + " " + size;
    }

    static TouchSettings load(Context c) {
        return parse(GameStore.readSettingFile(c, FILE));
    }

    boolean save(Context c) {
        return GameStore.writeSettingFile(c, FILE, format());
    }

    boolean isDefault() {
        return mode == Mode.AUTO && opacity == 50 && size == 100;
    }

    private static int nearest(int[] choices, int v) {
        int best = choices[0];
        for (int c : choices)
            if (Math.abs(c - v) < Math.abs(best - v))
                best = c;
        return best;
    }

    static int step(int[] choices, int v, int d) {
        int at = 0;
        for (int i = 0; i < choices.length; i++)
            if (choices[i] == v)
                at = i;
        return choices[Math.floorMod(at + d, choices.length)];
    }

    /**
     * Whether the pad shows: never while the companion panel is on the second screen (the AYN Thor has its own pad
     * and its panel's buttons), never Off, always On; Auto: while no controller is connected and none was used since
     * the last time there was none.
     */
    static boolean visible(Mode mode, boolean controllerConnected, boolean controllerUsed, boolean panelShowing) {
        if (panelShowing || mode == Mode.OFF)
            return false;
        if (mode == Mode.ON)
            return true;
        return !controllerConnected && !controllerUsed;
    }
}
