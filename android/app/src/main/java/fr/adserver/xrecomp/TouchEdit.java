package fr.adserver.xrecomp;

import java.util.HashMap;
import java.util.Map;

/**
 * The touch layout editor's gestures (XBA-207), pure Java so TouchLayoutTest drives them: a finger on a control
 * selects it and drags it (its centre on the grid when the grid is on); a second finger while the first drags pinches
 * the selected control's size; a tap on an empty spot drops the selection. The buttons of the editor's panel act on
 * the selection ({@link #grow}, {@link #toggleHidden}) or on everything ({@link #reset}).
 */
final class TouchEdit {
    final TouchPad pad;
    boolean grid = true;
    TouchPad.Control selected;
    private final Map<Integer, float[]> fingers = new HashMap<>();
    private int dragPointer = -1, pinchPointer = -1;
    private float offX, offY, pinchDist, pinchSize;

    TouchEdit(TouchPad pad) {
        this.pad = pad;
    }

    /** The grid's step in pixels: square cells, TouchLayout.GRID_ROWS over the height. */
    float gridStep() {
        return pad.height() / TouchLayout.GRID_ROWS;
    }

    void down(int pointer, float x, float y) {
        fingers.put(pointer, new float[] { x, y });
        if (dragPointer < 0) {
            TouchPad.Control c = pad.pick(x, y);
            selected = c;
            if (c != null) {
                dragPointer = pointer;
                offX = c.cx - x;
                offY = c.cy - y;
            }
        } else if (pinchPointer < 0 && selected != null) {
            pinchPointer = pointer;
            float[] d = fingers.get(dragPointer);
            pinchDist = Math.max(1f, dist(d[0], d[1], x, y));
            pinchSize = selected.size;
        }
    }

    void move(int pointer, float x, float y) {
        float[] p = fingers.get(pointer);
        if (p == null)
            return;
        p[0] = x;
        p[1] = y;
        if (selected == null || dragPointer < 0)
            return;
        float[] d = fingers.get(dragPointer);
        if (pinchPointer >= 0) {
            float[] q = fingers.get(pinchPointer);
            pad.resize(selected, pinchSize * dist(d[0], d[1], q[0], q[1]) / pinchDist);
        } else if (pointer == dragPointer) {
            float nx = x + offX, ny = y + offY;
            if (grid) {
                nx = TouchLayout.snap(nx, gridStep());
                ny = TouchLayout.snap(ny, gridStep());
            }
            pad.moveTo(selected, nx, ny);
        }
    }

    void up(int pointer) {
        fingers.remove(pointer);
        if (pointer == dragPointer || pointer == pinchPointer) {
            // Either finger lifting ends the pinch; the drag goes on with the finger left only from a fresh touch.
            dragPointer = -1;
            pinchPointer = -1;
        }
    }

    void cancel() {
        fingers.clear();
        dragPointer = pinchPointer = -1;
    }

    /** The selected control a step bigger (d = 1) or smaller (d = -1): 10 % of its default size. */
    void grow(int d) {
        if (selected != null)
            pad.resize(selected, Math.round((selected.size + d * 0.1f) * 10f) / 10f);
    }

    void toggleHidden() {
        if (selected != null)
            selected.hidden = !selected.hidden;
    }

    /** Every control back to its default place, size and visibility. */
    void reset() {
        int id = selected != null ? selected.id : -1;
        cancel();
        pad.setCustom(null);
        selected = id > 0 ? pad.control(id) : null;
    }

    TouchLayout layout() {
        return TouchLayout.capture(pad);
    }

    private static float dist(float x0, float y0, float x1, float y1) {
        float dx = x1 - x0, dy = y1 - y0;
        return (float)Math.sqrt(dx * dx + dy * dy);
    }
}
