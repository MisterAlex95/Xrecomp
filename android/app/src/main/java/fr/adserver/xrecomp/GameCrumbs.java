package fr.adserver.xrecomp;

import android.content.ComponentCallbacks2;
import android.content.Context;
import android.view.KeyEvent;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * The game process's breadcrumbs (XBA-340, {@link Breadcrumbs}): one typed helper per event, so a call site never
 * hands over a string of its own. Every value is a number, on / off, or one of a fixed list written here (a value
 * outside it becomes « other »); never a name of anything (game, save, cheat, mod, disc, driver, file).
 * {@link Breadcrumbs#line} drops a whole event for one bad pair, so the helpers only build pairs that pass.
 */
final class GameCrumbs {
    private GameCrumbs() {}

    /** The values a setting's crumb may carry: anything else is « other ». */
    private static final Set<String> VALUES = new HashSet<>(Arrays.asList(
            "default", "auto", "on", "off", "0", "1", "2", "3", "4", "8", "30", "60", "cpu", "gl", "aspect", "integer",
            "stretch", "nearest", "linear"));

    static String value(String v) {
        if (v == null)
            return "default";
        v = v.trim().toLowerCase(Locale.ROOT);
        return VALUES.contains(v) ? v : "other";
    }

    static String onOff(boolean on) {
        return on ? "on" : "off";
    }

    /** A slot: « auto » for the automatic one, else its number. */
    static String slot(int slot) {
        return slot == GameMenu.AUTO_SLOT ? "auto" : String.valueOf(Math.max(0, Math.min(slot, 99)));
    }

    /** A native result code: « ok », else the number (« -2 »). */
    static String code(int code) {
        return code == GameMenu.STATE_OK ? "ok" : String.valueOf(Math.max(-99, Math.min(code, 99)));
    }

    // ---- The run -----------------------------------------------------------------------------------------------

    /** RECOMP_GPU's spelling: the CPU or the GL renderer, else auto. */
    static String renderer(String gpu) {
        if (gpu == null || gpu.trim().isEmpty())
            return "auto";
        switch (gpu.trim()) {
        case "0": return "cpu";
        case "1": return "gl";
        default: return "other";
        }
    }

    /** game_start's pairs: the console, the renderer, the internal resolution, the controllers. */
    static String[] startData(Console con, String gpu, String upscale, int pads) {
        return new String[] { "console", con == null ? "unknown" : con.id, "renderer", renderer(gpu),
                              "upscale", value(upscale), "pads", String.valueOf(Math.max(0, Math.min(pads, 9))) };
    }

    /** A new run: the ring starts again, then the mods it plays with (a count), if any. */
    static void start(Context c, Console con, GameSettings.Env env, int pads, int mods) {
        String up = env == null ? null : env.value(ConsoleVideo.ENV_UPSCALE);
        if (up == null && env != null)
            up = env.value("RECOMP_GPU_SCALE");
        Breadcrumbs.startRun(c, startData(con, env == null ? null : env.value("RECOMP_GPU"), up, pads));
        if (mods > 0)
            Breadcrumbs.add(c, "mod", "state", "on", "count", String.valueOf(Math.min(mods, 99)));
    }

    /** How many mods are on for the title: the entries of <mods>/<console>/<title id>/enabled.json; 0 when none. */
    static int enabledMods(File modsRoot, Console con, String key) {
        if (modsRoot == null || con == null || key == null || key.isEmpty())
            return 0;
        String bare = key.startsWith(con.id + "-") ? key.substring(con.id.length() + 1) : key;
        for (String k : new String[] { key, bare }) {
            File f = new File(new File(new File(modsRoot, con.id), k), "enabled.json");
            if (!f.isFile())
                continue;
            try {
                String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
                Object j = new org.json.JSONTokener(t).nextValue();
                if (j instanceof org.json.JSONArray)
                    return ((org.json.JSONArray)j).length();
                if (j instanceof org.json.JSONObject) {
                    org.json.JSONArray a = ((org.json.JSONObject)j).optJSONArray("enabled");
                    return a == null ? 0 : a.length();
                }
            } catch (java.io.IOException | org.json.JSONException e) {
                return 0;
            }
        }
        return 0;
    }

    /** How the run ended: quit, title_exit, restart, image_launch, no_data, bad_pack. */
    static void end(Context c, String how) {
        Breadcrumbs.add(c, "game_end", "how", how);
    }

    // ---- What the player does ------------------------------------------------------------------------------------

    /** A save state saved or loaded: the slot, the result, and how (menu, hotkey, resume, quit…). */
    static void state(Context c, boolean save, int slot, int code, String via) {
        Breadcrumbs.add(c, save ? "savestate_save" : "savestate_load", "slot", slot(slot), "result", code(code),
                        "via", via);
    }

    static void fastForward(Context c, String state, String via) {
        Breadcrumbs.add(c, "fast_forward", "state", state, "via", via);
    }

    /** A cheat turned on or off: never which one. */
    static void cheat(Context c, boolean on) {
        Breadcrumbs.add(c, "cheat", "state", onOff(on));
    }

    static void disc(Context c, int number) {
        Breadcrumbs.add(c, "disc_change", "disc", String.valueOf(Math.max(0, Math.min(number, 99))));
    }

    /** The in-game menu: the reason it opened becomes one of a few words. */
    static void menu(Context c, boolean open, String why) {
        if (open)
            Breadcrumbs.add(c, "menu_open", "via", via(why));
        else
            Breadcrumbs.add(c, "menu_close");
    }

    /** GameMenu.open's reasons. */
    static String via(String why) {
        if (why == null)
            return "other";
        switch (why) {
        case "Back": return "back";
        case "Guide": return "guide";
        case "Select+Start": return "combo";
        case "swipe": return "swipe";
        case "panel": return "panel";
        default: return "other";
        }
    }

    /** The game left the screen (another app, the launcher, the screen off) or came back. */
    static void pause(Context c, boolean paused) {
        Breadcrumbs.add(c, paused ? "pause" : "resume");
    }

    // ---- Settings changed in the menu: the last value per setting, written when the menu closes ----------------

    /** The settings changed while the menu was open (one crumb each, its last value, not one per step). */
    static final class Pending {
        private final Map<String, String[]> changes = new LinkedHashMap<>();

        /** RecompConfig keys: what the crumb is called. */
        static String eventOf(String envKey) {
            if (envKey == null)
                return "setting";
            switch (envKey) {
            case "RECOMP_GPU": return "renderer";
            case "RECOMP_GPU_SCALE":
            case ConsoleVideo.ENV_UPSCALE: return "resolution";
            case "RECOMP_GPU_FPS_CAP": return "speed";
            default: return "setting";
            }
        }

        /** The setting's short name: its key without RECOMP_, lower case (« gpu_scale »), or « other ». */
        static String nameOf(String envKey) {
            if (envKey == null)
                return "other";
            String n = envKey.startsWith("RECOMP_") ? envKey.substring(7) : envKey;
            n = n.toLowerCase(Locale.ROOT);
            return n.matches("[a-z0-9_]{1,24}") ? n : "other";
        }

        /** A RecompConfig option (a game setting) moved to value (null: « as in Settings »). */
        void option(String envKey, String value) {
            put(eventOf(envKey), nameOf(envKey), value(value));
        }

        /** A setting known by its name here (scale, filter, widescreen, language, vk_driver…), with a value. */
        void named(String event, String name, String value) {
            put(event, name, value(value));
        }

        private void put(String event, String name, String value) {
            changes.remove(event + "/" + name);   // the last change goes last
            changes.put(event + "/" + name, new String[] { event, "name", name, "value", value });
        }

        boolean isEmpty() {
            return changes.isEmpty();
        }

        /** What flush would write: { event, k, v, k, v } each. */
        List<String[]> list() {
            return new ArrayList<>(changes.values());
        }

        void flush(Context c) {
            for (String[] e : changes.values())
                Breadcrumbs.add(c, e[0], Arrays.copyOfRange(e, 1, e.length));
            changes.clear();
        }
    }

    // ---- Controllers -------------------------------------------------------------------------------------------

    /** The controller count: a crumb when it goes up or down (one pad is often several input devices). */
    static final class Pads {
        private int count;

        Pads(int count) {
            this.count = count;
        }

        /** The new count: the event and its pairs, or null when it did not change. */
        String[] changed(int now) {
            if (now == count)
                return null;
            String ev = now > count ? "pad_connect" : "pad_disconnect";
            count = now;
            return new String[] { ev, "pads", String.valueOf(Math.max(0, Math.min(now, 9))) };
        }

        void changed(Context c, int now) {
            String[] e = changed(now);
            if (e != null)
                Breadcrumbs.add(c, e[0], Arrays.copyOfRange(e, 1, e.length));
        }
    }

    // ---- Low memory --------------------------------------------------------------------------------------------

    private static int lastTrim = -1;

    /** onTrimMemory's level worth a crumb: not UI_HIDDEN (every trip to the background), not a repeat. */
    static boolean trimWorthIt(int level, int last) {
        return level != ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN && level != last && level > 0;
    }

    static void trim(Context c, int level) {
        synchronized (GameCrumbs.class) {
            if (!trimWorthIt(level, lastTrim))
                return;
            lastTrim = level;
        }
        Breadcrumbs.add(c, "low_memory", "level", String.valueOf(Math.min(level, 999)));
    }

    // ---- Hotkeys the runtime acts on alone (rewind, fast-forward): the chord seen pressed --------------------------

    /**
     * Watches the pad for the rewind and fast-forward chords ({@link Hotkeys}): the runtime acts on them without the
     * Java side knowing, so the crumb says the chord was pressed (not that the game rewound). Keys, and the triggers
     * as axes; a stick-direction chord is not watched.
     */
    static final class Chords {
        static final Hotkeys.Action[] WATCHED = { Hotkeys.Action.REWIND, Hotkeys.Action.FF_TOGGLE,
                                                  Hotkeys.Action.FF_HOLD };
        private final int[][] chords = new int[WATCHED.length][];
        private final boolean[] on = new boolean[WATCHED.length];
        private final Set<Integer> down = new HashSet<>();

        /** The chords to watch; an action that does not apply (rewind off, no fast-forward) is left out. */
        Chords(Hotkeys h, Console con, boolean rewind) {
            for (int i = 0; i < WATCHED.length; i++) {
                Hotkeys.Action a = WATCHED[i];
                if (h == null || !a.appliesTo(con) || (a == Hotkeys.Action.REWIND && !rewind))
                    continue;
                Hotkeys.Chord ch = h.get(a);
                if (ch == null || ch.isNone())
                    continue;
                int b = Hotkeys.keyOf(ch.button), m = ch.mod == null ? -1 : Hotkeys.keyOf(ch.mod);
                if (b < 0 || (ch.mod != null && m < 0))
                    continue;
                chords[i] = m < 0 ? new int[] { b } : new int[] { m, b };
            }
        }

        /** A key went down or up: the action whose chord it completed, or null. */
        Hotkeys.Action key(int keyCode, boolean isDown) {
            if (isDown)
                down.add(keyCode);
            else
                down.remove(keyCode);
            return check();
        }

        /** The triggers as axes (0..1): L2 / R2 down past half way. */
        Hotkeys.Action triggers(float l, float r) {
            boolean l2 = l > 0.5f, r2 = r > 0.5f;
            if (l2 == down.contains(KeyEvent.KEYCODE_BUTTON_L2) && r2 == down.contains(KeyEvent.KEYCODE_BUTTON_R2))
                return null;
            if (l2) down.add(KeyEvent.KEYCODE_BUTTON_L2); else down.remove(KeyEvent.KEYCODE_BUTTON_L2);
            if (r2) down.add(KeyEvent.KEYCODE_BUTTON_R2); else down.remove(KeyEvent.KEYCODE_BUTTON_R2);
            return check();
        }

        void reset() {
            down.clear();
            Arrays.fill(on, false);
        }

        private Hotkeys.Action check() {
            Hotkeys.Action fired = null;
            for (int i = 0; i < WATCHED.length; i++) {
                if (chords[i] == null)
                    continue;
                boolean all = true;
                for (int k : chords[i])
                    all &= down.contains(k);
                if (all && !on[i] && fired == null)
                    fired = WATCHED[i];
                on[i] = all;
            }
            return fired;
        }

        /** The crumb for a chord pressed. */
        static void crumb(Context c, Hotkeys.Action a) {
            if (a == Hotkeys.Action.REWIND)
                Breadcrumbs.add(c, "rewind", "via", "hotkey");
            else if (a == Hotkeys.Action.FF_TOGGLE)
                fastForward(c, "toggle", "hotkey");
            else if (a == Hotkeys.Action.FF_HOLD)
                fastForward(c, "hold", "hotkey");
        }
    }
}
