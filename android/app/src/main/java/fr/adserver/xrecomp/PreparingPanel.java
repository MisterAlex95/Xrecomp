package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;

/**
 * The launcher's second-screen panel while a Prepare runs (docs/android/app-shell.md « Dual screen »): pure black,
 * the overall percentage big, one bar, « Compile · about 12 min left », the game's title, and a hold-to-cancel pill;
 * the battery small in a corner. It reads status.json itself once a second ({@link Source}). After 30 s without a
 * touch it dims to the percentage alone (a Prepare takes 20 to 60 minutes: burn-in).
 */
final class PreparingPanel extends PanelView {
    /** Where the Prepare's state comes from: the files in the launcher, fixed values in the tests. */
    interface Source {
        PrepareProtocol.Status status();
    }

    static final int CANCEL = 1;
    static final long DIM_MS = 30_000;
    /** The stages' drawing is made for the main screen's sizes: larger here. */
    private static final float STAGES_SCALE = 1.3f;
    /** The bar: 800 x 12, centred. */
    private static final RectF BAR = new RectF(220, 440, 1020, 452);

    private final Source source;
    private final Runnable cancel;
    private final String title;
    private PrepareProtocol.Status status;
    private String batteryText = "", lastJson = "";
    private boolean cancelSent;

    PreparingPanel(Context c, String title, Source source, Runnable cancel) {
        super(c, DIM_MS);
        this.title = title == null ? "" : title;
        this.source = source;
        this.cancel = cancel;
        buttons.add(new Button(CANCEL, Kind.HOLD, 420, 680, 820, 800, null, c.getString(R.string.PANEL_HOLD_CANCEL),
                               null));
    }

    /** The Prepare's status file of a title. */
    static Source files(Context c, String titleId) {
        return () -> titleId == null ? null : PrepareProtocol.readStatus(PrepareService.statusFile(c, titleId));
    }

    boolean running() {
        return status == null || status.running();
    }

    @Override
    protected void onAction(Button b) {
        if (b.id != CANCEL || cancelSent || !running())
            return;
        cancelSent = true;
        b.enabled = false;
        b.label = getContext().getString(R.string.CANCELING);
        invalidate();
        if (cancel != null)
            cancel.run();
    }

    boolean cancelSentForTest() {
        return cancelSent;
    }

    @Override
    protected boolean onTick() {
        PrepareProtocol.Status s = source == null ? null : source.status();
        String json = s == null ? "" : s.toJson().toString();
        String bat = battery(getContext());
        boolean changed = !json.equals(lastJson) || !bat.equals(batteryText);
        status = s;
        lastJson = json;
        batteryText = bat;
        Button b = button(CANCEL);
        boolean show = running() && !cancelSent;
        if (b.enabled != show && !cancelSent) {
            b.enabled = show;
            changed = true;
        }
        return changed;
    }

    private float fraction() {
        if (status == null)
            return 0;
        if (PrepareProtocol.DONE.equals(status.state))
            return 1;
        return PrepareProtocol.overall(status.step, status.n, status.pct);
    }

    @Override
    protected void drawPanel(Canvas c, boolean dimmed) {
        Context ctx = getContext();
        float frac = fraction();
        String pct = Math.round(frac * 100) + " %";
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(150);
        if (dimmed) {
            // The percentage alone, faint, where it always is.
            text.setColor(Theme.alpha(Theme.TEXT_FAINT, 0.55f));
            c.drawText(pct, W / 2, 380, text);
            return;
        }
        drawBattery(c, batteryText);
        boolean done = status != null && PrepareProtocol.DONE.equals(status.state);
        boolean failed = status != null && !status.running() && !done;
        if (!failed) {   // the stages above, as Help draws them (Read, Translate, Compile, Pack), lit by the progress
            c.save();
            c.translate(BAR.left, 70);
            c.scale(STAGES_SCALE, STAGES_SCALE);
            Schema.draw(getContext(), c, new RectF(0, 0, BAR.width() / STAGES_SCALE, 130), Schema.Kind.STAGES,
                        frac, 0);
            c.restore();
        }
        text.setColor(failed ? Theme.ERROR : Theme.TEXT);
        c.drawText(pct, W / 2, 380, text);

        fill.setColor(Theme.LINE);
        c.drawRoundRect(BAR, BAR.height() / 2, BAR.height() / 2, fill);
        if (frac > 0) {
            fill.setColor(failed ? Theme.ERROR : Theme.ACCENT);
            c.drawRoundRect(BAR.left, BAR.top, BAR.left + Math.max(BAR.height(), BAR.width() * frac), BAR.bottom,
                            BAR.height() / 2, BAR.height() / 2, fill);
        }

        String line;
        if (done)
            line = ctx.getString(R.string.PANEL_PREPARE_DONE);
        else if (failed)
            line = ctx.getString(R.string.CHIP_PREPARE_FAILED);
        else if (status == null)
            line = ctx.getString(R.string.PREPARE_STARTING);
        else {
            line = PrepareService.stepName(ctx, status.id);
            if (status.eta >= 0)
                line += " · " + ctx.getString(R.string.PREPARE_ETA, PrepareProtocol.formatEta(status.eta));
        }
        text.setTypeface(Theme.REGULAR);
        text.setTextSize(Theme.T_ROW);
        text.setColor(Theme.TEXT_DIM);
        drawFit(c, line, W / 2, 522, W - 160);
        text.setTextSize(Theme.T_BODY);
        drawFit(c, title, W / 2, 568, W - 160);

        if (running() || cancelSent)
            drawButton(c, button(CANCEL), false);
    }
}
