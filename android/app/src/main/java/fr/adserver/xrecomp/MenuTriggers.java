package fr.adserver.xrecomp;

import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * The in-game menu's triggers as plain state machines (docs/android/app-shell.md « In-game menu »), so the JVM tests
 * drive them with made-up events and times: the swipe in from the left edge of the screen, and the gamepad combo
 * (Select + Start, or the player's « Game menu » hotkey) held {@link #COMBO_MS}.
 */
final class MenuTriggers {
    private MenuTriggers() {}

    /** Select + Start held this long opens the menu (the Xbox games use each alone, not both held together). */
    static final long COMBO_MS = 500;

    /** What a touch event means for the game. */
    enum Touch {
        /** Not ours: the game gets it. */
        PASS,
        /** The swipe was recognised with this event: cancel the game's touch, open the menu. */
        TAKE,
        /** The rest of a taken swipe: nobody else gets it. */
        CONSUME
    }

    /**
     * A swipe in from the left edge: a pointer that goes down within {@link #EDGE_FRAC} of the width (at least
     * edgeMinPx) from the left edge and travels right by {@link #TRAVEL_FRAC} of the width, more sideways than up or
     * down. Until then every event passes to the game; a second pointer, or a pointer that goes the wrong way, ends
     * the tracking.
     */
    static final class EdgeSwipe {
        static final float EDGE_FRAC = 0.03f, TRAVEL_FRAC = 0.06f;
        private enum State { IDLE, TRACKING, TAKEN }

        private State state = State.IDLE;
        private float x0, y0;
        private final float edgeMinPx;

        EdgeSwipe(float edgeMinPx) {
            this.edgeMinPx = edgeMinPx;
        }

        boolean taken() {
            return state == State.TAKEN;
        }

        void reset() {
            state = State.IDLE;
        }

        Touch onTouch(int action, float x, float y, int width) {
            switch (action) {
            case MotionEvent.ACTION_DOWN:
                state = x <= Math.max(edgeMinPx, width * EDGE_FRAC) ? State.TRACKING : State.IDLE;
                x0 = x;
                y0 = y;
                return Touch.PASS;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (state == State.TRACKING)
                    state = State.IDLE;
                return state == State.TAKEN ? Touch.CONSUME : Touch.PASS;
            case MotionEvent.ACTION_MOVE:
                if (state == State.TAKEN)
                    return Touch.CONSUME;
                if (state == State.TRACKING) {
                    float dx = x - x0, dy = Math.abs(y - y0);
                    if (dx >= width * TRAVEL_FRAC && dx > dy * 1.5f) {
                        state = State.TAKEN;
                        return Touch.TAKE;
                    }
                    if (dx < -width * EDGE_FRAC || dy > width * TRAVEL_FRAC)
                        state = State.IDLE;
                }
                return Touch.PASS;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                boolean was = state == State.TAKEN;
                state = State.IDLE;
                return was ? Touch.CONSUME : Touch.PASS;
            }
            default:
                return state == State.TAKEN ? Touch.CONSUME : Touch.PASS;
            }
        }
    }

    /**
     * The menu's pad chord held {@link #COMBO_MS}: Select + Start by default, the player's « Game menu » hotkey
     * (Hotkeys, XBA-190) when set ({@link #setChord}): both held at once, from the moment the second one goes down; a
     * chord of one button: that button held alone; none: the combo never fires (Guide, Back and the swipe still
     * open the menu). The keys still reach the game (each alone is a game button); the menu, once open, releases them
     * in the game.
     */
    static final class Combo {
        private int keyA = KeyEvent.KEYCODE_BUTTON_SELECT, keyB = KeyEvent.KEYCODE_BUTTON_START;
        private boolean a, b;
        private long since = -1;

        static boolean isSelect(int keyCode) {
            return keyCode == KeyEvent.KEYCODE_BUTTON_SELECT;
        }

        static boolean isStart(int keyCode) {
            return keyCode == KeyEvent.KEYCODE_BUTTON_START;
        }

        /** The chord to listen for; a chord with a stick direction (no key) keeps Select + Start. */
        void setChord(Hotkeys.Chord c) {
            reset();
            if (c == null || c.isNone()) {
                keyA = keyB = -1;
                return;
            }
            int kb = Hotkeys.keyOf(c.button), ka = c.mod == null ? -1 : Hotkeys.keyOf(c.mod);
            if (kb < 0 || (c.mod != null && ka < 0)) {
                keyA = KeyEvent.KEYCODE_BUTTON_SELECT;
                keyB = KeyEvent.KEYCODE_BUTTON_START;
                return;
            }
            keyA = c.mod == null ? kb : ka;
            keyB = c.mod == null ? -1 : kb;
        }

        /** False when the menu has no pad chord. */
        boolean enabled() {
            return keyA >= 0;
        }

        /** A key event; true when it was one of the chord's keys. */
        boolean key(int keyCode, boolean down, long now) {
            if (keyCode < 0)
                return false;
            if (keyCode == keyA)
                a = down;
            else if (keyCode == keyB)
                b = down;
            else
                return false;
            if (a && (b || keyB < 0)) {
                if (since < 0)
                    since = now;
            } else {
                since = -1;
            }
            return true;
        }

        boolean held() {
            return since >= 0;
        }

        /** When the combo fires (both held since + COMBO_MS), or -1 when it is not held. */
        long dueAt() {
            return since < 0 ? -1 : since + COMBO_MS;
        }

        boolean due(long now) {
            return since >= 0 && now - since >= COMBO_MS;
        }

        void reset() {
            a = b = false;
            since = -1;
        }
    }
}
