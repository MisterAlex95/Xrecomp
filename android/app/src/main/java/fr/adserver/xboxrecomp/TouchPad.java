package fr.adserver.xboxrecomp;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The on-screen pad's logic (XBA-42): where each control is and what the fingers on the screen press. Pure Java (no
 * Android), so TouchPadTest drives it with made-up pointers; TouchPadView draws it and feeds it MotionEvents.
 *
 * Layout, placed in thousandths of the screen's height and sized by that times the size setting, anchored to the
 * edges (the right side mirrors the left), the Duke's arrangement: left stick high and D-pad low on the left, A B X Y high and the
 * right stick low on the right, the triggers in the top corners, White and Black over the face buttons, Back and
 * Start at the bottom centre, the stick clicks (LS, RS) next to the sticks. Nothing within 3 % of the left edge,
 * where a swipe opens the in-game menu (MenuTriggers).
 *
 * Fingers: every pointer counts on its own. A finger that lands on a stick or the D-pad keeps it until it lifts
 * (sliding off a stick holds it at full tilt); a finger on a button can slide to the next button, as on the companion
 * panel. Sticks are analog (offset from the base's centre, a 6 % dead zone, the circle's edge is full tilt); the
 * D-pad gives eight directions; triggers are full when touched (Android gives no usable pressure).
 */
final class TouchPad {
    enum Kind { STICK, DPAD, BUTTON }

    // Xbox pad bits (xinput_xbox.h) and analog indices.
    static final int XB_UP = 0x01, XB_DOWN = 0x02, XB_LEFT = 0x04, XB_RIGHT = 0x08, XB_START = 0x10, XB_BACK = 0x20,
                     XB_LTHUMB = 0x40, XB_RTHUMB = 0x80;
    static final int AN_A = 0, AN_B = 1, AN_X = 2, AN_Y = 3, AN_BLACK = 4, AN_WHITE = 5, AN_LT = 6, AN_RT = 7;

    static final int LSTICK = 1, RSTICK = 2, DPAD = 3, A = 4, B = 5, X = 6, Y = 7, BLACK = 8, WHITE = 9, LT = 10,
                     RT = 11, BACK = 12, START = 13, L3 = 14, R3 = 15;

    static final float DEAD = 0.06f, STICK_SLOP = 1.3f, BUTTON_SLOP = 1.2f, EDGE_FRAC = 0.03f;

    /** One control: a circle (cx, cy, r) or, for the triggers and Back / Start, a pill (cx, cy, half w, half h). */
    static final class Control {
        final int id;
        final Kind kind;
        final String label;
        final int bits, analog;   // wButtons bits, bAnalogButtons index (-1 = none)
        float cx, cy, r, hw, hh;

        Control(int id, Kind kind, String label, int bits, int analog) {
            this.id = id;
            this.kind = kind;
            this.label = label;
            this.bits = bits;
            this.analog = analog;
        }

        boolean pill() {
            return hw > 0;
        }

        /** Distance-based hit with a slop factor; pills by their rectangle grown by the same factor. */
        boolean hit(float x, float y, float slop) {
            if (pill())
                return Math.abs(x - cx) <= hw * slop && Math.abs(y - cy) <= hh * slop;
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * slop * slop;
        }
    }

    /** The pad state the fingers make, as the overlay takes it (CompanionNative.nativeOverlayPad). */
    static final class State {
        int buttons, analogMask, lt, rt, lx, ly, rx, ry;

        boolean held(Control c) {
            if (c.kind == Kind.STICK)
                return c.id == LSTICK ? (lx != 0 || ly != 0) : (rx != 0 || ry != 0);
            if (c.analog == AN_LT)
                return lt > 0;
            if (c.analog == AN_RT)
                return rt > 0;
            return (c.bits != 0 && (buttons & c.bits) == c.bits) || (c.analog >= 0 && (analogMask & 1 << c.analog) != 0);
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof State))
                return false;
            State s = (State)o;
            return buttons == s.buttons && analogMask == s.analogMask && lt == s.lt && rt == s.rt && lx == s.lx
                   && ly == s.ly && rx == s.rx && ry == s.ry;
        }

        @Override
        public int hashCode() {
            return buttons * 31 + analogMask;
        }

        @Override
        public String toString() {
            return String.format(java.util.Locale.ROOT, "buttons=%02x analog=%02x lt=%d rt=%d L=(%d,%d) R=(%d,%d)",
                                 buttons, analogMask, lt, rt, lx, ly, rx, ry);
        }
    }

    final List<Control> controls = new ArrayList<>();
    private final Map<Integer, float[]> pointers = new HashMap<>();   // id -> { x, y }
    private final Map<Integer, Control> captured = new HashMap<>();   // id -> the stick or D-pad it landed on
    private float width, height;

    TouchPad() {
        controls.add(new Control(LSTICK, Kind.STICK, "", 0, -1));
        controls.add(new Control(RSTICK, Kind.STICK, "", 0, -1));
        controls.add(new Control(DPAD, Kind.DPAD, "", 0, -1));
        controls.add(new Control(A, Kind.BUTTON, "A", 0, AN_A));
        controls.add(new Control(B, Kind.BUTTON, "B", 0, AN_B));
        controls.add(new Control(X, Kind.BUTTON, "X", 0, AN_X));
        controls.add(new Control(Y, Kind.BUTTON, "Y", 0, AN_Y));
        controls.add(new Control(WHITE, Kind.BUTTON, "", 0, AN_WHITE));
        controls.add(new Control(BLACK, Kind.BUTTON, "", 0, AN_BLACK));
        controls.add(new Control(LT, Kind.BUTTON, "LT", 0, AN_LT));
        controls.add(new Control(RT, Kind.BUTTON, "RT", 0, AN_RT));
        controls.add(new Control(BACK, Kind.BUTTON, "", XB_BACK, -1));
        controls.add(new Control(START, Kind.BUTTON, "", XB_START, -1));
        controls.add(new Control(L3, Kind.BUTTON, "LS", XB_LTHUMB, -1));
        controls.add(new Control(R3, Kind.BUTTON, "RS", XB_RTHUMB, -1));
    }

    Control control(int id) {
        for (Control c : controls)
            if (c.id == id)
                return c;
        throw new IllegalArgumentException("no control " + id);
    }

    /** Places the controls on a w x h screen; scale is the size setting (1 = normal). Releases every finger. */
    void layout(float w, float h, float scale) {
        width = w;
        height = h;
        // Where things are: o (a thousandth of the height). How big they are, and the spacing inside a group: u (o
        // times the size setting), so a larger pad grows in place rather than running into its neighbours.
        float o = h / 1000f, u = o * scale;
        // Never closer to the left edge than the in-game menu's swipe zone allows, whatever the size.
        float minLeft = w * EDGE_FRAC;
        circle(LSTICK, left(300 * o, 150 * STICK_SLOP * u, minLeft), 400 * o, 150 * u);
        circle(DPAD, left(390 * o, 125 * 1.1f * u, minLeft), h - 230 * o, 125 * u);
        circle(L3, left(130 * o, 50 * BUTTON_SLOP * u, minLeft), 640 * o, 50 * u);
        pill(LT, left(165 * o, 110 * BUTTON_SLOP * u, minLeft), 95 * o, 110 * u, 55 * u);
        float fx = w - 300 * o, fy = 400 * o, gap = 105 * u, br = 62 * u;
        circle(A, fx, fy + gap, br);
        circle(B, fx + gap, fy, br);
        circle(X, fx - gap, fy, br);
        circle(Y, fx, fy - gap, br);
        circle(WHITE, fx - 130 * u, fy - 175 * o, 42 * u);
        circle(BLACK, fx + 130 * u, fy - 175 * o, 42 * u);
        circle(RSTICK, w - 410 * o, h - 225 * o, 150 * u);
        circle(R3, w - 130 * o, 640 * o, 50 * u);
        pill(RT, w - 165 * o, 95 * o, 110 * u, 55 * u);
        pill(BACK, w / 2 - 110 * u, h - 60 * o, 70 * u, 32 * u);
        pill(START, w / 2 + 110 * u, h - 60 * o, 70 * u, 32 * u);
        cancel();
    }

    /** A left-side x, pushed right so that x - reach stays out of the menu's edge zone. */
    private static float left(float x, float reach, float minLeft) {
        return Math.max(x, minLeft + reach);
    }

    private void circle(int id, float cx, float cy, float r) {
        Control c = control(id);
        c.cx = cx;
        c.cy = cy;
        c.r = r;
        c.hw = c.hh = 0;
    }

    private void pill(int id, float cx, float cy, float hw, float hh) {
        Control c = control(id);
        c.cx = cx;
        c.cy = cy;
        c.hw = hw;
        c.hh = hh;
        c.r = Math.min(hw, hh);
    }

    float width() {
        return width;
    }

    float height() {
        return height;
    }

    // ---- Fingers -------------------------------------------------------------------------------------------------

    void down(int pointer, float x, float y) {
        pointers.put(pointer, new float[] { x, y });
        Control c = at(x, y, true);
        if (c != null && c.kind != Kind.BUTTON)
            captured.put(pointer, c);
    }

    void move(int pointer, float x, float y) {
        float[] p = pointers.get(pointer);
        if (p == null)
            return;
        p[0] = x;
        p[1] = y;
    }

    void up(int pointer) {
        pointers.remove(pointer);
        captured.remove(pointer);
    }

    /** Everything released (ACTION_CANCEL, the view hidden or detached, the game stopping). */
    void cancel() {
        pointers.clear();
        captured.clear();
    }

    int fingers() {
        return pointers.size();
    }

    /** The control under (x, y): sticks and the D-pad only when landing (sliding never takes one). */
    Control at(float x, float y, boolean landing) {
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (!landing && c.kind != Kind.BUTTON)
                continue;
            float slop = c.kind == Kind.STICK ? STICK_SLOP : c.kind == Kind.DPAD ? 1.1f : BUTTON_SLOP;
            if (!c.hit(x, y, slop))
                continue;
            float dx = x - c.cx, dy = y - c.cy, d = (dx * dx + dy * dy) / (c.r * c.r);
            if (d < bestD) {
                bestD = d;
                best = c;
            }
        }
        return best;
    }

    /** The pad as the fingers hold it now. */
    State state() {
        State s = new State();
        for (Map.Entry<Integer, float[]> e : pointers.entrySet()) {
            float x = e.getValue()[0], y = e.getValue()[1];
            Control c = captured.get(e.getKey());
            if (c == null)
                c = at(x, y, false);
            if (c == null)
                continue;
            switch (c.kind) {
            case STICK:
                stick(s, c, x, y);
                break;
            case DPAD:
                s.buttons |= dpad(c, x, y);
                break;
            default:
                s.buttons |= c.bits;
                if (c.analog == AN_LT)
                    s.lt = 255;
                else if (c.analog == AN_RT)
                    s.rt = 255;
                else if (c.analog >= 0)
                    s.analogMask |= 1 << c.analog;
                break;
            }
        }
        return s;
    }

    /** A stick: the finger's offset from the centre over the radius, clamped to the circle, Y up. */
    static void stick(State s, Control c, float x, float y) {
        float dx = (x - c.cx) / c.r, dy = (c.cy - y) / c.r;
        float m = (float)Math.sqrt(dx * dx + dy * dy);
        int vx = 0, vy = 0;
        if (m > DEAD) {
            // The dead zone is taken off and the rest rescaled, so the edge is still full tilt.
            float scaled = (Math.min(1f, m) - DEAD) / (1f - DEAD);
            vx = Math.round(dx / m * scaled * 32767f);
            vy = Math.round(dy / m * scaled * 32767f);
        }
        if (c.id == LSTICK) {
            s.lx = vx;
            s.ly = vy;
        } else {
            s.rx = vx;
            s.ry = vy;
        }
    }

    /** Eight directions: each arm covers 135 degrees, so the diagonals press two; the centre fifth presses none. */
    static int dpad(Control c, float x, float y) {
        float dx = x - c.cx, dy = y - c.cy;
        if (dx * dx + dy * dy < c.r * c.r * 0.04f)
            return 0;
        final float t = 2.4142f;   // tan(67.5 degrees)
        int b = 0;
        if (dx > 0 && Math.abs(dy) < dx * t)
            b |= XB_RIGHT;
        if (dx < 0 && Math.abs(dy) < -dx * t)
            b |= XB_LEFT;
        if (dy < 0 && Math.abs(dx) < -dy * t)
            b |= XB_UP;
        if (dy > 0 && Math.abs(dx) < dy * t)
            b |= XB_DOWN;
        return b;
    }
}
