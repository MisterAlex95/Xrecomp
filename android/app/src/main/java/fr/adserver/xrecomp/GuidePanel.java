package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;

/**
 * The launcher's second screen on the first start and in Help (docs/android/app-shell.md « Dual screen »): the
 * screen's drawing (Schema) large, a caption under it, and up to two touch actions (Back / Continue, previous / next
 * question) away from the bottom centre. Shown through LauncherCompanion.guide.
 */
final class GuidePanel extends PanelView {
    static final int LEFT = 1, RIGHT = 2;
    static final long DIM_MS = 30_000;
    /** The drawing is made for the main screen's sizes: drawn this much larger here. */
    static final float SCALE = 1.5f;

    private Schema.Kind kind;
    private int found;
    private String caption = "", batteryText = "";
    private Runnable left, right;

    GuidePanel(Context c) {
        super(c, DIM_MS);
        buttons.add(new Button(LEFT, Kind.TAP, 80, 760, 560, 880, null, "", null));
        Button r = new Button(RIGHT, Kind.TAP, 680, 760, 1160, 880, null, "", null);
        r.accent = true;
        buttons.add(r);
    }

    /** The drawing (null: none), its count for LOOP, the caption, and the two actions (a null label: none). */
    void set(Schema.Kind kind, int found, String caption, String leftLabel, Runnable left, String rightLabel,
             Runnable right) {
        this.kind = kind;
        this.found = found;
        this.caption = caption == null ? "" : caption;
        this.left = left;
        this.right = right;
        Button l = button(LEFT), r = button(RIGHT);
        l.enabled = leftLabel != null;
        l.label = leftLabel == null ? "" : leftLabel;
        r.enabled = rightLabel != null;
        r.label = rightLabel == null ? "" : rightLabel;
        invalidate();
    }

    Schema.Kind kindForTest() {
        return kind;
    }

    @Override
    protected void onAction(Button b) {
        Runnable a = b.id == LEFT ? left : right;
        if (a != null && b.enabled)
            a.run();
    }

    @Override
    protected boolean onTick() {
        String bat = battery(getContext());
        boolean changed = !bat.equals(batteryText);
        batteryText = bat;
        return changed;
    }

    @Override
    protected void drawPanel(Canvas c, boolean dimmed) {
        if (dimmed)
            return;
        drawBattery(c, batteryText);
        if (kind != null) {
            float w = (W - 160) / SCALE, top = 140, bottom = 600;
            c.save();
            c.translate(80, top);
            c.scale(SCALE, SCALE);
            Schema.draw(getContext(), c, new RectF(0, 0, w, (bottom - top) / SCALE), kind, -1, found);
            c.restore();
        }
        if (!caption.isEmpty()) {
            text.setTextAlign(Paint.Align.CENTER);
            text.setTypeface(Theme.REGULAR);
            text.setTextSize(Theme.T_ROW);
            text.setColor(Theme.TEXT_DIM);
            drawFit(c, caption, W / 2, 680, W - 160);
        }
        for (Button b : buttons)
            if (b.enabled)
                drawButton(c, b, false);
    }
}
