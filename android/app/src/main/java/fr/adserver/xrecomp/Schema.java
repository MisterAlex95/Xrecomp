package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/**
 * The launcher's explanatory drawings (docs/android/app-shell.md « Help and questions »): how a game goes from its
 * console's code to native code, the Prepare's steps, the « prepare again » loop, Self-healing's three cases, saves kept
 * apart, and the consoles that start at once. Drawn on a Canvas in any rectangle ({@link #draw}), so Help's
 * description column ({@link SchemaView}) and the second screen's panels share them.
 */
final class Schema {
    private Schema() {}

    /** STAGES: STEPS without its bar (a panel that draws its own). */
    enum Kind { PIPE, STEPS, STAGES, LOOP, HEAL, SAVES, AT_ONCE }

    /** The drawing that goes with each Help question (LauncherActivity.HELP order). */
    static final Kind[] FOR_HELP = { Kind.PIPE, Kind.STEPS, Kind.LOOP, Kind.HEAL, Kind.SAVES, Kind.AT_ONCE };

    /**
     * Draws kind in r. progress: STEPS' bar and lit steps (0 to 1, negative: three of four lit, as an example);
     * found: LOOP's count of addresses found (0: a « ↻ » instead).
     */
    static void draw(Context c, Canvas cv, RectF r, Kind kind, float progress, int found) {
        float h = Math.min(r.height(), height(kind, r.width()));
        RectF b = new RectF(r.left, r.centerY() - h / 2, r.right, r.centerY() + h / 2);
        Pen p = new Pen(r.width());
        switch (kind) {
        case PIPE:
            pipe(c, cv, b, p, R.string.SCH_GAME, R.string.SCH_GAME_SUB, R.string.SCH_PREPARE, R.string.SCH_PREPARE_SUB,
                 R.string.SCH_NATIVE, R.string.SCH_NATIVE_SUB, b.centerY());
            break;
        case STEPS:
        case STAGES:
            steps(c, cv, b, p, progress, kind == Kind.STEPS);
            break;
        case LOOP:
            loop(c, cv, b, p, found);
            break;
        case HEAL:
            heal(c, cv, b, p);
            break;
        case SAVES:
            saves(c, cv, b, p);
            break;
        case AT_ONCE:
            float lane = BOX_H / 2 + 4;
            pipe(c, cv, b, p, R.string.SCH_FILE, R.string.SCH_FILE_SUB, R.string.SCH_INTERP, R.string.SCH_INTERP_SUB,
                 0, 0, b.top + lane);
            pipe(c, cv, b, p, R.string.SCH_FILE, R.string.SCH_FILE_SUB, R.string.SCH_PREPARE,
                 R.string.SCH_PREPARE_SUB, R.string.SCH_NATIVE, R.string.SCH_NATIVE_SUB, b.bottom - lane);
            break;
        }
    }

    /** Narrower than this, a drawing is left out (Help's column on a square screen). */
    static final int MIN_W = 480;

    /** A box's height: its title and line in the launcher's small sizes. */
    static final float BOX_H = 92;

    /** The height kind wants at width w (its text stays at the launcher's sizes; the gaps follow the width). */
    static float height(Kind kind, float w) {
        switch (kind) {
        case STEPS:
            return 200;
        case STAGES:
            return 130;
        case LOOP:
            return BOX_H + 150;
        case HEAL:
            return BOX_H + 40;
        case AT_ONCE:
            return 2 * BOX_H + 40;
        default:
            return BOX_H + 16;
        }
    }

    /** The paints at the drawing's scale (gaps in fractions of its width, text at the launcher's sizes). */
    private static final class Pen {
        final float u, title, sub, stroke, radius;
        final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                text = new Paint(Paint.ANTI_ALIAS_FLAG);

        Pen(float w) {
            u = 8;   // the stage's pixels: arrows, dashes and dots keep their size at any width
            title = Theme.T_SMALL;
            sub = 20;
            stroke = 3;
            radius = 14;
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(stroke);
            line.setStrokeCap(Paint.Cap.ROUND);
            text.setTextAlign(Paint.Align.CENTER);
        }
    }

    /** A box: its title and line below, centred; accent: the lit border. */
    private static void box(Context c, Canvas cv, Pen p, RectF r, int title, int sub, int border) {
        p.fill.setColor(Theme.PANEL_HI);
        cv.drawRoundRect(r, p.radius, p.radius, p.fill);
        p.line.setColor(border);
        p.line.setPathEffect(null);
        cv.drawRoundRect(r, p.radius, p.radius, p.line);
        float gap = p.title * 0.35f;
        boolean two = sub != 0;
        float ty = two ? r.centerY() - gap / 2 : r.centerY() + p.title / 3;
        label(cv, p, c.getString(title), r.centerX(), ty, p.title, Theme.MEDIUM, Theme.TEXT, r.width() * 0.9f);
        if (two)
            label(cv, p, c.getString(sub), r.centerX(), r.centerY() + gap / 2 + p.sub, p.sub, Theme.REGULAR,
                  Theme.TEXT_DIM, r.width() * 0.9f);
    }

    private static void label(Canvas cv, Pen p, String s, float x, float y, float size, android.graphics.Typeface face,
                              int color, float maxW) {
        p.text.setTypeface(face);
        p.text.setColor(color);
        p.text.setTextSize(size);
        while (size > 15 && p.text.measureText(s) > maxW)
            p.text.setTextSize(--size);
        String t = s;
        for (int n = s.length() - 1; n > 0 && p.text.measureText(t) > maxW; n--)
            t = s.substring(0, n).trim() + "…";
        cv.drawText(t, x, y, p.text);
    }

    /** An arrow from x1 to x2 at y, in the accent. */
    private static void arrow(Canvas cv, Pen p, float x1, float x2, float y) {
        p.line.setColor(Theme.ACCENT);
        p.line.setPathEffect(new DashPathEffect(new float[] { 1.2f * p.u, 1.4f * p.u }, 0));
        cv.drawLine(x1, y, x2 - 1.6f * p.u, y, p.line);
        p.line.setPathEffect(null);
        Path head = new Path();
        head.moveTo(x2 - 2 * p.u, y - 1.3f * p.u);
        head.lineTo(x2, y);
        head.lineTo(x2 - 2 * p.u, y + 1.3f * p.u);
        head.close();
        p.fill.setColor(Theme.ACCENT);
        cv.drawPath(head, p.fill);
    }

    /** Three boxes in a row joined by arrows (the third left out when its title is 0), centred on y. */
    private static void pipe(Context c, Canvas cv, RectF b, Pen p, int t1, int s1, int t2, int s2, int t3, int s3,
                             float y) {
        float gap = Math.max(36, b.width() * 0.06f), bw = (b.width() - 2 * gap) / 3, bh = BOX_H;
        int[][] boxes = { { t1, s1 }, { t2, s2 }, { t3, s3 } };
        for (int i = 0; i < 3; i++) {
            if (boxes[i][0] == 0)
                continue;
            float x = b.left + i * (bw + gap);
            box(c, cv, p, new RectF(x, y - bh / 2, x + bw, y + bh / 2), boxes[i][0], boxes[i][1],
                i == 1 && t2 == R.string.SCH_PREPARE ? Theme.ACCENT : Theme.LINE);
            if (i > 0)
                arrow(cv, p, x - gap + p.u, x - p.u, y);
        }
    }

    private static void steps(Context c, Canvas cv, RectF b, Pen p, float progress, boolean bar) {
        int[] names = { R.string.SCH_READ, R.string.SCH_TRANSLATE, R.string.SCH_COMPILE, R.string.SCH_PACK };
        float frac = progress < 0 ? 0.62f : Math.min(1, progress);
        int lit = progress < 0 ? 2 : Math.min(4, (int)Math.floor(frac * 4 + 1e-4));
        float rad = 26, cy = b.top + 40, step = b.width() / 4;
        for (int i = 0; i < 4; i++) {
            float cx = b.left + step * (i + 0.5f);
            boolean done = i < lit, now = i == lit;
            if (i < 3) {
                p.line.setColor(done ? Theme.ACCENT : Theme.LINE);
                cv.drawLine(cx + rad, cy, cx + step - rad, cy, p.line);
            }
            p.fill.setColor(done ? Theme.ACCENT : Theme.PANEL_HI);
            cv.drawCircle(cx, cy, rad, p.fill);
            p.line.setColor(done || now ? Theme.ACCENT : Theme.LINE);
            cv.drawCircle(cx, cy, rad, p.line);
            label(cv, p, done ? "✓" : String.valueOf(i + 1), cx, cy + p.title / 3, p.title, Theme.MEDIUM,
                  done ? Theme.BG_BOTTOM : now ? Theme.ACCENT : Theme.TEXT_FAINT, rad * 2);
            label(cv, p, c.getString(names[i]), cx, cy + rad + 1.4f * p.title, p.sub, now ? Theme.MEDIUM : Theme.REGULAR,
                  now ? Theme.TEXT : Theme.TEXT_DIM, step * 0.95f);
        }
        if (!bar)
            return;
        float by = b.bottom - 14, bh = 10;
        RectF track = new RectF(b.left + step / 2, by - bh / 2, b.right - step / 2, by + bh / 2);
        p.fill.setColor(Theme.TRACK);
        cv.drawRoundRect(track, bh / 2, bh / 2, p.fill);
        p.fill.setColor(Theme.ACCENT);
        cv.drawRoundRect(new RectF(track.left, track.top, track.left + track.width() * frac, track.bottom), bh / 2,
                         bh / 2, p.fill);
    }

    private static void loop(Context c, Canvas cv, RectF b, Pen p, int found) {
        float gap = Math.max(36, b.width() * 0.06f), bw = (b.width() - 2 * gap) / 3, bh = BOX_H;
        float y = b.top + bh / 2 + 4;
        int[][] boxes = { { R.string.SCH_PLAY, R.string.SCH_PLAY_SUB }, { R.string.SCH_MISSED, R.string.SCH_MISSED_SUB },
                          { R.string.SCH_AGAIN, R.string.SCH_AGAIN_SUB } };
        for (int i = 0; i < 3; i++) {
            float x = b.left + i * (bw + gap);
            box(c, cv, p, new RectF(x, y - bh / 2, x + bw, y + bh / 2), boxes[i][0], boxes[i][1],
                i == 1 ? Theme.WARN : i == 2 ? Theme.ACCENT : Theme.LINE);
            if (i > 0)
                arrow(cv, p, x - gap + p.u, x - p.u, y);
        }
        // Back to Play: from under the last box round to under the first.
        float x1 = b.left + bw / 2, x3 = b.right - bw / 2, top = y + bh / 2 + 6, low = b.bottom - 40;
        Path back = new Path();
        back.moveTo(x3, top);
        back.cubicTo(x3, low, x3, low, b.centerX(), low);
        back.cubicTo(x1, low, x1, low, x1, top + 1.6f * p.u);
        p.line.setColor(Theme.ACCENT);
        p.line.setPathEffect(new DashPathEffect(new float[] { 1.2f * p.u, 1.4f * p.u }, 0));
        cv.drawPath(back, p.line);
        p.line.setPathEffect(null);
        Path head = new Path();
        head.moveTo(x1 - 1.3f * p.u, top + 2 * p.u);
        head.lineTo(x1, top);
        head.lineTo(x1 + 1.3f * p.u, top + 2 * p.u);
        head.close();
        p.fill.setColor(Theme.ACCENT);
        cv.drawPath(head, p.fill);
        String mid = found > 0 ? "+" + found : "↻";
        label(cv, p, mid, b.centerX(), low - 1.4f * p.u, p.title * 1.2f, Theme.MEDIUM,
              found > 0 ? Theme.WARN : Theme.ACCENT, b.width() * 0.3f);
        if (found > 0)
            label(cv, p, c.getString(R.string.SCH_FOUND), b.centerX(), low + p.sub * 1.1f, p.sub, Theme.REGULAR,
                  Theme.TEXT_DIM, b.width() * 0.4f);
    }

    private static void heal(Context c, Canvas cv, RectF b, Pen p) {
        int[][] cases = { { R.string.SCH_CRASH, R.string.SCH_CRASH_SUB }, { R.string.SCH_GOOD, R.string.SCH_GOOD_SUB },
                          { R.string.SCH_NOTHING, R.string.SCH_NOTHING_SUB } };
        int[] colors = { Theme.ERROR, Theme.OK, Theme.WARN };
        float gap = Math.max(16, b.width() * 0.03f), bw = (b.width() - 2 * gap) / 3, bh = BOX_H + 20;
        for (int i = 0; i < 3; i++) {
            float x = b.left + i * (bw + gap);
            RectF r = new RectF(x, b.centerY() - bh / 2, x + bw, b.centerY() + bh / 2);
            box(c, cv, p, r, cases[i][0], cases[i][1], colors[i]);
            p.fill.setColor(colors[i]);
            cv.drawCircle(r.left + 3 * p.u, r.top + 3 * p.u, 1.1f * p.u, p.fill);
        }
    }

    private static void saves(Context c, Canvas cv, RectF b, Pen p) {
        float bw = b.width() * 0.4f, bh = BOX_H, y = b.centerY();
        box(c, cv, p, new RectF(b.left + b.width() * 0.04f, y - bh / 2, b.left + b.width() * 0.04f + bw, y + bh / 2),
            R.string.SCH_SAVES, R.string.SCH_SAVES_SUB, Theme.ACCENT);
        box(c, cv, p, new RectF(b.right - b.width() * 0.04f - bw, y - bh / 2, b.right - b.width() * 0.04f, y + bh / 2),
            R.string.SCH_CODE, R.string.SCH_CODE_SUB, Theme.LINE);
        p.line.setColor(Theme.LINE);
        p.line.setPathEffect(new DashPathEffect(new float[] { 1.2f * p.u, 1.4f * p.u }, 0));
        cv.drawLine(b.centerX(), b.top + 2 * p.u, b.centerX(), b.bottom - 2 * p.u, p.line);
        p.line.setPathEffect(null);
    }

    /** A drawing as a view: Help's description column, as tall as the drawing wants (height). */
    static final class SchemaView extends View {
        final Kind kind;
        private final RectF r = new RectF();

        SchemaView(Context c, Kind kind) {
            super(c);
            this.kind = kind;
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec);   // too narrow for three boxes (a square screen): the text alone
            setMeasuredDimension(w, w < MIN_W ? 0 : Math.round(height(kind, w)));
        }

        @Override
        protected void onDraw(Canvas cv) {
            r.set(getPaddingLeft(), getPaddingTop(), getWidth() - getPaddingRight(), getHeight() - getPaddingBottom());
            Schema.draw(getContext(), cv, r, kind, -1, 0);
        }
    }

    /**
     * A game Home's track (docs « A game's Home »): Read › Translate › Compile › Play, lit as far as the game got, then
     * one dot per address its runs found that the preparation missed (twelve at most, then « +N »).
     */
    static final class TrackView extends View {
        static final int H = 52;
        private static final int[] STAGES = { R.string.SCH_READ, R.string.SCH_TRANSLATE, R.string.SCH_COMPILE,
                                              R.string.SCH_PLAY };
        final int lit, found;
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);

        TrackView(Context c, int lit, int found) {
            super(c);
            this.lit = Math.max(0, Math.min(4, lit));
            this.found = Math.max(0, found);
            text.setTextSize(Theme.T_SMALL);
            text.setTypeface(Theme.MEDIUM);
        }

        /** How many stages a Prepare at overall fraction frac has done (an estimate from its progress). */
        static int litFor(float frac) {
            return frac < 0.15f ? 0 : frac < 0.45f ? 1 : frac < 0.97f ? 2 : 3;
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            setMeasuredDimension(MeasureSpec.getSize(wSpec), H);
        }

        @Override
        protected void onDraw(Canvas cv) {
            float x = 0, h = getHeight(), pad = 18, gap = 10, sep = 22;
            for (int i = 0; i < STAGES.length; i++) {
                boolean on = i < lit;
                String s = (on ? "✓ " : "") + getContext().getString(STAGES[i]);
                float w = text.measureText(s) + 2 * pad;
                fill.setColor(on ? Theme.alpha(Theme.ACCENT, 0.16f) : Theme.PANEL);
                cv.drawRoundRect(new RectF(x, 4, x + w, h - 4), h / 2, h / 2, fill);
                text.setColor(on ? Theme.ACCENT : Theme.TEXT_FAINT);
                cv.drawText(s, x + pad, h / 2 + Theme.T_SMALL / 3f, text);
                x += w;
                if (i < STAGES.length - 1) {
                    fill.setColor(Theme.LINE);
                    cv.drawRect(x + gap / 2, h / 2 - 1, x + sep - gap / 2, h / 2 + 1, fill);
                    x += sep;
                }
            }
            if (found == 0)
                return;
            x += 24;
            fill.setColor(Theme.WARN);
            int dots = Math.min(12, found);
            for (int i = 0; i < dots && x < getWidth() - 80; i++, x += 22)
                cv.drawCircle(x + 7, h / 2, 7, fill);
            if (found > dots) {
                text.setColor(Theme.WARN);
                cv.drawText("+" + (found - dots), x + 4, h / 2 + Theme.T_SMALL / 3f, text);
            }
        }
    }
}
