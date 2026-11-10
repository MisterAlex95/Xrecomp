package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;

/**
 * The in-game companion panel on the second screen (docs/android/app-shell.md « Dual screen »), 1240 x 1080 design
 * grid:
 * <pre>
 *   fps · frame time                 battery                     thermal
 *   [ Back   ]   [Launcher] [Screenshot] [Perf ]   [ White ]
 *   [ Start  ]   [Share log] [  Quit (hold)    ]   [ Black ]
 *                 (perf details when Perf is on)
 *                 (nothing bottom-centre: palms)
 * </pre>
 * The edge columns sit next to the thumbs and duplicate Select/Start and L1/R1 (Back, Start, Black, White), so the
 * player finds them. They go to the pad through {@link Sink#overlay} (input_overlay.h); every pointer counts on its
 * own, and everything is released on ACTION_CANCEL, on detach (dismiss, onStop) and by {@link #releaseAll()}.
 */
final class CompanionPanel extends PanelView {
    /** Where the panel's buttons go: the native overlay in the game, a fake in the tests. */
    interface Sink {
        /** The overlay buttons now: wButtons bits, analog mask (bit i = bAnalogButtons[i] at 255). */
        void overlay(int buttons, int analogMask);

        /** The panel went away: the overlay is off (no stand-in pad any more). */
        void release();

        void action(int id);
    }

    /** { fps x 100, draws } (xbox_FramebufferWindowGetStats), or null when unknown. */
    interface Stats {
        int[] read();
    }

    // Xbox pad bits (xinput_xbox.h).
    static final int XB_START = 0x0010, XB_BACK = 0x0020, XB_BLACK = 4, XB_WHITE = 5;

    static final int BACK = 1, START = 2, WHITE = 3, BLACK = 4, LAUNCHER = 10, SCREENSHOT = 11, PERF = 12,
                     SHARE_LOG = 13, QUIT = 14;

    static final long DIM_MS = 10_000, FLASH_MS = 3000;

    private final Sink sink;
    private final Stats stats;
    private String fpsText = "", batteryText = "", flash = "";
    private int thermal = -1;
    private float temp = Float.NaN;
    private int draws;
    private long flashUntil;

    CompanionPanel(Context c, Sink sink, Stats stats) {
        super(c, DIM_MS);
        this.sink = sink;
        this.stats = stats;
        // Edge columns (next to the thumbs).
        Button b = new Button(BACK, Kind.PAD, 36, 150, 266, 490, null, c.getString(R.string.PANEL_BACK),
                              c.getString(R.string.PANEL_HINT_SELECT));
        b.padButtons = XB_BACK;
        buttons.add(b);
        b = new Button(START, Kind.PAD, 36, 515, 266, 855, null, c.getString(R.string.PANEL_START),
                       c.getString(R.string.PANEL_HINT_START));
        b.padButtons = XB_START;
        buttons.add(b);
        b = new Button(WHITE, Kind.PAD, 974, 150, 1204, 490, null, c.getString(R.string.PANEL_WHITE),
                       c.getString(R.string.PANEL_HINT_R1));
        b.padAnalog = 1 << XB_WHITE;
        buttons.add(b);
        b = new Button(BLACK, Kind.PAD, 974, 515, 1204, 855, null, c.getString(R.string.PANEL_BLACK),
                       c.getString(R.string.PANEL_HINT_L1));
        b.padAnalog = 1 << XB_BLACK;
        buttons.add(b);
        // Centre.
        buttons.add(new Button(LAUNCHER, Kind.TAP, 290, 150, 496, 400, PanelIcons.Icon.HOME,
                               c.getString(R.string.PANEL_LAUNCHER), null));
        buttons.add(new Button(SCREENSHOT, Kind.TAP, 517, 150, 723, 400, PanelIcons.Icon.CAMERA,
                               c.getString(R.string.PANEL_SCREENSHOT), null));
        buttons.add(new Button(PERF, Kind.TOGGLE, 744, 150, 950, 400, PanelIcons.Icon.BARS,
                               c.getString(R.string.PANEL_PERF), null));
        buttons.add(new Button(SHARE_LOG, Kind.TAP, 290, 425, 496, 675, PanelIcons.Icon.SHARE,
                               c.getString(R.string.PANEL_SHARE_LOG), null));
        buttons.add(new Button(QUIT, Kind.HOLD, 517, 425, 950, 675, PanelIcons.Icon.POWER,
                               c.getString(R.string.PANEL_QUIT), c.getString(R.string.PANEL_HOLD)));
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        sink.overlay(0, 0);   // overlay on: a connected pad 0 even without a gamepad
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();   // releaseAll -> overlay(0, 0) when something was held
        sink.overlay(0, 0);
        sink.release();
    }

    @Override
    protected void onPad(int buttons, int analogMask) {
        sink.overlay(buttons, analogMask);
    }

    @Override
    protected void onAction(Button b) {
        if (b.id == PERF) {
            b.on = !b.on;
            onTick();
            invalidate();
        }
        sink.action(b.id);
    }

    boolean perfOn() {
        return button(PERF).on;
    }

    /** A short message in the status strip (« Screenshot saved »), for FLASH_MS. */
    void flash(String s) {
        flash = s == null ? "" : s;
        flashUntil = clock.now() + FLASH_MS;
        wake();
        invalidate();
    }

    @Override
    protected boolean onTick() {
        Context c = getContext();
        int[] st = stats == null ? null : stats.read();
        String f;
        if (st == null || st[0] <= 0) {
            f = c.getString(R.string.PANEL_FPS_NONE);
        } else {
            float fps = st[0] / 100f;
            f = c.getString(R.string.PANEL_FPS, fmt("%.1f", fps), fmt("%.1f", 1000f / fps));
        }
        int d = st == null ? 0 : st[1];
        String bat = battery(c);
        int th = thermal(c);
        float t = perfOn() ? batteryTemp(c) : Float.NaN;
        boolean flashEnded = !flash.isEmpty() && clock.now() >= flashUntil;
        if (flashEnded)
            flash = "";
        boolean changed = !f.equals(fpsText) || !bat.equals(batteryText) || th != thermal || flashEnded
                          || (perfOn() && (d != draws || Float.compare(t, temp) != 0));
        fpsText = f;
        batteryText = bat;
        thermal = th;
        draws = d;
        temp = t;
        return changed;
    }

    @Override
    protected void drawPanel(Canvas c, boolean dimmed) {
        Context ctx = getContext();
        if (!dimmed) {
            float y = 92;
            if (!flash.isEmpty()) {
                drawStatus(c, 44, y, Theme.ACCENT, flash, Theme.TEXT);
            } else {
                drawStatus(c, 44, y, Theme.ACCENT, fpsText, Theme.TEXT);
            }
            if (!batteryText.isEmpty()) {
                String s = ctx.getString(R.string.PANEL_BATTERY, batteryText);
                text.setTextSize(30);
                text.setTypeface(Theme.MEDIUM);
                float w = text.measureText(s) + 30;
                drawStatus(c, W / 2 - w / 2 + 40, y, 0, s, Theme.TEXT_DIM);
            }
            String th = ctx.getString(thermalLabel(thermal));
            text.setTextSize(30);
            text.setTypeface(Theme.MEDIUM);
            float tw = text.measureText(th) + 30;
            drawStatus(c, W - 44 - tw, y, thermalColor(thermal), th, thermalColor(thermal));
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.LINE);
            c.drawLine(36, 122, W - 36, 122, stroke);
        }
        for (Button b : buttons) {
            if (b.kind == Kind.PAD)
                drawPad(c, b, dimmed);
            else
                drawButton(c, b, dimmed);
        }
        if (!dimmed && perfOn()) {
            text.setTypeface(Theme.REGULAR);
            text.setTextSize(30);
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(Theme.TEXT_DIM);
            c.drawText(ctx.getString(R.string.PANEL_PERF_LINE1, fpsText, draws), 620, 750, text);
            String tl = Float.isNaN(temp) ? ctx.getString(thermalLabel(thermal))
                                          : ctx.getString(R.string.PANEL_PERF_LINE2, fmt("%.1f", temp),
                                                          ctx.getString(thermalLabel(thermal)));
            c.drawText(tl, 620, 794, text);
        }
    }

    /** A pad button: the Xbox glyph (a white or black disc, a small oval for Back and Start), its name, its key. */
    private void drawPad(Canvas c, Button b, boolean dimmed) {
        RectF r = b.r;
        float cx = r.centerX(), gy = r.centerY() - 78;
        if (dimmed) {
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.alpha(Theme.TEXT_FAINT, 0.32f));
            c.drawRoundRect(r, 30, 30, stroke);
            c.drawCircle(cx, gy, 44, stroke);
            return;
        }
        boolean held = isHeld(b);
        fill.setColor(held ? Theme.ACCENT_DEEP : Theme.PANEL_HI);
        c.drawRoundRect(r, 30, 30, fill);
        stroke.setStrokeWidth(held ? 4 : 3);
        stroke.setColor(held ? Theme.ACCENT : Theme.LINE);
        c.drawRoundRect(r, 30, 30, stroke);
        if (b.id == WHITE || b.id == BLACK) {
            fill.setColor(b.id == WHITE ? Theme.GLYPH_FACE : 0xFF050505);
            c.drawCircle(cx, gy, 50, fill);
            stroke.setStrokeWidth(4);
            stroke.setColor(b.id == WHITE ? 0xFFFFFFFF : Theme.TEXT_DIM);
            c.drawCircle(cx, gy, 50, stroke);
        } else {
            RectF pill = new RectF(cx - 46, gy - 24, cx + 46, gy + 24);
            fill.setColor(0xFF050505);
            c.drawRoundRect(pill, 24, 24, fill);
            stroke.setStrokeWidth(4);
            stroke.setColor(Theme.TEXT_DIM);
            c.drawRoundRect(pill, 24, 24, stroke);
            // Back: an arrow pointing left; Start: pointing right (the Duke's own glyphs are plain ovals).
            fill.setColor(Theme.TEXT);
            float d = b.id == BACK ? -1 : 1;
            android.graphics.Path p = new android.graphics.Path();
            p.moveTo(cx + d * 14, gy);
            p.lineTo(cx - d * 8, gy - 12);
            p.lineTo(cx - d * 8, gy + 12);
            p.close();
            c.drawPath(p, fill);
        }
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(40);
        text.setColor(held ? 0xFFFFFFFF : Theme.TEXT);
        drawFit(c, b.label, cx, r.centerY() + 22, r.width() - 24);
        text.setTypeface(Theme.REGULAR);
        text.setTextSize(Theme.T_SMALL);
        text.setColor(held ? Theme.TEXT_DIM : Theme.TEXT_FAINT);
        drawFit(c, b.hint, cx, r.centerY() + 62, r.width() - 24);
    }
}
