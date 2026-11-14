package fr.adserver.xrecomp;

import java.util.Locale;

/**
 * The player's mapping for a GBA, PS1 or N64 game (XBA-158 / XBA-168 / XBA-174): which of their pad's controls each of
 * the console's buttons takes, and which stick its analog sticks follow. The Java twin of src/input/console_pad.c,
 * which each console's runtime applies: the same targets in the same order, the same sources (PadMapping.Source, whose
 * order is console_pad.h's CPS_* bits), the same text ("target=source[+source...]", "none", "left_stick=",
 * "right_stick=" / "stick=", "range=", "deadzone=", only what differs from the default). The defaults are the fixed
 * mappings the runtimes had before (no line: the game plays as it did).
 *
 * A target may take several sources (the GBA's A is south + north by default); the screen steps a target through one
 * source at a time, so a remapped target has one. No Android here: the JVM tests drive it.
 */
final class ConsolePadMap {
    /** Range values the N64 screen steps through; -1 = the runtime's (RECOMP_N64_STICK_RANGE, else 80). */
    static final int[] RANGES = { -1, 64, 72, 80, 88, 96, 104, 112, 127 };
    /** Dead zones in percent; -1 = the runtime's (RECOMP_N64_DEADZONE, else 8). */
    static final int[] DEADZONES = { -1, 0, 4, 8, 12, 16, 20, 25, 30 };

    private static final PadMapping.Source[] SOURCES = sources();

    private static PadMapping.Source[] sources() {
        PadMapping.Source[] all = PadMapping.Source.values();
        PadMapping.Source[] s = new PadMapping.Source[all.length - 1];   // without NONE
        System.arraycopy(all, 0, s, 0, s.length);
        return s;
    }

    static int bit(PadMapping.Source s) {
        return s == PadMapping.Source.NONE ? 0 : 1 << s.ordinal();
    }

    private static int bits(PadMapping.Source... s) {
        int b = 0;
        for (PadMapping.Source x : s)
            b |= bit(x);
        return b;
    }

    /** The console's targets, console_pad.h's order (GBA_T_*, PS1_T_*, N64_T_*). */
    static String[] targets(Console c) {
        switch (c) {
        case GBA:
            return new String[] { "a", "b", "l", "r", "start", "select", "dpad_up", "dpad_down", "dpad_left",
                                  "dpad_right" };
        case PS1:
            return new String[] { "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "start",
                                  "select", "dpad_up", "dpad_down", "dpad_left", "dpad_right" };
        case N64:
            return new String[] { "a", "b", "z", "l", "r", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                  "c_up", "c_down", "c_left", "c_right" };
        default:
            return new String[0];
        }
    }

    /** The console's analog sticks' keys in the text: the PS1's two, the N64's one, none on a GBA. */
    static String[] sticks(Console c) {
        switch (c) {
        case PS1:
            return new String[] { "left_stick", "right_stick" };
        case N64:
            return new String[] { "stick" };
        default:
            return new String[0];
        }
    }

    /** Whether the console has the stick's range and dead zone (the N64). */
    static boolean hasRange(Console c) {
        return c == Console.N64;
    }

    static int[] defaultSources(Console c) {
        PadMapping.Source S = PadMapping.Source.SOUTH, E = PadMapping.Source.EAST, W = PadMapping.Source.WEST,
                          N = PadMapping.Source.NORTH;
        int up = bit(PadMapping.Source.DPAD_UP), down = bit(PadMapping.Source.DPAD_DOWN),
            left = bit(PadMapping.Source.DPAD_LEFT), right = bit(PadMapping.Source.DPAD_RIGHT);
        int start = bit(PadMapping.Source.START), back = bit(PadMapping.Source.BACK);
        int l1 = bit(PadMapping.Source.LEFT_SHOULDER), r1 = bit(PadMapping.Source.RIGHT_SHOULDER);
        int l2 = bit(PadMapping.Source.LEFT_TRIGGER), r2 = bit(PadMapping.Source.RIGHT_TRIGGER);
        switch (c) {
        case GBA:
            return new int[] { bits(S, N), bits(E, W), l1 | l2, r1 | r2, start, back,
                               up | bit(PadMapping.Source.LSTICK_UP), down | bit(PadMapping.Source.LSTICK_DOWN),
                               left | bit(PadMapping.Source.LSTICK_LEFT), right | bit(PadMapping.Source.LSTICK_RIGHT) };
        case PS1:
            return new int[] { bit(S), bit(E), bit(W), bit(N), l1, r1, l2, r2, bit(PadMapping.Source.LEFT_STICK_CLICK),
                               bit(PadMapping.Source.RIGHT_STICK_CLICK), start, back, up, down, left, right };
        case N64:
            return new int[] { bit(S), bits(E, W), l2 | r2, l1, r1, start, up, down, left, right,
                               bit(N) | bit(PadMapping.Source.RSTICK_UP), bit(PadMapping.Source.RSTICK_DOWN),
                               bit(PadMapping.Source.RSTICK_LEFT), bit(PadMapping.Source.RSTICK_RIGHT) };
        default:
            return new int[0];
        }
    }

    static PadMapping.Stick[] defaultSticks(Console c) {
        switch (c) {
        case PS1:
            return new PadMapping.Stick[] { PadMapping.Stick.LEFT, PadMapping.Stick.RIGHT };
        case N64:
            return new PadMapping.Stick[] { PadMapping.Stick.LEFT };
        default:
            return new PadMapping.Stick[0];
        }
    }

    final Console console;
    final int[] source;
    final PadMapping.Stick[] stick;
    int range = -1, deadzone = -1;

    ConsolePadMap(Console c) {
        console = c;
        source = defaultSources(c);
        stick = defaultSticks(c);
    }

    int targetCount() {
        return source.length;
    }

    /** Everything back to the console's usual mapping. */
    void reset() {
        int[] d = defaultSources(console);
        System.arraycopy(d, 0, source, 0, d.length);
        PadMapping.Stick[] s = defaultSticks(console);
        System.arraycopy(s, 0, stick, 0, s.length);
        range = deadzone = -1;
    }

    boolean isDefault() {
        return format().isEmpty();
    }

    /** How many targets, sticks and stick settings differ from the default. */
    int changedCount() {
        int n = 0;
        int[] d = defaultSources(console);
        for (int i = 0; i < source.length; i++)
            n += source[i] != d[i] ? 1 : 0;
        PadMapping.Stick[] s = defaultSticks(console);
        for (int i = 0; i < stick.length; i++)
            n += stick[i] != s[i] ? 1 : 0;
        return n + (range != -1 ? 1 : 0) + (deadzone != -1 ? 1 : 0);
    }

    /** The sources of target t, in PadMapping.Source order (empty: not used). */
    PadMapping.Source[] sourcesOf(int t) {
        int n = Integer.bitCount(source[t]), k = 0;
        PadMapping.Source[] out = new PadMapping.Source[n];
        for (PadMapping.Source s : SOURCES)
            if ((source[t] & bit(s)) != 0)
                out[k++] = s;
        return out;
    }

    /**
     * The next (dir 1) or previous (-1) single source for target t, through every source then « not used », wrapping;
     * a target with several sources steps from its first. Returns the source set (NONE: not used).
     */
    PadMapping.Source step(int t, int dir) {
        int n = SOURCES.length + 1, cur = n - 1;   // n - 1: not used
        if (source[t] != 0)
            cur = Integer.numberOfTrailingZeros(source[t]);
        int i = Math.floorMod(cur + dir, n);
        source[t] = i == n - 1 ? 0 : bit(SOURCES[i]);
        return i == n - 1 ? PadMapping.Source.NONE : SOURCES[i];
    }

    void stepStick(int i, int dir) {
        stick[i] = PadMapping.stepStick(stick[i], dir);
    }

    static int stepIn(int[] values, int v, int dir) {
        int at = 0;
        for (int i = 0; i < values.length; i++)
            if (values[i] == v)
                at = i;
        return values[Math.floorMod(at + dir, values.length)];
    }

    /** The other targets that take one of target t's sources (allowed: one press gives both). */
    java.util.List<Integer> sharing(int t) {
        java.util.List<Integer> out = new java.util.ArrayList<>();
        for (int o = 0; o < source.length; o++)
            if (o != t && (source[o] & source[t]) != 0)
                out.add(o);
        return out;
    }

    /** The text for console_pad_parse: what differs from the default, in console_pad.c's order; "" when nothing. */
    String format() {
        StringBuilder b = new StringBuilder();
        String[] names = targets(console);
        int[] d = defaultSources(console);
        for (int t = 0; t < source.length; t++) {
            if (source[t] == d[t])
                continue;
            b.append(b.length() == 0 ? "" : ",").append(names[t]).append('=');
            if (source[t] == 0) {
                b.append("none");
                continue;
            }
            boolean first = true;
            for (PadMapping.Source s : sourcesOf(t)) {
                b.append(first ? "" : "+").append(s.key);
                first = false;
            }
        }
        String[] sk = sticks(console);
        PadMapping.Stick[] ds = defaultSticks(console);
        for (int i = 0; i < stick.length; i++)
            if (stick[i] != ds[i])
                b.append(b.length() == 0 ? "" : ",").append(sk[i]).append('=').append(stick[i].key);
        if (hasRange(console)) {
            if (range > 0)
                b.append(b.length() == 0 ? "" : ",").append("range=").append(range);
            if (deadzone >= 0)
                b.append(b.length() == 0 ? "" : ",").append("deadzone=").append(deadzone);
        }
        return b.toString();
    }

    /** Reads the text as console_pad.c does (on top of the defaults); entries it does not know are skipped. */
    static ConsolePadMap parse(Console c, String text) {
        ConsolePadMap m = new ConsolePadMap(c);
        if (text == null)
            return m;
        String[] names = targets(c), sk = sticks(c);
        for (String e : text.split("[,;\\s]+")) {
            int eq = e.indexOf('=');
            if (eq <= 0)
                continue;
            String k = e.substring(0, eq).toLowerCase(Locale.ROOT), v = e.substring(eq + 1).toLowerCase(Locale.ROOT);
            int si = indexOf(sk, k);
            if (si >= 0) {
                PadMapping.Stick s = PadMapping.Stick.of(v);
                if (s != null)
                    m.stick[si] = s;
                continue;
            }
            if (hasRange(c) && (k.equals("range") || k.equals("deadzone"))) {
                try {
                    int x = Integer.parseInt(v);
                    if (k.equals("range") && x >= 1 && x <= 127)
                        m.range = x;
                    else if (k.equals("deadzone") && x >= 0 && x <= 90)
                        m.deadzone = x;
                } catch (NumberFormatException ignored) {
                }
                continue;
            }
            int t = indexOf(names, k);
            if (t < 0)
                continue;
            if (v.equals("none")) {
                m.source[t] = 0;
                continue;
            }
            int set = 0;
            boolean ok = !v.isEmpty() && !v.endsWith("+");
            for (String part : v.split("\\+")) {
                PadMapping.Source s = PadMapping.Source.of(part);
                if (s == null || s == PadMapping.Source.NONE) {
                    ok = false;
                    break;
                }
                set |= bit(s);
            }
            if (ok && set != 0)
                m.source[t] = set;
        }
        return m;
    }

    private static int indexOf(String[] a, String k) {
        for (int i = 0; i < a.length; i++)
            if (a[i].equals(k))
                return i;
        return -1;
    }

    ConsolePadMap copy() {
        ConsolePadMap m = new ConsolePadMap(console);
        System.arraycopy(source, 0, m.source, 0, source.length);
        System.arraycopy(stick, 0, m.stick, 0, stick.length);
        m.range = range;
        m.deadzone = deadzone;
        return m;
    }
}
