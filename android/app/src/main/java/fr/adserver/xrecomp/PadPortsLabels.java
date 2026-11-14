package fr.adserver.xrecomp;

import android.content.Context;

import java.util.List;

/** The words of Controls › Players (XBA-123) and Controls › On-screen pad (XBA-42). */
final class PadPortsLabels {
    private PadPortsLabels() {}

    /** A port row's value: First come, or the pinned controller (marked when not connected now). */
    static String pin(Context c, PadPorts p, int port, List<PadPorts.Device> present) {
        String key = p.pin(port);
        if (key == null)
            return c.getString(R.string.CTL_PORT_AUTO);
        for (PadPorts.Device d : present)
            if (d.key.equals(key))
                return device(c, d);
        String name = p.pinName(port);
        return c.getString(R.string.CTL_PORT_ABSENT, name == null || name.isEmpty() ? key : name);
    }

    static String device(Context c, PadPorts.Device d) {
        String n = d.name.isEmpty() ? d.key : d.name;
        return d.builtin ? c.getString(R.string.CTL_PORT_BUILTIN, n) : n;
    }

    /** « port 1, Thor Controller (built in); port 2, nobody … »: what the game's start would give each port. */
    static String now(Context c, PadPorts p, List<PadPorts.Device> present) {
        PadPorts.Device[] at = p.start(present);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < PadPorts.PORTS; i++) {
            if (b.length() > 0)
                b.append("; ");
            b.append(c.getString(R.string.CTL_PORT_LINE, i + 1,
                                 at[i] == null ? c.getString(R.string.CTL_PORT_NOBODY) : device(c, at[i])));
        }
        return c.getString(R.string.CTL_PORT_NOW, b);
    }

    static String mode(Context c, TouchSettings.Mode m) {
        switch (m) {
        case ON: return c.getString(R.string.CTL_TOUCH_ON);
        case OFF: return c.getString(R.string.CTL_TOUCH_OFF);
        default: return c.getString(R.string.CTL_TOUCH_AUTO);
        }
    }

    static String size(Context c, int size) {
        return c.getString(size < 100 ? R.string.CTL_SIZE_SMALL : size > 100 ? R.string.CTL_SIZE_LARGE
                                                                             : R.string.CTL_SIZE_NORMAL);
    }

    static String percent(Context c, int v) {
        return c.getString(R.string.PERCENT, v);
    }
}
