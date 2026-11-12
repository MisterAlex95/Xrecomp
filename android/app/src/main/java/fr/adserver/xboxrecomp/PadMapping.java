package fr.adserver.xboxrecomp;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The player's controller mapping (XBA-131): which button of their pad each Xbox control takes, one mapping for every
 * title and every pad. Stored in recomp.cfg as RECOMP_PAD_MAP, the text src/input/input_map.c parses
 * (xbox_InputMap_PadMapParse): "control=source" entries for what differs from the default, comma separated, plus
 * "left_stick=" / "right_stick=" (left, right, none). No line at all is the default (the fixed mapping of
 * src/input/xinput_device.c: L1 = Black, R1 = White, L2 / R2 = the triggers, Select = Back).
 *
 * The names are input_map.c's: controls in input_map.h's order, sources as SDL names a pad's controls by position
 * (south is the bottom face button, which Android calls A). Entries this class does not know (a source the shell does
 * not read, a typo) are dropped when the screen saves. No Android here: the JVM tests drive it, and the on-screen pad
 * of XBA-42 can use the same Control list.
 */
final class PadMapping {
    static final String KEY = "RECOMP_PAD_MAP";

    /** What a pad has, as the shell reads it (xinput_device.c padmap_read), in the order the screen cycles them. */
    enum Source {
        SOUTH("south"), EAST("east"), WEST("west"), NORTH("north"),
        LEFT_SHOULDER("left_shoulder"), RIGHT_SHOULDER("right_shoulder"),
        LEFT_TRIGGER("left_trigger"), RIGHT_TRIGGER("right_trigger"),
        LEFT_STICK_CLICK("left_stick_click"), RIGHT_STICK_CLICK("right_stick_click"),
        BACK("back"), START("start"),
        DPAD_UP("dpad_up"), DPAD_DOWN("dpad_down"), DPAD_LEFT("dpad_left"), DPAD_RIGHT("dpad_right"),
        LSTICK_UP("left_stick_up"), LSTICK_DOWN("left_stick_down"), LSTICK_LEFT("left_stick_left"),
        LSTICK_RIGHT("left_stick_right"),
        RSTICK_UP("right_stick_up"), RSTICK_DOWN("right_stick_down"), RSTICK_LEFT("right_stick_left"),
        RSTICK_RIGHT("right_stick_right"),
        NONE("none");

        final String key;

        Source(String key) {
            this.key = key;
        }

        static Source of(String key) {
            for (Source s : values()) {
                if (s.key.equalsIgnoreCase(key))
                    return s;
            }
            return null;
        }
    }

    /** The sixteen Xbox controls a pad button can be mapped onto, input_map.h's InputControl order, with defaults. */
    enum Control {
        DPAD_UP("dpad_up", Source.DPAD_UP), DPAD_DOWN("dpad_down", Source.DPAD_DOWN),
        DPAD_LEFT("dpad_left", Source.DPAD_LEFT), DPAD_RIGHT("dpad_right", Source.DPAD_RIGHT),
        START("start", Source.START), BACK("back", Source.BACK),
        LEFT_THUMB("left_thumb", Source.LEFT_STICK_CLICK), RIGHT_THUMB("right_thumb", Source.RIGHT_STICK_CLICK),
        A("a", Source.SOUTH), B("b", Source.EAST), X("x", Source.WEST), Y("y", Source.NORTH),
        BLACK("black", Source.LEFT_SHOULDER), WHITE("white", Source.RIGHT_SHOULDER),
        LEFT_TRIGGER("left_trigger", Source.LEFT_TRIGGER), RIGHT_TRIGGER("right_trigger", Source.RIGHT_TRIGGER);

        final String key;
        final Source def;

        Control(String key, Source def) {
            this.key = key;
            this.def = def;
        }

        static Control of(String key) {
            for (Control c : values()) {
                if (c.key.equalsIgnoreCase(key))
                    return c;
            }
            return null;
        }
    }

    /** Which physical stick an Xbox stick takes. */
    enum Stick {
        LEFT("left"), RIGHT("right"), NONE("none");

        final String key;

        Stick(String key) {
            this.key = key;
        }

        static Stick of(String key) {
            for (Stick s : values()) {
                if (s.key.equalsIgnoreCase(key))
                    return s;
            }
            return null;
        }
    }

    private final Source[] source = new Source[Control.values().length];
    Stick leftStick = Stick.LEFT, rightStick = Stick.RIGHT;

    PadMapping() {
        reset();
    }

    Source get(Control c) {
        return source[c.ordinal()];
    }

    void set(Control c, Source s) {
        source[c.ordinal()] = s == null ? Source.NONE : s;
    }

    /** Everything back to the default mapping. */
    void reset() {
        for (Control c : Control.values())
            source[c.ordinal()] = c.def;
        leftStick = Stick.LEFT;
        rightStick = Stick.RIGHT;
    }

    boolean isDefault() {
        return format().isEmpty();
    }

    /** How many controls and sticks differ from the default. */
    int changedCount() {
        int n = 0;
        for (Control c : Control.values())
            n += get(c) != c.def ? 1 : 0;
        return n + (leftStick != Stick.LEFT ? 1 : 0) + (rightStick != Stick.RIGHT ? 1 : 0);
    }

    /** The next (dir 1) or previous (-1) source for c, wrapping. */
    Source step(Control c, int dir) {
        Source[] all = Source.values();
        int n = all.length, i = ((get(c).ordinal() + dir) % n + n) % n;
        set(c, all[i]);
        return all[i];
    }

    static Stick stepStick(Stick s, int dir) {
        Stick[] all = Stick.values();
        int n = all.length;
        return all[((s.ordinal() + dir) % n + n) % n];
    }

    /** The Xbox controls that take s (the reverse view, for the pad diagram and the XBA-42 overlay's labels). */
    List<Control> controlsOf(Source s) {
        List<Control> out = new ArrayList<>();
        for (Control c : Control.values()) {
            if (get(c) == s)
                out.add(c);
        }
        return out;
    }

    /** The other Xbox controls that take the same source as c (two controls on one button: allowed, worth a word). */
    List<Control> sharing(Control c) {
        List<Control> out = new ArrayList<>();
        if (get(c) == Source.NONE)
            return out;
        for (Control o : Control.values()) {
            if (o != c && get(o) == get(c))
                out.add(o);
        }
        return out;
    }

    /** The RECOMP_PAD_MAP text: what differs from the default, input_map.c's order; "" for the default mapping. */
    String format() {
        StringBuilder b = new StringBuilder();
        for (Control c : Control.values()) {
            if (get(c) != c.def)
                b.append(b.length() == 0 ? "" : ",").append(c.key).append('=').append(get(c).key);
        }
        if (leftStick != Stick.LEFT)
            b.append(b.length() == 0 ? "" : ",").append("left_stick=").append(leftStick.key);
        if (rightStick != Stick.RIGHT)
            b.append(b.length() == 0 ? "" : ",").append("right_stick=").append(rightStick.key);
        return b.toString();
    }

    /** Reads the text as input_map.c does (commas, semicolons or spaces between entries); unknown entries are skipped. */
    static PadMapping parse(String text) {
        PadMapping m = new PadMapping();
        if (text == null)
            return m;
        for (String e : text.split("[,;\\s]+")) {
            int eq = e.indexOf('=');
            if (eq <= 0)
                continue;
            String k = e.substring(0, eq).toLowerCase(Locale.ROOT), v = e.substring(eq + 1);
            if (k.equals("left_stick") || k.equals("right_stick")) {
                Stick s = Stick.of(v);
                if (s != null && k.equals("left_stick"))
                    m.leftStick = s;
                else if (s != null)
                    m.rightStick = s;
                continue;
            }
            Control c = Control.of(k);
            Source s = Source.of(v);
            if (c != null && s != null)
                m.set(c, s);
        }
        return m;
    }

    static PadMapping load(RecompConfig cfg) {
        return parse(cfg.get(KEY));
    }

    /** Into cfg (not saved): the line, or none for the default mapping. */
    void store(RecompConfig cfg) {
        String t = format();
        cfg.set(KEY, t.isEmpty() ? null : t);
    }
}
