package fr.adserver.xboxrecomp;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Which controller is which Xbox port (XBA-123): the player's pins, RECOMP_PAD_PORTS in recomp.cfg, and the start
 * rule of src/input/input_ports.c, so the Controls screen shows what the game will do. No Android here (PadDevices
 * reads the devices); PadPortsTest has the same start cases as tests/input_pad_ports.
 *
 * A controller is known by "vvvv:pppp" (USB vendor:product, lower case hex), which SDL and InputDevice both give;
 * two pads of one model are interchangeable. The text is "1=045e:0b13:Xbox Wireless Controller,2=2dc8:3106", a port,
 * a key and optionally a name (for the screen when that pad is not connected; the runtime ignores it). No line: first
 * come. At the game's start the controllers present are packed from port 1: the pinned ones in port order, then the
 * handheld's built-in pad, then the rest; a pinned pad that is absent leaves no hole. During the game a port waits
 * for its controller across a disconnect.
 */
final class PadPorts {
    static final String KEY = "RECOMP_PAD_PORTS", BUILTIN_ENV = "RECOMP_PAD_BUILTIN";
    static final int PORTS = 4;

    /** A connected controller: its key, its name, whether it is the handheld's own. */
    static final class Device {
        final String key, name;
        final boolean builtin;

        Device(String key, String name, boolean builtin) {
            this.key = key;
            this.name = name == null ? "" : name;
            this.builtin = builtin;
        }

        @Override
        public String toString() {
            return name + " (" + key + ")";
        }
    }

    private final String[] pin = new String[PORTS], pinName = new String[PORTS];

    static String key(int vendor, int product) {
        return String.format(Locale.ROOT, "%04x:%04x", vendor & 0xFFFF, product & 0xFFFF);
    }

    static boolean isKey(String s) {
        return s != null && s.matches("[0-9a-fA-F]{4}:[0-9a-fA-F]{4}");
    }

    /** RECOMP_PAD_PORTS; entries it does not understand are dropped (as input_ports.c ignores them). */
    static PadPorts parse(String text) {
        PadPorts p = new PadPorts();
        if (text == null)
            return p;
        for (String e : text.split(",")) {
            String s = e.trim();
            if (s.length() < 11 || s.charAt(1) != '=' || s.charAt(0) < '1' || s.charAt(0) > '4')
                continue;
            String k = s.substring(2, 11);
            if (!isKey(k) || (s.length() > 11 && s.charAt(11) != ':'))
                continue;
            int port = s.charAt(0) - '1';
            p.pin[port] = k.toLowerCase(Locale.ROOT);
            p.pinName[port] = s.length() > 12 ? s.substring(12).trim() : null;
        }
        return p;
    }

    static PadPorts load(RecompConfig cfg) {
        return parse(cfg.get(KEY));
    }

    /** Into cfg (not saved): the line, or none when every port is first come. */
    void store(RecompConfig cfg) {
        String t = format();
        cfg.set(KEY, t.isEmpty() ? null : t);
    }

    String format() {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < PORTS; i++) {
            if (pin[i] == null)
                continue;
            if (b.length() > 0)
                b.append(',');
            b.append(i + 1).append('=').append(pin[i]);
            if (pinName[i] != null && !pinName[i].isEmpty())
                b.append(':').append(clean(pinName[i]));
        }
        return b.toString();
    }

    /** A name as the text can hold it: no comma, no line break. */
    private static String clean(String name) {
        return name.replace(',', ' ').replace('\n', ' ').replace('\r', ' ').trim();
    }

    /** The pinned key of a port, or null (first come). */
    String pin(int port) {
        return pin[port];
    }

    String pinName(int port) {
        return pinName[port];
    }

    boolean isDefault() {
        for (String s : pin)
            if (s != null)
                return false;
        return true;
    }

    /** Pins d to port (null: first come). The same controller pinned elsewhere is unpinned there. */
    void set(int port, Device d) {
        if (d != null) {
            for (int i = 0; i < PORTS; i++)
                if (d.key.equals(pin[i])) {
                    pin[i] = null;
                    pinName[i] = null;
                }
        }
        pin[port] = d == null ? null : d.key;
        pinName[port] = d == null ? null : clean(d.name);
    }

    void reset() {
        for (int i = 0; i < PORTS; i++) {
            pin[i] = null;
            pinName[i] = null;
        }
    }

    /**
     * The game's start with these controllers present, in this order (input_ports.c xbox_InputPortsStart): the
     * controller each port gets, null for an empty one.
     */
    Device[] start(List<Device> present) {
        Device[] out = new Device[PORTS];
        List<Device> order = new ArrayList<>();
        boolean[] used = new boolean[present.size()];
        for (int k = 0; k < PORTS; k++) {
            if (pin[k] == null)
                continue;
            for (int i = 0; i < present.size(); i++)
                if (!used[i] && pin[k].equals(present.get(i).key)) {
                    used[i] = true;
                    order.add(present.get(i));
                    break;
                }
        }
        String builtin = builtinKey(present);
        for (int i = 0; i < present.size(); i++)
            if (!used[i] && builtin != null && builtin.equals(present.get(i).key)) {
                used[i] = true;
                order.add(present.get(i));
            }
        for (int i = 0; i < present.size(); i++)
            if (!used[i]) {
                used[i] = true;
                order.add(present.get(i));
            }
        for (int k = 0; k < PORTS && k < order.size(); k++)
            out[k] = order.get(k);
        return out;
    }

    /** RECOMP_PAD_BUILTIN: the key of the first built-in controller, or null. */
    static String builtinKey(List<Device> present) {
        for (Device d : present)
            if (d.builtin)
                return d.key;
        return null;
    }

    /**
     * What a port's row cycles through: first come (null), every controller connected, and the pinned one when it
     * is not connected (so stepping does not lose it).
     */
    List<Device> choices(int port, List<Device> present) {
        List<Device> out = new ArrayList<>();
        out.add(null);
        List<String> seen = new ArrayList<>();
        for (Device d : present) {
            if (seen.contains(d.key))
                continue;   // one entry per model: two of them are interchangeable
            seen.add(d.key);
            out.add(d);
        }
        if (pin[port] != null && !seen.contains(pin[port]))
            out.add(new Device(pin[port], pinName[port], false));
        return out;
    }

    /** Steps a port's pin through {@link #choices}, d = +1 or -1. Returns the new pin (null: first come). */
    Device step(int port, int d, List<Device> present) {
        List<Device> c = choices(port, present);
        int at = 0;
        for (int i = 1; i < c.size(); i++)
            if (c.get(i).key.equals(pin[port]))
                at = i;
        Device next = c.get(Math.floorMod(at + d, c.size()));
        set(port, next);
        return next;
    }
}
