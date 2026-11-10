package fr.adserver.xboxrecomp;

import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * The in-game menu's triggers as plain state machines (docs/android/app-shell.md « In-game menu »), so the JVM tests
 * drive them with made-up events and times: the swipe in from the left edge of the screen, and the gamepad combo
 * Select + Start held {@link #COMBO_MS}.
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
     * Select + Start: both held at once, from the moment the second one goes down. The keys still reach the game
     * (each alone is a game button); the menu, once open, releases them in the game.
     */
    static final class Combo {
        private boolean select, start;
        private long since = -1;

        static boolean isSelect(int keyCode) {
            return keyCode == KeyEvent.KEYCODE_BUTTON_SELECT;
        }

        static boolean isStart(int keyCode) {
            return keyCode == KeyEvent.KEYCODE_BUTTON_START;
        }

        /** A key event; true when it was Select or Start. */
        boolean key(int keyCode, boolean down, long now) {
            if (isSelect(keyCode))
                select = down;
            else if (isStart(keyCode))
                start = down;
            else
                return false;
            if (select && start) {
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
            select = start = false;
            since = -1;
        }
    }
}
