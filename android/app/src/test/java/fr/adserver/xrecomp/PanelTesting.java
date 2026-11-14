package fr.adserver.xrecomp;

import android.graphics.RectF;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Multi-touch for the panel tests: pointers put down, moved and lifted by id, each change sent to the view as the
 * MotionEvent Android would send. Coordinates are design pixels (the views are laid out at 1240 x 1080, scale 1).
 */
final class PanelTesting {
    private final View view;
    private final Map<Integer, float[]> down = new LinkedHashMap<>();
    private final long downTime = SystemClock.uptimeMillis();

    PanelTesting(View view) {
        this.view = view;
    }

    static void layout(View v) {
        v.measure(View.MeasureSpec.makeMeasureSpec(1240, View.MeasureSpec.EXACTLY),
                  View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
        v.layout(0, 0, 1240, 1080);
    }

    static float[] centre(PanelView.Button b) {
        RectF r = b.r;
        return new float[] { r.centerX(), r.centerY() };
    }

    void press(int id, float[] at) {
        down.put(id, at.clone());
        int index = new ArrayList<>(down.keySet()).indexOf(id);
        int action = down.size() == 1 ? MotionEvent.ACTION_DOWN
                                      : MotionEvent.ACTION_POINTER_DOWN | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        send(action);
    }

    void move(int id, float[] to) {
        down.put(id, to.clone());
        send(MotionEvent.ACTION_MOVE);
    }

    void lift(int id) {
        int index = new ArrayList<>(down.keySet()).indexOf(id);
        int action = down.size() == 1 ? MotionEvent.ACTION_UP
                                      : MotionEvent.ACTION_POINTER_UP | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        send(action);
        down.remove(id);
    }

    void cancel() {
        send(MotionEvent.ACTION_CANCEL);
        down.clear();
    }

    void tap(float[] at) {
        press(99, at);
        lift(99);
    }

    private void send(int action) {
        int n = down.size();
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        int i = 0;
        for (Map.Entry<Integer, float[]> e : down.entrySet()) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = e.getKey();
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = e.getValue()[0];
            coords[i].y = e.getValue()[1];
            coords[i].pressure = 1;
            coords[i].size = 1;
            i++;
        }
        MotionEvent ev = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, n, props, coords, 0, 0, 1, 1,
                                            0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
        view.dispatchTouchEvent(ev);
        ev.recycle();
    }

    /** A sink that records what the panel sends. */
    static final class Sink implements CompanionPanel.Sink {
        int buttons = -1, analog = -1, releases;
        final List<Integer> actions = new ArrayList<>();
        final List<int[]> history = new ArrayList<>();

        @Override
        public void overlay(int buttons, int analogMask) {
            this.buttons = buttons;
            this.analog = analogMask;
            history.add(new int[] { buttons, analogMask });
        }

        @Override
        public void release() {
            releases++;
        }

        @Override
        public void action(int id) {
            actions.add(id);
        }
    }
}
