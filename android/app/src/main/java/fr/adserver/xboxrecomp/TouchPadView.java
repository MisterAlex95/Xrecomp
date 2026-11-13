package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * The on-screen pad over the game (XBA-42): a transparent full-screen View above SDL's surface, under the in-game
 * menu. Drawn by the app rather than in the native present pass: the window this View is in is composited over the
 * SurfaceView whatever it holds (the in-game menu's layer is there already), so a View that only redraws when a
 * finger changes something costs the game's GL frame nothing, with either renderer; the native presenter would draw
 * it into every frame, twice (GPU and software back ends). Touch goes from here straight to the overlay
 * (input_overlay.h) on the UI thread, read by the next pad report: no SDL event queue in between.
 *
 * Every pointer of every MotionEvent goes to {@link TouchPad}; the state is sent only when it changed. Released on
 * ACTION_CANCEL, when hidden, and when detached.
 */
final class TouchPadView extends View {
    interface Listener {
        void state(TouchPad.State s);
    }

    final TouchPad pad = new TouchPad();
    private final Listener listener;
    private TouchPad.State last = new TouchPad.State();
    private float scale = 1f;
    private int opacity = 50;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
                        text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private final Path path = new Path();

    TouchPadView(Context c, Listener listener) {
        super(c);
        this.listener = listener;
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.MEDIUM);
        setHapticFeedbackEnabled(true);
    }

    void apply(TouchSettings s) {
        scale = s.size / 100f;
        opacity = s.opacity;
        if (getWidth() > 0)
            pad.layout(getWidth(), getHeight(), scale);
        send();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        pad.layout(w, h, scale);
        send();
    }

    @Override
    public void setVisibility(int v) {
        if (v != VISIBLE)
            releaseAll();
        super.setVisibility(v);
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    void releaseAll() {
        pad.cancel();
        send();
        invalidate();
    }

    TouchPad.State state() {
        return last;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), idx = e.getActionIndex();
        switch (a) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN:
            pad.down(e.getPointerId(idx), e.getX(idx), e.getY(idx));
            break;
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < e.getPointerCount(); i++)
                pad.move(e.getPointerId(i), e.getX(i), e.getY(i));
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP:
            pad.up(e.getPointerId(idx));
            break;
        case MotionEvent.ACTION_CANCEL:
            pad.cancel();
            break;
        default:
            return true;
        }
        send();
        return true;   // a touch on an empty spot does nothing; the game has no use for it either
    }

    /** The state to the listener when it changed; a click of feedback when a button goes down. */
    private void send() {
        TouchPad.State s = pad.state();
        if (s.equals(last))
            return;
        boolean pressed = (s.buttons & ~last.buttons) != 0 || (s.analogMask & ~last.analogMask) != 0
                          || (s.lt > 0 && last.lt == 0) || (s.rt > 0 && last.rt == 0);
        last = s;
        if (pressed)
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        if (listener != null)
            listener.state(s);
        invalidate();
    }

    // ---- Drawing -------------------------------------------------------------------------------------------------

    private int a(int argb, float k) {
        int alpha = Math.round(((argb >>> 24) & 0xFF) * k * opacity / 100f);
        return (Math.max(0, Math.min(255, alpha)) << 24) | (argb & 0xFFFFFF);
    }

    @Override
    protected void onDraw(Canvas c) {
        TouchPad.State s = last;
        for (TouchPad.Control k : pad.controls) {
            boolean held = s.held(k);
            switch (k.kind) {
            case STICK:
                drawStick(c, k, s);
                break;
            case DPAD:
                drawDpad(c, k, s.buttons);
                break;
            default:
                drawButton(c, k, held);
                break;
            }
        }
    }

    private void base(Canvas c, float cx, float cy, float r, boolean held) {
        fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
        c.drawCircle(cx, cy, r, fill);
        stroke.setStrokeWidth(Math.max(2f, r * 0.05f));
        stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
        c.drawCircle(cx, cy, r, stroke);
    }

    private void drawStick(Canvas c, TouchPad.Control k, TouchPad.State s) {
        int vx = k.id == TouchPad.LSTICK ? s.lx : s.rx, vy = k.id == TouchPad.LSTICK ? s.ly : s.ry;
        fill.setColor(a(0x50202020, 1f));
        c.drawCircle(k.cx, k.cy, k.r, fill);
        stroke.setStrokeWidth(Math.max(2f, k.r * 0.03f));
        stroke.setColor(a(0xB0FFFFFF, 1f));
        c.drawCircle(k.cx, k.cy, k.r, stroke);
        float kx = k.cx + vx / 32767f * k.r * 0.6f, ky = k.cy - vy / 32767f * k.r * 0.6f;
        boolean held = vx != 0 || vy != 0;
        base(c, kx, ky, k.r * 0.42f, held);
    }

    private void drawDpad(Canvas c, TouchPad.Control k, int buttons) {
        float r = k.r, w = r * 0.36f;
        int[][] arms = { { TouchPad.XB_UP, 0, -1 }, { TouchPad.XB_DOWN, 0, 1 }, { TouchPad.XB_LEFT, -1, 0 },
                         { TouchPad.XB_RIGHT, 1, 0 } };
        for (int[] arm : arms) {
            boolean held = (buttons & arm[0]) != 0;
            float x0 = k.cx + (arm[1] == 0 ? -w : arm[1] < 0 ? -r : w * 0.9f);
            float x1 = k.cx + (arm[1] == 0 ? w : arm[1] < 0 ? -w * 0.9f : r);
            float y0 = k.cy + (arm[2] == 0 ? -w : arm[2] < 0 ? -r : w * 0.9f);
            float y1 = k.cy + (arm[2] == 0 ? w : arm[2] < 0 ? -w * 0.9f : r);
            rect.set(x0, y0, x1, y1);
            fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
            c.drawRoundRect(rect, w * 0.4f, w * 0.4f, fill);
            stroke.setStrokeWidth(Math.max(2f, r * 0.03f));
            stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
            c.drawRoundRect(rect, w * 0.4f, w * 0.4f, stroke);
            // The arm's arrow.
            float ax = k.cx + arm[1] * r * 0.68f, ay = k.cy + arm[2] * r * 0.68f, t = w * 0.45f;
            path.reset();
            path.moveTo(ax + arm[1] * t, ay + arm[2] * t);
            path.lineTo(ax - arm[1] * t * 0.6f + arm[2] * t, ay - arm[2] * t * 0.6f + arm[1] * t);
            path.lineTo(ax - arm[1] * t * 0.6f - arm[2] * t, ay - arm[2] * t * 0.6f - arm[1] * t);
            path.close();
            fill.setColor(a(held ? 0xFF101010 : 0xE0FFFFFF, 1f));
            c.drawPath(path, fill);
        }
    }

    /** The face buttons in the Duke's colours, White and Black as discs, the rest as labelled outlines. */
    private void drawButton(Canvas c, TouchPad.Control k, boolean held) {
        int face = 0;
        switch (k.id) {
        case TouchPad.A: face = 0xFF3FB34F; break;
        case TouchPad.B: face = 0xFFE0413A; break;
        case TouchPad.X: face = 0xFF3B7FE0; break;
        case TouchPad.Y: face = 0xFFE8C433; break;
        default: break;
        }
        if (k.pill()) {
            rect.set(k.cx - k.hw, k.cy - k.hh, k.cx + k.hw, k.cy + k.hh);
            fill.setColor(a(held ? 0xC0FFFFFF : 0x70202020, 1f));
            c.drawRoundRect(rect, k.hh, k.hh, fill);
            stroke.setStrokeWidth(Math.max(2f, k.hh * 0.08f));
            stroke.setColor(a(0xE0FFFFFF, held ? 1f : 0.8f));
            c.drawRoundRect(rect, k.hh, k.hh, stroke);
        } else if (k.id == TouchPad.WHITE || k.id == TouchPad.BLACK) {
            fill.setColor(a(k.id == TouchPad.WHITE ? 0xF0F2F2F2 : 0xF0080808, held ? 1f : 0.85f));
            c.drawCircle(k.cx, k.cy, k.r, fill);
            stroke.setStrokeWidth(Math.max(2f, k.r * (held ? 0.14f : 0.07f)));
            stroke.setColor(a(held ? 0xFFFFFFFF : 0xB0FFFFFF, 1f));
            c.drawCircle(k.cx, k.cy, k.r, stroke);
            return;
        } else {
            base(c, k.cx, k.cy, k.r, held);
        }
        if (k.id == TouchPad.BACK || k.id == TouchPad.START) {
            float d = k.id == TouchPad.BACK ? -1 : 1, t = k.hh * 0.45f;
            path.reset();
            path.moveTo(k.cx + d * t, k.cy);
            path.lineTo(k.cx - d * t * 0.7f, k.cy - t);
            path.lineTo(k.cx - d * t * 0.7f, k.cy + t);
            path.close();
            fill.setColor(a(held ? 0xFF101010 : 0xE0FFFFFF, 1f));
            c.drawPath(path, fill);
            return;
        }
        float size = k.pill() ? k.hh * 0.9f : k.r * (k.label.length() > 1 ? 0.62f : 0.9f);
        text.setTextSize(size);
        text.setColor(face != 0 ? a(face, held ? 1f : 0.95f) : a(held ? 0xFF101010 : 0xF0FFFFFF, 1f));
        if (held && face != 0)
            text.setColor(a(0xFF101010, 1f));
        c.drawText(k.label, k.cx, k.cy + size * 0.35f, text);
    }
}
