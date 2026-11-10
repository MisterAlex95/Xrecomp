package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The launcher's second-screen panel while a Prepare runs (docs/android/app-shell.md « Dual screen »): the step list,
 * the overall percentage and time left, the last lines of prepare.log, battery and thermal status, and a hold-to-cancel
 * button. It reads status.json and the log itself once a second ({@link Source}). After 30 s without a touch it dims to
 * the percentage alone (a Prepare takes 20 to 60 minutes: burn-in).
 */
final class PreparingPanel extends PanelView {
    /** Where the Prepare's state comes from: the files in the launcher, fixed values in the tests. */
    interface Source {
        PrepareProtocol.Status status();

        List<String> logTail(int lines);
    }

    static final int CANCEL = 1;
    static final long DIM_MS = 30_000;
    static final int LOG_LINES = 4;

    private static final String[] IDS = { PrepareProtocol.STEP_INSTALL, "identify", "analyse", "lift", "compile",
                                          "pack" };
    private static final int[] NAMES = { R.string.STEP_INSTALL, R.string.STEP_IDENTIFY, R.string.STEP_ANALYSE,
                                         R.string.STEP_LIFT, R.string.STEP_COMPILE, R.string.STEP_PACK };

    private final Source source;
    private final Runnable cancel;
    private final String title;
    private PrepareProtocol.Status status;
    private List<String> log = Collections.emptyList();
    private String batteryText = "", lastJson = "";
    private int thermal = -1;
    private boolean cancelSent;

    PreparingPanel(Context c, String title, Source source, Runnable cancel) {
        super(c, DIM_MS);
        this.title = title == null ? "" : title;
        this.source = source;
        this.cancel = cancel;
        buttons.add(new Button(CANCEL, Kind.HOLD, 760, 470, 1204, 680, PanelIcons.Icon.STOP,
                               c.getString(R.string.CANCEL), c.getString(R.string.PANEL_HOLD_CANCEL)));
    }

    /** The Prepare's files of a title: status.json and the tail of prepare.log. */
    static Source files(Context c, String titleId) {
        return new Source() {
            @Override
            public PrepareProtocol.Status status() {
                return titleId == null ? null : PrepareProtocol.readStatus(PrepareService.statusFile(c, titleId));
            }

            @Override
            public List<String> logTail(int lines) {
                return titleId == null ? Collections.emptyList() : tail(PrepareService.logFile(c, titleId), lines);
            }
        };
    }

    /** The last n non-empty lines of a text file (its last 8 KB). */
    static List<String> tail(File f, int n) {
        List<String> out = new ArrayList<>();
        if (f == null || !f.isFile())
            return out;
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long len = r.length(), from = Math.max(0, len - 8192);
            byte[] b = new byte[(int)(len - from)];
            r.seek(from);
            r.readFully(b);
            String[] lines = new String(b, StandardCharsets.UTF_8).split("\r?\n");
            for (int i = lines.length - 1; i >= 0 && out.size() < n; i--) {
                if (i == 0 && from > 0)
                    break;   // cut in the middle
                String s = lines[i].trim();
                if (!s.isEmpty())
                    out.add(0, s);
            }
        } catch (IOException ignored) {
        }
        return out;
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
        b.hint = "";
        invalidate();
        if (cancel != null)
            cancel.run();
    }

    boolean cancelSentForTest() {
        return cancelSent;
    }

    @Override
    protected boolean onTick() {
        Context c = getContext();
        PrepareProtocol.Status s = source == null ? null : source.status();
        List<String> l = source == null ? Collections.<String>emptyList() : source.logTail(LOG_LINES);
        String json = s == null ? "" : s.toJson().toString();
        String bat = battery(c);
        int th = thermal(c);
        boolean changed = !json.equals(lastJson) || !l.equals(log) || !bat.equals(batteryText) || th != thermal;
        status = s;
        lastJson = json;
        log = l;
        batteryText = bat;
        thermal = th;
        Button b = button(CANCEL);
        boolean show = running() && !cancelSent;
        if (b.enabled != show && !cancelSent) {
            b.enabled = show;
            changed = true;
        }
        return changed;
    }

    private int percent() {
        if (status == null)
            return 0;
        if (PrepareProtocol.DONE.equals(status.state))
            return 100;
        return Math.round(PrepareProtocol.overall(status.step, status.n, status.pct) * 100);
    }

    @Override
    protected void drawPanel(Canvas c, boolean dimmed) {
        Context ctx = getContext();
        String pct = percent() + " %";
        if (dimmed) {
            // The percentage alone, faint, where it always is.
            text.setTextAlign(Paint.Align.CENTER);
            text.setTypeface(Theme.LIGHT);
            text.setTextSize(150);
            text.setColor(Theme.alpha(Theme.TEXT_FAINT, 0.55f));
            c.drawText(pct, W / 2, H / 2 - 40, text);
            return;
        }
        drawStrip(c, ctx.getString(R.string.PANEL_PREPARING_CAPS), batteryText, thermal);
        stroke.setStrokeWidth(2);
        stroke.setColor(Theme.LINE);
        c.drawLine(36, 122, W - 36, 122, stroke);

        // Title, then the steps on the left.
        text.setTextAlign(Paint.Align.LEFT);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_TITLE);
        text.setColor(Theme.TEXT);
        drawFit(c, title, 44, 196, 680);
        int cur = status == null ? 0 : Math.max(0, Math.min(IDS.length - 1, status.step));
        boolean done = status != null && PrepareProtocol.DONE.equals(status.state);
        boolean failed = status != null && !status.running() && !done;
        for (int i = 0; i < IDS.length; i++) {
            float y = 268 + i * 66;
            boolean past = done || i < cur, now = !done && i == cur;
            int col = past ? Theme.ACCENT : now ? (failed ? Theme.ERROR : Theme.TEXT) : Theme.TEXT_FAINT;
            if (past) {
                stroke.setColor(Theme.ACCENT);
                stroke.setStrokeWidth(5);
                c.drawLine(52, y - 12, 62, y - 2, stroke);
                c.drawLine(62, y - 2, 80, y - 24, stroke);
            } else if (now) {
                fill.setColor(col);
                c.drawCircle(66, y - 12, 11, fill);
            } else {
                stroke.setColor(Theme.TEXT_FAINT);
                stroke.setStrokeWidth(3);
                c.drawCircle(66, y - 12, 10, stroke);
            }
            text.setTypeface(now ? Theme.MEDIUM : Theme.REGULAR);
            text.setTextSize(Theme.T_BODY + 3);
            text.setColor(col);
            c.drawText(ctx.getString(NAMES[i]), 106, y, text);
            if (now && status != null && status.running() && i > 0) {
                text.setTextAlign(Paint.Align.RIGHT);
                text.setColor(Theme.TEXT_DIM);
                c.drawText(status.pct + " %", 700, y, text);
                text.setTextAlign(Paint.Align.LEFT);
            }
        }

        // The overall percentage and time left on the right.
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.LIGHT);
        text.setTextSize(140);
        text.setColor(failed ? Theme.ERROR : Theme.TEXT);
        c.drawText(pct, 982, 300, text);
        text.setTypeface(Theme.REGULAR);
        text.setTextSize(Theme.T_BODY);
        text.setColor(Theme.TEXT_DIM);
        String sub;
        if (done)
            sub = ctx.getString(R.string.PANEL_PREPARE_DONE);
        else if (failed)
            sub = ctx.getString(R.string.CHIP_PREPARE_FAILED);
        else if (status != null && status.eta >= 0)
            sub = ctx.getString(R.string.PANEL_TIME_LEFT, PrepareProtocol.formatEta(status.eta));
        else
            sub = "";
        c.drawText(sub, 982, 372, text);
        if (status != null && status.step > 0 && !done) {
            text.setColor(Theme.TEXT_FAINT);
            c.drawText(ctx.getString(R.string.PREPARE_STEP_OF, status.step, status.n), 982, 418, text);
        }

        if (running() || cancelSent)
            drawButton(c, button(CANCEL), false);

        // The log's last lines, under it all (text only: nothing to touch down there).
        text.setTextAlign(Paint.Align.LEFT);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_CAPS);
        text.setLetterSpacing(0.22f);
        text.setColor(Theme.TEXT_FAINT);
        c.drawText(ctx.getString(R.string.PANEL_LOG_CAPS).toUpperCase(java.util.Locale.ROOT), 44, 740, text);
        text.setLetterSpacing(0);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(24);
        text.setColor(Theme.TEXT_DIM);
        for (int i = 0; i < log.size(); i++) {
            drawFit(c, log.get(i), 44, 784 + i * 38, W - 88);
        }
    }
}
