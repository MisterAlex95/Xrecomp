package fr.adserver.xrecomp;

import android.content.Context;

import java.util.List;

/**
 * The words of the Controls screen (XBA-131) for PadMapping's enums, and the pad diagram's labels under a mapping.
 * The rows, in the screen's order, grouped: what an Xbox player looks for first (the face buttons, Black and White,
 * the triggers), then Back / Start / the stick presses, the D-pad, the sticks.
 */
final class ControlsLabels {
    private ControlsLabels() {}

    /** The screen's groups of rows: a label and its controls. */
    static final int[] GROUPS = { R.string.CTL_GROUP_BUTTONS, R.string.CTL_GROUP_SYSTEM, R.string.CTL_GROUP_DPAD };
    static final PadMapping.Control[][] GROUP_ROWS = {
        { PadMapping.Control.A, PadMapping.Control.B, PadMapping.Control.X, PadMapping.Control.Y,
          PadMapping.Control.BLACK, PadMapping.Control.WHITE, PadMapping.Control.LEFT_TRIGGER,
          PadMapping.Control.RIGHT_TRIGGER },
        { PadMapping.Control.BACK, PadMapping.Control.START, PadMapping.Control.LEFT_THUMB,
          PadMapping.Control.RIGHT_THUMB },
        { PadMapping.Control.DPAD_UP, PadMapping.Control.DPAD_DOWN, PadMapping.Control.DPAD_LEFT,
          PadMapping.Control.DPAD_RIGHT },
    };

    static int control(PadMapping.Control c) {
        switch (c) {
        case DPAD_UP: return R.string.XB_DPAD_UP;
        case DPAD_DOWN: return R.string.XB_DPAD_DOWN;
        case DPAD_LEFT: return R.string.XB_DPAD_LEFT;
        case DPAD_RIGHT: return R.string.XB_DPAD_RIGHT;
        case START: return R.string.XB_START;
        case BACK: return R.string.XB_BACK;
        case LEFT_THUMB: return R.string.XB_LTHUMB;
        case RIGHT_THUMB: return R.string.XB_RTHUMB;
        case A: return R.string.XB_A;
        case B: return R.string.XB_B;
        case X: return R.string.XB_X;
        case Y: return R.string.XB_Y;
        case BLACK: return R.string.XB_BLACK;
        case WHITE: return R.string.XB_WHITE;
        case LEFT_TRIGGER: return R.string.XB_LT;
        default: return R.string.XB_RT;
        }
    }

    static int source(PadMapping.Source s) {
        switch (s) {
        case SOUTH: return R.string.SRC_SOUTH;
        case EAST: return R.string.SRC_EAST;
        case WEST: return R.string.SRC_WEST;
        case NORTH: return R.string.SRC_NORTH;
        case LEFT_SHOULDER: return R.string.SRC_L1;
        case RIGHT_SHOULDER: return R.string.SRC_R1;
        case LEFT_TRIGGER: return R.string.SRC_L2;
        case RIGHT_TRIGGER: return R.string.SRC_R2;
        case LEFT_STICK_CLICK: return R.string.SRC_L3;
        case RIGHT_STICK_CLICK: return R.string.SRC_R3;
        case BACK: return R.string.SRC_SELECT;
        case START: return R.string.SRC_START;
        case DPAD_UP: return R.string.SRC_DPAD_UP;
        case DPAD_DOWN: return R.string.SRC_DPAD_DOWN;
        case DPAD_LEFT: return R.string.SRC_DPAD_LEFT;
        case DPAD_RIGHT: return R.string.SRC_DPAD_RIGHT;
        case LSTICK_UP: return R.string.SRC_LSTICK_UP;
        case LSTICK_DOWN: return R.string.SRC_LSTICK_DOWN;
        case LSTICK_LEFT: return R.string.SRC_LSTICK_LEFT;
        case LSTICK_RIGHT: return R.string.SRC_LSTICK_RIGHT;
        case RSTICK_UP: return R.string.SRC_RSTICK_UP;
        case RSTICK_DOWN: return R.string.SRC_RSTICK_DOWN;
        case RSTICK_LEFT: return R.string.SRC_RSTICK_LEFT;
        case RSTICK_RIGHT: return R.string.SRC_RSTICK_RIGHT;
        default: return R.string.SRC_NONE;
        }
    }

    static int stick(PadMapping.Stick s) {
        return s == PadMapping.Stick.LEFT ? R.string.STICK_LEFT : s == PadMapping.Stick.RIGHT ? R.string.STICK_RIGHT
                                                                                            : R.string.SRC_NONE;
    }

    /** A control's short name on the diagram: the console's own marks (A, LT, L3, ↑), Black and White translated. */
    static String shortName(Context c, PadMapping.Control x) {
        switch (x) {
        case DPAD_UP: return "↑";
        case DPAD_DOWN: return "↓";
        case DPAD_LEFT: return "←";
        case DPAD_RIGHT: return "→";
        case LEFT_THUMB: return "L3";
        case RIGHT_THUMB: return "R3";
        case LEFT_TRIGGER: return "LT";
        case RIGHT_TRIGGER: return "RT";
        case BLACK:
        case WHITE:
        case START:
        case BACK:
            return c.getString(control(x));
        default:
            return x.key.toUpperCase(java.util.Locale.ROOT);
        }
    }

    /** The Xbox controls a source gives, a short name, « Black +1 » for two (the list says which), « — » for none. */
    static String gives(Context c, PadMapping m, PadMapping.Source s) {
        List<PadMapping.Control> l = m.controlsOf(s);
        if (l.isEmpty())
            return c.getString(R.string.VAL_NONE);
        return shortName(c, l.get(0)) + (l.size() > 1 ? " +" + (l.size() - 1) : "");
    }

    /**
     * Widgets.PadDiagram's eleven labels (L1, L2, R1, R2, Select, Start, D-pad, left stick, right stick, face buttons,
     * screen) for the mapping: each of the player's controls with what the game gets from it.
     */
    static String[] diagram(Context c, PadMapping m) {
        String[] out = new String[11];
        PadMapping.Source[] at = { PadMapping.Source.LEFT_SHOULDER, PadMapping.Source.LEFT_TRIGGER,
                                   PadMapping.Source.RIGHT_SHOULDER, PadMapping.Source.RIGHT_TRIGGER,
                                   PadMapping.Source.BACK, PadMapping.Source.START };
        for (int i = 0; i < at.length; i++)
            out[i] = c.getString(source(at[i])) + " · " + gives(c, m, at[i]);
        out[6] = c.getString(R.string.PAD_DPAD);
        out[7] = c.getString(R.string.SRC_L3) + " · " + gives(c, m, PadMapping.Source.LEFT_STICK_CLICK);
        out[8] = c.getString(R.string.SRC_R3) + " · " + gives(c, m, PadMapping.Source.RIGHT_STICK_CLICK);
        // The face buttons in the order of the usual label (A B X Y: bottom, right, left, top): what each gives.
        PadMapping.Source[] face = { PadMapping.Source.SOUTH, PadMapping.Source.EAST, PadMapping.Source.WEST,
                                     PadMapping.Source.NORTH };
        StringBuilder b = new StringBuilder();
        for (PadMapping.Source s : face)
            b.append(b.length() == 0 ? "" : " ").append(gives(c, m, s));
        out[9] = b.toString();
        out[10] = c.getString(R.string.PAD_SCREEN);
        return out;
    }
}
