package fr.adserver.xrecomp;

import android.content.Context;
import android.view.KeyEvent;

import java.io.File;
import java.util.ArrayList;
import java.util.EnumMap;
import java.util.List;
import java.util.Locale;
import java.util.Objects;

/**
 * The player's hotkeys (XBA-190, docs/android/app-shell.md « Hotkeys »): which pad chord does fast-forward, quick save /
 * load, the quick slot and the in-game menu. The text is src/platform/recomp_hotkeys.h's RECOMP_HOTKEYS (« action:mod+
 * button », only what differs from the defaults), kept in recomp.cfg for every game and in a game's own settings
 * (GameSettings, XBA-188) when its Controls are « This game only »; the GBA / PS1 / N64 runtimes read it at the start,
 * the in-game menu ({@link MenuTriggers.Combo}) reads the menu chord, the only one an Xbox title has (decision
 * 2026-11-14: no save states there).
 *
 * <p>Entries this class does not show (the runtime's save1..4 / load1..4) are kept as they are.
 */
final class Hotkeys {
    static final String KEY = "RECOMP_HOTKEYS";

    enum Action {
        FF_HOLD("ff_hold"), FF_TOGGLE("ff_toggle"), QSAVE("qsave"), QLOAD("qload"), SLOT_NEXT("slot_next"),
        SLOT_PREV("slot_prev"), MENU("menu");

        final String key;

        Action(String key) {
            this.key = key;
        }

        static Action of(String key) {
            for (Action a : values()) {
                if (a.key.equalsIgnoreCase(key))
                    return a;
            }
            return null;
        }

        /** Whether it means anything to a game of console c: the menu everywhere, the rest with save states. */
        boolean appliesTo(Console c) {
            return this == MENU || (c != null && c.hasSaveStates());
        }
    }

    /** A modifier (null: the button alone) and a button (null: no chord). */
    static final class Chord {
        static final Chord NONE = new Chord(null, null);

        final PadMapping.Source mod, button;

        Chord(PadMapping.Source mod, PadMapping.Source button) {
            this.button = button == PadMapping.Source.NONE ? null : button;
            this.mod = this.button == null || mod == PadMapping.Source.NONE || mod == this.button ? null : mod;
        }

        boolean isNone() {
            return button == null;
        }

        /** The sources it holds (empty for none). */
        List<PadMapping.Source> sources() {
            List<PadMapping.Source> out = new ArrayList<>();
            if (mod != null)
                out.add(mod);
            if (button != null)
                out.add(button);
            return out;
        }

        /** The same buttons, in either order (what the runtime treats as one chord). */
        boolean sameButtons(Chord o) {
            if (o == null || isNone() || o.isNone())
                return false;
            return (Objects.equals(mod, o.mod) && button == o.button)
                   || (mod != null && mod == o.button && button == o.mod);
        }

        /** « back+right_shoulder », « start », « none ». */
        String text() {
            if (button == null)
                return "none";
            return mod == null ? button.key : mod.key + "+" + button.key;
        }

        static Chord parse(String t) {
            if (t == null)
                return null;
            t = t.trim();
            if (t.isEmpty() || t.equalsIgnoreCase("none"))
                return NONE;
            String[] p = t.split("\\+", -1);
            if (p.length > 2)
                return null;
            PadMapping.Source a = source(p[0]), b = p.length == 2 ? source(p[1]) : null;
            if (a == null || (p.length == 2 && (b == null || a == b)))
                return null;
            return p.length == 2 ? new Chord(a, b) : new Chord(null, a);
        }

        @Override
        public boolean equals(Object o) {
            return o instanceof Chord && ((Chord) o).mod == mod && ((Chord) o).button == button;
        }

        @Override
        public int hashCode() {
            return Objects.hash(mod, button);
        }

        @Override
        public String toString() {
            return text();
        }
    }

    /** A source by its name or recomp_hotkeys.c's short name (rt, lb, select, ...); null when unknown or none. */
    static PadMapping.Source source(String name) {
        if (name == null)
            return null;
        String n = name.trim().toLowerCase(Locale.ROOT);
        switch (n) {
        case "a": return PadMapping.Source.SOUTH;
        case "b": return PadMapping.Source.EAST;
        case "x": return PadMapping.Source.WEST;
        case "y": return PadMapping.Source.NORTH;
        case "lb": return PadMapping.Source.LEFT_SHOULDER;
        case "rb": return PadMapping.Source.RIGHT_SHOULDER;
        case "lt": return PadMapping.Source.LEFT_TRIGGER;
        case "rt": return PadMapping.Source.RIGHT_TRIGGER;
        case "l3": return PadMapping.Source.LEFT_STICK_CLICK;
        case "r3": return PadMapping.Source.RIGHT_STICK_CLICK;
        case "select": return PadMapping.Source.BACK;
        case "up": return PadMapping.Source.DPAD_UP;
        case "down": return PadMapping.Source.DPAD_DOWN;
        case "left": return PadMapping.Source.DPAD_LEFT;
        case "right": return PadMapping.Source.DPAD_RIGHT;
        default:
            PadMapping.Source s = PadMapping.Source.of(n);
            return s == PadMapping.Source.NONE ? null : s;
        }
    }

    /** recomp_hotkeys.c's defaults: XBA-182's chords, Select + Start for the menu. */
    static Chord def(Action a) {
        switch (a) {
        case FF_HOLD:
            return new Chord(PadMapping.Source.BACK, PadMapping.Source.RIGHT_TRIGGER);
        case QSAVE:
            return new Chord(PadMapping.Source.BACK, PadMapping.Source.RIGHT_SHOULDER);
        case QLOAD:
            return new Chord(PadMapping.Source.BACK, PadMapping.Source.LEFT_SHOULDER);
        case MENU:
            return new Chord(PadMapping.Source.BACK, PadMapping.Source.START);
        default:
            return Chord.NONE;
        }
    }

    private final EnumMap<Action, Chord> chords = new EnumMap<>(Action.class);
    /** Entries kept as they are: the runtime's own (save1..4, load1..4) and any this version does not know. */
    private final List<String> others = new ArrayList<>();

    Hotkeys() {
        reset();
    }

    void reset() {
        for (Action a : Action.values())
            chords.put(a, def(a));
        others.clear();
    }

    Chord get(Action a) {
        return chords.get(a);
    }

    void set(Action a, Chord c) {
        chords.put(a, c == null ? Chord.NONE : c);
    }

    /** RECOMP_HOTKEYS text over the defaults (null or « »: the defaults); bad entries are dropped. */
    static Hotkeys parse(String text) {
        Hotkeys h = new Hotkeys();
        if (text == null)
            return h;
        for (String e : text.trim().split("[,;\\s]+")) {
            if (e.isEmpty())
                continue;
            int sep = indexOfSep(e);
            if (sep <= 0)
                continue;
            Action a = Action.of(e.substring(0, sep));
            if (a == null) {
                h.others.add(e);
                continue;
            }
            Chord c = Chord.parse(e.substring(sep + 1));
            if (c != null)
                h.chords.put(a, c);
        }
        return h;
    }

    private static int indexOfSep(String e) {
        int a = e.indexOf(':'), b = e.indexOf('=');
        return a < 0 ? b : b < 0 ? a : Math.min(a, b);
    }

    /** What differs from the defaults, recomp_hotkeys.h's order; « » for the defaults. */
    String format() {
        StringBuilder b = new StringBuilder();
        for (Action a : Action.values()) {
            if (!chords.get(a).equals(def(a)))
                b.append(b.length() == 0 ? "" : ",").append(a.key).append(':').append(chords.get(a).text());
        }
        for (String o : others)
            b.append(b.length() == 0 ? "" : ",").append(o);
        return b.toString();
    }

    boolean isDefault() {
        return format().isEmpty();
    }

    int changedCount() {
        int n = 0;
        for (Action a : Action.values())
            n += chords.get(a).equals(def(a)) ? 0 : 1;
        return n;
    }

    // ---- Conflicts --------------------------------------------------------------------------------------------------

    /** The other actions of console c's list with the same chord as a (the runtime would do both). */
    List<Action> sharing(Action a, Console c) {
        List<Action> out = new ArrayList<>();
        Chord mine = chords.get(a);
        for (Action o : actions(c)) {
            if (o != a && mine.sameButtons(chords.get(o)))
                out.add(o);
        }
        return out;
    }

    /**
     * The button of a's chord when the game needs it alone: a chord without a modifier on a button the game's mapping
     * uses (gameSources: the CPS bits the mapping reads, {@link #sourcesOf}) takes that button from the game. Null
     * when there is no such clash. The menu's never: its buttons still reach the game.
     */
    PadMapping.Source gameNeeds(Action a, int gameSources) {
        Chord c = chords.get(a);
        if (a == Action.MENU || c.isNone() || c.mod != null)
            return null;
        return (gameSources & ConsolePadMap.bit(c.button)) != 0 ? c.button : null;
    }

    /** Whether any action of console c's list has a conflict (shared chord, or a game button taken). */
    boolean hasConflict(Action a, Console c, int gameSources) {
        return !sharing(a, c).isEmpty() || gameNeeds(a, gameSources) != null;
    }

    /** The CPS bits a console mapping reads. */
    static int sourcesOf(ConsolePadMap m) {
        int s = 0;
        if (m != null)
            for (int v : m.source)
                s |= v;
        return s;
    }

    /** The CPS bits an Xbox mapping reads. */
    static int sourcesOf(PadMapping m) {
        int s = 0;
        if (m != null)
            for (PadMapping.Control c : PadMapping.Control.values()) {
                PadMapping.Source src = m.get(c);
                if (src != null && src != PadMapping.Source.NONE)
                    s |= ConsolePadMap.bit(src);
            }
        return s;
    }

    /** The actions a game of console c shows: all of them with save states, else the menu only. */
    static List<Action> actions(Console c) {
        List<Action> out = new ArrayList<>();
        for (Action a : Action.values()) {
            if (a.appliesTo(c))
                out.add(a);
        }
        return out;
    }

    // ---- Keys -------------------------------------------------------------------------------------------------------

    /** The source of an Android gamepad key (a gamepad's Back is its Select, as for SDL), or null. */
    static PadMapping.Source sourceOfKey(int keyCode) {
        switch (keyCode) {
        case KeyEvent.KEYCODE_BUTTON_A: return PadMapping.Source.SOUTH;
        case KeyEvent.KEYCODE_BUTTON_B: return PadMapping.Source.EAST;
        case KeyEvent.KEYCODE_BUTTON_X: return PadMapping.Source.WEST;
        case KeyEvent.KEYCODE_BUTTON_Y: return PadMapping.Source.NORTH;
        case KeyEvent.KEYCODE_BUTTON_L1: return PadMapping.Source.LEFT_SHOULDER;
        case KeyEvent.KEYCODE_BUTTON_R1: return PadMapping.Source.RIGHT_SHOULDER;
        case KeyEvent.KEYCODE_BUTTON_L2: return PadMapping.Source.LEFT_TRIGGER;
        case KeyEvent.KEYCODE_BUTTON_R2: return PadMapping.Source.RIGHT_TRIGGER;
        case KeyEvent.KEYCODE_BUTTON_THUMBL: return PadMapping.Source.LEFT_STICK_CLICK;
        case KeyEvent.KEYCODE_BUTTON_THUMBR: return PadMapping.Source.RIGHT_STICK_CLICK;
        case KeyEvent.KEYCODE_BUTTON_SELECT:
        case KeyEvent.KEYCODE_BACK: return PadMapping.Source.BACK;
        case KeyEvent.KEYCODE_BUTTON_START: return PadMapping.Source.START;
        case KeyEvent.KEYCODE_DPAD_UP: return PadMapping.Source.DPAD_UP;
        case KeyEvent.KEYCODE_DPAD_DOWN: return PadMapping.Source.DPAD_DOWN;
        case KeyEvent.KEYCODE_DPAD_LEFT: return PadMapping.Source.DPAD_LEFT;
        case KeyEvent.KEYCODE_DPAD_RIGHT: return PadMapping.Source.DPAD_RIGHT;
        default: return null;
        }
    }

    /** Whether a key event's source is a gamepad or joystick (its Back is a button, not the system's back). */
    static boolean fromGamepad(int source) {
        return (source & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
               || (source & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK;
    }

    /** The Android key of a source (the menu's combo listens to keys), or -1 for a stick direction. */
    static int keyOf(PadMapping.Source s) {
        if (s == null)
            return -1;
        switch (s) {
        case SOUTH: return KeyEvent.KEYCODE_BUTTON_A;
        case EAST: return KeyEvent.KEYCODE_BUTTON_B;
        case WEST: return KeyEvent.KEYCODE_BUTTON_X;
        case NORTH: return KeyEvent.KEYCODE_BUTTON_Y;
        case LEFT_SHOULDER: return KeyEvent.KEYCODE_BUTTON_L1;
        case RIGHT_SHOULDER: return KeyEvent.KEYCODE_BUTTON_R1;
        case LEFT_TRIGGER: return KeyEvent.KEYCODE_BUTTON_L2;
        case RIGHT_TRIGGER: return KeyEvent.KEYCODE_BUTTON_R2;
        case LEFT_STICK_CLICK: return KeyEvent.KEYCODE_BUTTON_THUMBL;
        case RIGHT_STICK_CLICK: return KeyEvent.KEYCODE_BUTTON_THUMBR;
        case BACK: return KeyEvent.KEYCODE_BUTTON_SELECT;
        case START: return KeyEvent.KEYCODE_BUTTON_START;
        case DPAD_UP: return KeyEvent.KEYCODE_DPAD_UP;
        case DPAD_DOWN: return KeyEvent.KEYCODE_DPAD_DOWN;
        case DPAD_LEFT: return KeyEvent.KEYCODE_DPAD_LEFT;
        case DPAD_RIGHT: return KeyEvent.KEYCODE_DPAD_RIGHT;
        default: return -1;
        }
    }

    /**
     * The Controls screen's « press the buttons »: the first button down is the modifier, the second makes the chord;
     * one button pressed and released alone is a chord of that button. Triggers come as keys (L2 / R2) or as axes
     * ({@link #triggers}). After the chord, it keeps eating the events until every button it saw is up (so the A or B
     * of a chord does not click or go back).
     */
    static final class Capture {
        static final float TRIGGER_DOWN = 0.5f;
        private final List<PadMapping.Source> down = new ArrayList<>();
        private boolean lt, rt;
        private Chord result;

        /** A source pressed or released: the chord when this completes it (once), else null. */
        Chord press(PadMapping.Source s, boolean isDown) {
            if (s == null || s == PadMapping.Source.NONE)
                return null;
            if (isDown) {
                if (!down.contains(s))
                    down.add(s);
                if (result == null && down.size() >= 2)
                    return result = new Chord(down.get(0), down.get(1));
            } else if (down.contains(s)) {
                Chord done = result == null && down.size() == 1 ? new Chord(null, s) : null;
                down.remove(s);
                if (done != null)
                    return result = done;
            }
            return null;
        }

        Chord key(int keyCode, boolean isDown) {
            return press(sourceOfKey(keyCode), isDown);
        }

        /** The trigger axes (0..1): past {@link #TRIGGER_DOWN} is a press. */
        Chord triggers(float l, float r) {
            Chord c = null;
            boolean nl = l > TRIGGER_DOWN, nr = r > TRIGGER_DOWN;
            if (nl != lt) {
                lt = nl;
                c = press(PadMapping.Source.LEFT_TRIGGER, nl);
            }
            if (nr != rt) {
                rt = nr;
                Chord c2 = press(PadMapping.Source.RIGHT_TRIGGER, nr);
                if (c == null)
                    c = c2;
            }
            return c;
        }

        Chord result() {
            return result;
        }

        /** True until the chord is made and every button is up again. */
        boolean active() {
            return result == null || !down.isEmpty();
        }
    }

    // ---- Where they are kept -------------------------------------------------------------------------------------

    /** Every game's (recomp.cfg). */
    static Hotkeys loadGlobal(RecompConfig cfg) {
        return parse(cfg == null ? null : cfg.get(KEY));
    }

    /** Into cfg (not saved): the line, none for the defaults. */
    static void storeGlobal(RecompConfig cfg, Hotkeys h) {
        String t = h.format();
        cfg.set(KEY, t.isEmpty() ? null : t);
    }

    /** The game's own text (« » for the defaults), or null when it follows every game's. */
    static String gameText(Context c, String key) {
        return key == null || key.isEmpty() ? null : GameSettings.raw(c, key, KEY);
    }

    /** The game gets its own hotkeys (h), or follows every game's again (null). */
    static boolean storeGame(Context c, String key, Hotkeys h) {
        return key != null && !key.isEmpty() && GameSettings.put(c, key, KEY, h == null ? null : h.format());
    }

    /** What the game plays with: its own, else every game's (recomp.cfg in ext, may be null). */
    static Hotkeys effective(Context c, File ext, String key) {
        String own = gameText(c, key);
        if (own != null)
            return parse(own);
        return loadGlobal(ext == null ? null : RecompConfig.load(ext));
    }

    // ---- Words --------------------------------------------------------------------------------------------------

    static int label(Action a) {
        switch (a) {
        case FF_HOLD: return R.string.HK_FF_HOLD;
        case FF_TOGGLE: return R.string.HK_FF_TOGGLE;
        case QSAVE: return R.string.HK_QSAVE;
        case QLOAD: return R.string.HK_QLOAD;
        case SLOT_NEXT: return R.string.HK_SLOT_NEXT;
        case SLOT_PREV: return R.string.HK_SLOT_PREV;
        default: return R.string.HK_MENU;
        }
    }

    /** « Select + RB », « None ». */
    static String chordLabel(Context c, Chord ch) {
        if (ch == null || ch.isNone())
            return c.getString(R.string.HK_NONE);
        StringBuilder b = new StringBuilder();
        for (PadMapping.Source s : ch.sources())
            b.append(b.length() == 0 ? "" : " + ").append(c.getString(ControlsLabels.source(s)));
        return b.toString();
    }
}
