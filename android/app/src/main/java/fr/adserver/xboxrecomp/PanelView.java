package fr.adserver.xboxrecomp;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.os.SystemClock;
import android.text.TextPaint;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The base of the second-screen panels (docs/android/app-shell.md « Dual screen »): one plain View drawn on a
 * 1240 x 1080 design grid (the AYN Thor's bottom screen) scaled to fit, with touch buttons, every pointer tracked on
 * its own. Cheap on purpose, as it shares the GPU with the game: no animation; a 1 Hz tick (stats, dimming) that
 * invalidates only when something changed; redraws while a hold button is held (800 ms at most).
 *
 * Burn-in (AMOLED): pure black background; after {@link #dimAfterMs} without a touch the panel dims (to faint
 * outlines, or what the subclass draws dimmed); the layout shifts by a few pixels each minute. The touch that wakes a
 * dimmed panel presses pad buttons but fires no action.
 */
abstract class PanelView extends View {
    static final float W = 1240, H = 1080;
    static final long HOLD_MS = 800, TICK_MS = 1000;

    /** The clock (uptime ms); the tests set their own. */
    interface Clock {
        long now();
    }

    static Clock clock = SystemClock::uptimeMillis;

    enum Kind { PAD, TAP, TOGGLE, HOLD }

    static final class Button {
        final int id;
        final Kind kind;
        final RectF r;
        final PanelIcons.Icon icon;
        String label, hint;
        /** PAD: the wButtons bits and the analog-button mask (bit i = bAnalogButtons[i] at 255) it holds. */
        int padButtons, padAnalog;
        boolean on, enabled = true, accent;

        Button(int id, Kind kind, float l, float t, float r, float b, PanelIcons.Icon icon, String label, String hint) {
            this.id = id;
            this.kind = kind;
            this.r = new RectF(l, t, r, b);
            this.icon = icon;
            this.label = label;
            this.hint = hint;
        }
    }

    /** Small per-minute moves of the whole layout, in design pixels. */
    static final int[][] SHIFTS = { { 0, 0 }, { 5, 3 }, { -4, 5 }, { 3, -5 }, { -5, -3 }, { 6, -1 }, { -2, 6 },
                                    { -6, 1 } };

    protected final List<Button> buttons = new ArrayList<>();
    private final SparseArray<Button> current = new SparseArray<>();   // pointer id -> button under it
    private final SparseArray<Button> downOn = new SparseArray<>();    // pointer id -> button it went down on
    private final Set<Integer> waking = new HashSet<>();               // pointers that woke the panel
    protected final long dimAfterMs;
    private long lastTouch, attachedAt;
    private boolean dimmed;
    private int shift;
    private float scale = 1, ox, oy;
    private Button holding;
    private long holdStart;
    private int padButtons, padAnalog;

    protected final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    protected final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    protected final TextPaint text = new TextPaint(Paint.ANTI_ALIAS_FLAG);

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            tickNow();
            if (isAttachedToWindow())
                postDelayed(this, TICK_MS);
        }
    };

    private final Runnable holdDone = () -> {
        Button b = holding;
        holding = null;
        if (b != null) {
            invalidate();
            onAction(b);
        }
    };

    PanelView(Context c, long dimAfterMs) {
        super(c);
        this.dimAfterMs = dimAfterMs;
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        setBackgroundColor(0xFF000000);
        lastTouch = attachedAt = clock.now();
    }

    // ---- Hooks ------------------------------------------------------------------------------------------------------

    /** The pad buttons held changed (OR of every pointer's PAD button). */
    protected void onPad(int buttons, int analogMask) {}

    /** A TAP or TOGGLE button was released over itself, or a HOLD button held for HOLD_MS. */
    protected abstract void onAction(Button b);

    /** Once a second: refresh what the panel shows; true when it changed (the panel is then redrawn). */
    protected boolean onTick() {
        return false;
    }

    /** Draws the panel on the design grid; dimmed: only faint outlines (burn-in). */
    protected abstract void drawPanel(Canvas c, boolean dimmed);

    // ---- State for subclasses and tests -------------------------------------------------------------------------

    boolean isDimmed() {
        return dimmed;
    }

    int shiftIndex() {
        return shift;
    }

    Button button(int id) {
        for (Button b : buttons) {
            if (b.id == id)
                return b;
        }
        return null;
    }

    /** The pad state now: { wButtons, analog mask }. */
    int[] padState() {
        return new int[] { padButtons, padAnalog };
    }

    boolean isHeld(Button b) {
        for (int i = 0; i < current.size(); i++) {
            if (current.valueAt(i) == b)
                return true;
        }
        return holding == b;
    }

    /** The 1 Hz work, callable by the tests. */
    void tickNow() {
        boolean changed = onTick();
        long now = clock.now();
        if (!dimmed && current.size() == 0 && holding == null && now - lastTouch >= dimAfterMs) {
            dimmed = true;
            changed = true;
        }
        int s = (int)(((now - attachedAt) / 60000) % SHIFTS.length);
        if (s != shift) {
            shift = s;
            changed = true;
        }
        if (changed)
            invalidate();
    }

    /** Wakes a dimmed panel (a touch does it too). */
    void wake() {
        lastTouch = clock.now();
        if (dimmed) {
            dimmed = false;
            invalidate();
        }
    }

    /** Lets go of every pointer and hold: pad buttons released (onPad(0, 0) if anything was held). */
    void releaseAll() {
        current.clear();
        downOn.clear();
        waking.clear();
        if (holding != null) {
            removeCallbacks(holdDone);
            holding = null;
        }
        updatePad();
        invalidate();
    }

    // ---- Lifecycle -------------------------------------------------------------------------------------------------

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attachedAt = lastTouch = clock.now();
        shift = 0;
        dimmed = false;
        onTick();
        removeCallbacks(tick);
        postDelayed(tick, TICK_MS);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(tick);
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        scale = Math.min(w / W, h / H);
        ox = (w - W * scale) / 2;
        oy = (h - H * scale) / 2;
    }

    // ---- Touch -----------------------------------------------------------------------------------------------------

    private Button hit(MotionEvent e, int index) {
        float x = (e.getX(index) - ox) / scale - SHIFTS[shift][0];
        float y = (e.getY(index) - oy) / scale - SHIFTS[shift][1];
        // A little slop around each button: fingers are bigger than their centre point.
        for (Button b : buttons) {
            if (b.enabled && x >= b.r.left - 12 && x <= b.r.right + 12 && y >= b.r.top - 12 && y <= b.r.bottom + 12)
                return b;
        }
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        switch (action) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN: {
            int i = e.getActionIndex(), id = e.getPointerId(i);
            boolean wasDim = dimmed;
            wake();
            if (wasDim)
                waking.add(id);
            Button b = hit(e, i);
            current.put(id, b);
            downOn.put(id, b);
            if (b != null && b.kind == Kind.HOLD && !wasDim && holding == null) {
                holding = b;
                holdStart = clock.now();
                postDelayed(holdDone, HOLD_MS);
            }
            updatePad();
            invalidate();
            return true;
        }
        case MotionEvent.ACTION_MOVE: {
            boolean changed = false;
            for (int i = 0; i < e.getPointerCount(); i++) {
                int id = e.getPointerId(i);
                Button b = hit(e, i), was = current.get(id);
                if (b != was) {
                    current.put(id, b);
                    changed = true;
                    if (holding != null && was == holding && downOn.get(id) == holding)
                        cancelHold();
                }
            }
            if (changed) {
                lastTouch = clock.now();
                updatePad();
                invalidate();
            }
            return true;
        }
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP: {
            int i = e.getActionIndex(), id = e.getPointerId(i);
            Button over = hit(e, i), d = downOn.get(id);
            boolean woke = waking.remove(id);
            current.remove(id);
            downOn.remove(id);
            lastTouch = clock.now();
            if (holding != null && d == holding)
                cancelHold();
            if (d != null && d == over && !woke && (d.kind == Kind.TAP || d.kind == Kind.TOGGLE))
                onAction(d);
            updatePad();
            invalidate();
            return true;
        }
        case MotionEvent.ACTION_CANCEL:
            releaseAll();
            return true;
        default:
            return true;
        }
    }

    private void cancelHold() {
        removeCallbacks(holdDone);
        holding = null;
    }

    private void updatePad() {
        int pb = 0, pa = 0;
        for (int i = 0; i < current.size(); i++) {
            Button b = current.valueAt(i);
            if (b != null && b.kind == Kind.PAD) {
                pb |= b.padButtons;
                pa |= b.padAnalog;
            }
        }
        if (pb != padButtons || pa != padAnalog) {
            padButtons = pb;
            padAnalog = pa;
            onPad(pb, pa);
        }
    }

    // ---- Drawing ---------------------------------------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas c) {
        c.drawColor(0xFF000000);
        c.save();
        c.translate(ox, oy);
        c.scale(scale, scale);
        c.translate(SHIFTS[shift][0], SHIFTS[shift][1]);
        drawPanel(c, dimmed);
        c.restore();
        if (holding != null)
            postInvalidateOnAnimation();   // the hold ring, for HOLD_MS at most
    }

    /** How far the held HOLD button is, 0..1 (0 when b is not held). */
    protected float holdFraction(Button b) {
        if (holding != b)
            return 0;
        return Math.max(0, Math.min(1, (clock.now() - holdStart) / (float)HOLD_MS));
    }

    protected void drawButton(Canvas c, Button b, boolean dimmed) {
        RectF r = b.r;
        float rad = 30;
        if (dimmed) {
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.alpha(Theme.TEXT_FAINT, 0.32f));
            c.drawRoundRect(r, rad, rad, stroke);
            return;
        }
        boolean held = isHeld(b), hold = b.kind == Kind.HOLD;
        float a = b.enabled ? 1 : 0.4f;
        // A hold button does something that cannot be undone: it fills in red while held.
        int face = held ? (hold ? 0xFF2A1210 : Theme.ACCENT_DEEP) : b.accent ? 0xFF143A26 : Theme.PANEL_HI;
        int edge = held ? (hold ? Theme.ERROR : Theme.ACCENT) : b.on ? Theme.ACCENT
                 : b.accent ? Theme.ACCENT_DEEP : Theme.LINE;
        fill.setColor(Theme.alpha(face, a));
        c.drawRoundRect(r, rad, rad, fill);
        stroke.setStrokeWidth(b.on || held ? 4 : 3);
        stroke.setColor(Theme.alpha(edge, a));
        c.drawRoundRect(r, rad, rad, stroke);

        float hf = holdFraction(b);
        if (hf > 0) {
            fill.setColor(Theme.alpha(Theme.ERROR, 0.35f));
            c.save();
            c.clipRect(r.left, r.bottom - r.height() * hf, r.right, r.bottom);
            c.drawRoundRect(r, rad, rad, fill);
            c.restore();
        }

        int ink = Theme.alpha(held ? 0xFFFFFFFF : b.accent || b.on ? Theme.ACCENT : Theme.TEXT, a);
        boolean hasHint = b.hint != null && !b.hint.isEmpty();
        float cy = r.centerY() - (hasHint ? 34 : 22);
        if (b.icon != null) {
            stroke.setColor(ink);
            stroke.setStrokeWidth(6);
            PanelIcons.draw(c, b.icon, r.centerX(), cy - 18, Math.min(84, r.height() * 0.36f), stroke);
        }
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(b.label.length() > 9 ? 32 : 38);
        text.setColor(ink);
        text.setTextAlign(Paint.Align.CENTER);
        float ly = b.icon != null ? cy + 62 : r.centerY() + (hasHint ? -4 : 13);
        drawFit(c, b.label, r.centerX(), ly, r.width() - 24);
        if (hasHint) {
            text.setTypeface(Theme.REGULAR);
            text.setTextSize(Theme.T_SMALL);
            text.setColor(Theme.alpha(held ? Theme.TEXT_DIM : Theme.TEXT_FAINT, a));
            drawFit(c, b.hint, r.centerX(), ly + 40, r.width() - 24);
        }
    }

    /** Text at x (the paint's alignment), cut with an ellipsis to width. */
    protected void drawFit(Canvas c, String s, float x, float y, float width) {
        c.drawText(fit(s, width), x, y, text);
    }

    /** s, or its start and an ellipsis, so that it fits width with the text paint. */
    protected String fit(String s, float width) {
        if (s == null || text.measureText(s) <= width)
            return s == null ? "" : s;
        int n = text.breakText(s, true, Math.max(0, width - text.measureText("…")), null);
        return s.substring(0, Math.max(0, n)).trim() + "…";
    }

    // ---- Device status (battery, thermal), shared by the panels ----------------------------------------------------

    /** « 82 % », « 82 % ⚡ », or "" when unknown. */
    static String battery(Context c) {
        BatteryManager bm = c.getSystemService(BatteryManager.class);
        if (bm == null)
            return "";
        int pct = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
        if (pct <= 0 || pct > 100)
            return "";
        return pct + " %" + (bm.isCharging() ? " +" : "");
    }

    /** The battery temperature in °C from the sticky battery broadcast, or NaN. */
    static float batteryTemp(Context c) {
        Intent i;
        try {
            i = c.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        } catch (RuntimeException e) {
            return Float.NaN;
        }
        int t = i == null ? Integer.MIN_VALUE : i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
        return t == Integer.MIN_VALUE ? Float.NaN : t / 10f;
    }

    /** PowerManager.getCurrentThermalStatus(), THERMAL_STATUS_NONE when unknown. */
    static int thermal(Context c) {
        PowerManager pm = c.getSystemService(PowerManager.class);
        return pm == null ? PowerManager.THERMAL_STATUS_NONE : pm.getCurrentThermalStatus();
    }

    static int thermalLabel(int status) {
        switch (status) {
        case PowerManager.THERMAL_STATUS_LIGHT: return R.string.THERMAL_LIGHT;
        case PowerManager.THERMAL_STATUS_MODERATE: return R.string.THERMAL_MODERATE;
        case PowerManager.THERMAL_STATUS_SEVERE: return R.string.THERMAL_SEVERE;
        case PowerManager.THERMAL_STATUS_CRITICAL: return R.string.THERMAL_CRITICAL;
        case PowerManager.THERMAL_STATUS_EMERGENCY: return R.string.THERMAL_EMERGENCY;
        case PowerManager.THERMAL_STATUS_SHUTDOWN: return R.string.THERMAL_SHUTDOWN;
        default: return R.string.THERMAL_NONE;
        }
    }

    static int thermalColor(int status) {
        if (status >= PowerManager.THERMAL_STATUS_SEVERE)
            return Theme.ERROR;
        if (status >= PowerManager.THERMAL_STATUS_MODERATE)
            return Theme.WARN;
        return Theme.TEXT_DIM;
    }

    /** A status item: a small dot of colour, then the text, from x; returns the x after it. */
    protected float drawStatus(Canvas c, float x, float y, int dot, String s, int color) {
        if (dot != 0) {
            fill.setColor(dot);
            c.drawCircle(x + 9, y - 11, 9, fill);
            x += 30;
        }
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(30);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(color);
        c.drawText(s, x, y, text);
        return x + text.measureText(s);
    }

    static String fmt(String f, Object... a) {
        return String.format(Locale.ROOT, f, a);
    }

    /** A rounded card behind a block. */
    protected void drawCard(Canvas c, RectF r) {
        fill.setColor(Theme.PANEL);
        c.drawRoundRect(r, 28, 28, fill);
        stroke.setStrokeWidth(2);
        stroke.setColor(Theme.LINE);
        c.drawRoundRect(r, 28, 28, stroke);
    }

    /** The launcher panels' top strip: an optional caption on the left, battery, then thermal status on the right. */
    protected void drawStrip(Canvas c, String caption, String bat, int th) {
        Context ctx = getContext();
        float y = 92, x = 44;
        if (caption != null && !caption.isEmpty())
            x = drawStatus(c, x, y, Theme.ACCENT, caption, Theme.TEXT) + 40;
        if (!bat.isEmpty())
            drawStatus(c, x, y, 0, ctx.getString(R.string.PANEL_BATTERY, bat), Theme.TEXT_DIM);
        String ts = ctx.getString(thermalLabel(th));
        text.setTextSize(30);
        text.setTypeface(Theme.MEDIUM);
        float tw = text.measureText(ts) + 30;
        drawStatus(c, W - 44 - tw, y, thermalColor(th), ts, thermalColor(th));
    }
}
