package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.View;

import java.util.function.LongSupplier;

/**
 * « Translation, live » (docs/android/app-shell.md « Translation, live »): a short animation of what Prepare does, on
 * a function of our own (the shape of nxdk test code, no game's): its x86 lines, Prepare writing each one again in
 * arm64, a call whose target only a run knows (orange, « where? »), the run finding it, and Prepare again compiling
 * that one line, then « XRecomp · Native, not emulated ». Six scenes, about 22 s; onEnd runs once at the end.
 */
final class TranslationDemo extends View {
    /** The scenes' lengths, ms. */
    static final long[] SCENES = { 3200, 4200, 3600, 4200, 4200, 3000 };
    static final String[] X86 = { "push   ebp", "mov    eax, [esp+8]", "add    eax, 0x10", "cmp    eax, ecx",
                                  "call   [eax]", "ret" };
    static final String[] ARM = { "stp    x29, x30, [sp,#-16]!", "ldr    w0, [x19, #8]", "add    w0, w0, #0x10",
                                  "cmp    w0, w1", "bl     ???", "ret" };
    static final String ARM_FOUND = "bl     fn_0001A3F0";
    /** The line whose target only a run knows. */
    static final int CALL = 4;
    /** The animation's clock (uptime ms); the JVM tests freeze it. */
    static LongSupplier clock = SystemClock::uptimeMillis;

    private static final int[][] CAPTIONS = { { R.string.DEMO_1_H, R.string.DEMO_1_T }, { R.string.DEMO_2_H, R.string.DEMO_2_T },
                                              { R.string.DEMO_3_H, R.string.DEMO_3_T }, { R.string.DEMO_4_H, R.string.DEMO_4_T },
                                              { R.string.DEMO_5_H, R.string.DEMO_5_T } };
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final android.text.TextPaint text = new android.text.TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final Runnable onEnd;
    private long start = -1;
    private boolean ended;

    TranslationDemo(Context c, Runnable onEnd) {
        super(c);
        this.onEnd = onEnd;
        stroke.setStyle(Paint.Style.STROKE);
    }

    static long total() {
        long t = 0;
        for (long s : SCENES)
            t += s;
        return t;
    }

    /** The scene at time t (ms from the start) and the time within it, as { scene, ms }. */
    static long[] sceneAt(long t) {
        for (int i = 0; i < SCENES.length; i++) {
            if (t < SCENES[i])
                return new long[] { i, t };
            t -= SCENES[i];
        }
        return new long[] { SCENES.length - 1, SCENES[SCENES.length - 1] };
    }

    /** Starts it again from the first scene. */
    void restart() {
        start = clock.getAsLong();
        ended = false;
        invalidate();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (start < 0)
            start = clock.getAsLong();
    }

    @Override
    protected void onDraw(Canvas c) {
        if (start < 0)
            start = clock.getAsLong();
        long t = clock.getAsLong() - start;
        if (t >= total() && !ended) {
            ended = true;
            if (onEnd != null)
                post(onEnd);
        }
        long[] at = sceneAt(Math.min(t, total() - 1));
        drawScene(c, (int)at[0], at[1]);
        if (!ended)
            postInvalidateOnAnimation();
    }

    private static float ramp(long ms, long from, long len) {
        return Math.max(0, Math.min(1, (ms - from) / (float)len));
    }

    /** Draws scene s at ms into it, in the view's size. */
    private void drawScene(Canvas c, int s, long ms) {
        Context ctx = getContext();
        float w = getWidth(), h = getHeight();
        if (s == SCENES.length - 1) {   // the name, fading in
            float a = ramp(ms, 0, 600);
            text.setTextAlign(Paint.Align.CENTER);
            text.setTypeface(Theme.MEDIUM);
            text.setTextSize(Theme.T_DISPLAY * 2);
            text.setColor(Theme.alpha(Theme.TEXT, a));
            c.drawText(ctx.getString(R.string.app_name), w / 2, h / 2, text);
            text.setTextSize(Theme.T_TITLE);
            text.setTypeface(Theme.REGULAR);
            text.setColor(Theme.alpha(Theme.ACCENT, a));
            c.drawText(ctx.getString(R.string.DEMO_TAGLINE), w / 2, h / 2 + 80, text);
            return;
        }
        // The caption: heading and line, fading in.
        float ca = ramp(ms, 0, 300);
        text.setTextAlign(Paint.Align.LEFT);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_TITLE * 1.15f);
        text.setColor(Theme.alpha(Theme.TEXT, ca));
        c.drawText(fit(ctx.getString(CAPTIONS[s][0]), w), 0, Theme.T_TITLE * 1.15f, text);
        text.setTypeface(Theme.REGULAR);
        text.setTextSize(Theme.T_BODY);
        text.setColor(Theme.alpha(Theme.TEXT_DIM, ca));
        android.text.StaticLayout sl = android.text.StaticLayout.Builder
                .obtain(ctx.getString(CAPTIONS[s][1]), 0, ctx.getString(CAPTIONS[s][1]).length(), text,
                        (int)(w * 0.8f)).setLineSpacing(0, 1.2f).build();
        c.save();
        c.translate(0, Theme.T_TITLE * 1.15f + 24);
        sl.draw(c);
        c.restore();

        // The two panes.
        float top = h * 0.36f, bottom = h - 16, arrowW = Math.max(64, w * 0.05f);
        float paneW = (w - arrowW) / 2;
        RectF left = new RectF(0, top, paneW, bottom), right = new RectF(paneW + arrowW, top, w, bottom);
        boolean rightOn = s >= 1;
        pane(c, left, ctx.getString(R.string.DEMO_PANE_X86), 1);
        pane(c, right, ctx.getString(R.string.DEMO_PANE_ARM), rightOn ? 1 : 0.3f);
        if (rightOn) {
            text.setTextAlign(Paint.Align.CENTER);
            text.setTypeface(Theme.MEDIUM);
            text.setTextSize(Theme.T_TITLE);
            text.setColor(Theme.ACCENT);
            c.drawText("→", paneW + arrowW / 2, (top + bottom) / 2, text);
        }
        float lineH = Math.min(56, (bottom - top - 90) / X86.length), y0 = top + 70;
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(Math.min(Math.min(30, lineH * 0.6f), paneW / 24));
        text.setTextAlign(Paint.Align.LEFT);
        for (int i = 0; i < X86.length; i++) {
            float y = y0 + i * lineH;
            // Left: shown (scene 0 types them in), dimmed once translated, the call orange while unknown.
            boolean shown = s > 0 || ms >= 300 + i * 300L;
            boolean done = s == 1 ? ms >= 250 + i * 560L + 300 : s > 1;
            boolean hot = s == 1 && ms >= 250 + i * 560L && ms < 250 + i * 560L + 300;
            boolean callOpen = i == CALL && (s == 2 || s == 3 || s == 4 && ms < 1400);
            if (hot)
                band(c, left, y, lineH, Theme.alpha(Theme.ACCENT, 0.16f));
            if (callOpen)
                band(c, left, y, lineH, Theme.alpha(Theme.WARN, 0.14f));
            if (shown) {
                text.setColor(callOpen ? Theme.WARN : done ? Theme.TEXT_FAINT : Theme.TEXT);
                c.drawText(X86[i], left.left + 28, y + lineH * 0.68f, text);
            }
            // Right: written as each left line is done; in scene 4 the others dim while the call compiles again.
            boolean written = s == 1 ? done : s > 1;
            if (!written)
                continue;
            boolean found = s == 4 && ms >= 1400;
            boolean recompiling = s == 4 && ms >= 900 && ms < 1400 && i == CALL;
            boolean others = s == 4 && i != CALL && ms < 2600;
            if (i == CALL && !found)
                band(c, right, y, lineH, Theme.alpha(Theme.WARN, 0.14f));
            if (recompiling)
                band(c, right, y, lineH, Theme.alpha(Theme.ACCENT, 0.2f));
            text.setColor(i == CALL && !found ? Theme.WARN : others ? Theme.TEXT_FAINT : Theme.ACCENT);
            c.drawText(i == CALL && found ? ARM_FOUND : ARM[i], right.left + 28, y + lineH * 0.68f, text);
            if (i == CALL && !found) {   // « where? », then the address the run found
                boolean known = s == 3 && ms >= 1800 || s == 4;
                text.setTypeface(Theme.REGULAR);
                text.setTextSize(Theme.T_SMALL);
                text.setTextAlign(Paint.Align.RIGHT);
                c.drawText(known ? "→ 0x0001A3F0" : ctx.getString(R.string.DEMO_WHERE), right.right - 20,
                           y + lineH * 0.66f, text);
                if (s == 3 && known) {
                    float p = (ms - 1800) % 1000 / 1000f;
                    stroke.setStrokeWidth(3);
                    stroke.setColor(Theme.alpha(Theme.WARN, 1 - p));
                    c.drawCircle(right.right - 8, y + lineH / 2, 8 + 18 * p, stroke);
                }
                text.setTypeface(Typeface.MONOSPACE);
                text.setTextSize(Math.min(Math.min(30, lineH * 0.6f), paneW / 24));
                text.setTextAlign(Paint.Align.LEFT);
            }
        }
        // The scan bar over the line being translated.
        if (s == 1 && ms >= 250 && ms < 250 + X86.length * 560L) {
            int i = (int)((ms - 250) / 560);
            band(c, left, y0 + i * lineH, lineH, Theme.alpha(Theme.ACCENT, 0.10f));
        }
        // The badge, top right of the panes; the run's bar, bottom left of the caption.
        String badge = null;
        int badgeColor = Theme.TEXT_DIM;
        if (s == 2)
            badge = ctx.getString(R.string.DEMO_BADGE_5_OF_6);
        else if (s == 3 && ms >= 1800) {
            badge = ctx.getString(R.string.DEMO_BADGE_FOUND);
            badgeColor = Theme.WARN;
        } else if (s == 4) {
            badge = ctx.getString(ms >= 2600 ? R.string.DEMO_BADGE_ALL : R.string.DEMO_BADGE_ONE_FILE);
            badgeColor = Theme.OK;
        }
        if (badge != null) {
            text.setTypeface(Theme.MEDIUM);
            text.setTextSize(Theme.T_SMALL);
            float bw = text.measureText(badge) + 48, bh = 52, bx = w - bw, by = top - bh - 16;
            fill.setColor(Theme.PANEL_HI);
            c.drawRoundRect(new RectF(bx, by, w, by + bh), bh / 2, bh / 2, fill);
            text.setColor(badgeColor);
            text.setTextAlign(Paint.Align.CENTER);
            c.drawText(badge, bx + bw / 2, by + bh / 2 + Theme.T_SMALL / 3f, text);
        }
        if (s == 3) {
            float p = ramp(ms, 0, 2000), bx = 0, by = top - 40, bw = w * 0.3f;
            text.setTypeface(Theme.REGULAR);
            text.setTextSize(Theme.T_SMALL);
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(Theme.TEXT_DIM);
            String play = "▶ " + ctx.getString(R.string.DEMO_PLAYING);
            c.drawText(play, bx, by + 8, text);
            float tx = bx + text.measureText(play) + 24;
            fill.setColor(Theme.TRACK);
            c.drawRoundRect(new RectF(tx, by - 4, tx + bw, by + 6), 5, 5, fill);
            fill.setColor(Theme.ACCENT);
            c.drawRoundRect(new RectF(tx, by - 4, tx + bw * p, by + 6), 5, 5, fill);
        }
    }

    private void pane(Canvas c, RectF r, String label, float alpha) {
        fill.setColor(Theme.alpha(Theme.PANEL, alpha));
        c.drawRoundRect(r, Theme.RADIUS, Theme.RADIUS, fill);
        stroke.setStrokeWidth(2);
        stroke.setColor(Theme.alpha(Theme.LINE, alpha));
        c.drawRoundRect(r, Theme.RADIUS, Theme.RADIUS, stroke);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_SMALL * 0.85f);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(Theme.alpha(Theme.TEXT_FAINT, alpha));
        c.drawText(fit(label, r.width() - 56), r.left + 28, r.top + 44, text);
    }

    private void band(Canvas c, RectF pane, float y, float h, int color) {
        fill.setColor(color);
        c.drawRoundRect(new RectF(pane.left + 12, y, pane.right - 12, y + h), 8, 8, fill);
    }

    private String fit(String s, float width) {
        if (text.measureText(s) <= width)
            return s;
        int n = text.breakText(s, true, Math.max(0, width - text.measureText("…")), null);
        return s.substring(0, Math.max(0, n)).trim() + "…";
    }
}
