package fr.adserver.xboxrecomp;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.os.SystemClock;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;

/**
 * The launcher's custom views and drawables (XBA-41), drawn on Canvas in design pixels (Theme). Framework classes
 * only: no AndroidX, no images, no font files.
 */
final class Widgets {
    private Widgets() {}

    private static Paint fill(int color) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.FILL);
        p.setColor(color);
        return p;
    }

    private static Paint stroke(int color, float width) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(width);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        p.setColor(color);
        return p;
    }

    private static TextPaint textPaint(float size, android.graphics.Typeface face, int color) {
        TextPaint p = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        p.setTextSize(size);
        p.setTypeface(face);
        p.setColor(color);
        return p;
    }

    private static int blend(int a, int b, float t) {
        t = Math.max(0, Math.min(1, t));
        int aa = a >>> 24, ar = a >> 16 & 0xff, ag = a >> 8 & 0xff, ab = a & 0xff;
        int ba = b >>> 24, br = b >> 16 & 0xff, bg = b >> 8 & 0xff, bb = b & 0xff;
        return Math.round(aa + (ba - aa) * t) << 24 | Math.round(ar + (br - ar) * t) << 16
               | Math.round(ag + (bg - ag) * t) << 8 | Math.round(ab + (bb - ab) * t);
    }

    // ---- Stage: the 1920 x 1080 design grid, scaled to the screen ----------------------------------------------

    /**
     * Holds the launcher's content at design size (at least 1920 x 1080, wider or taller to match the screen's
     * shape) and scales it to fill the window less the display cutout. Touch and focus go through the transform.
     */
    static final class Stage extends ViewGroup {
        private final View content;
        private Insets cut = Insets.NONE;
        private float scale = 1;
        private int dw = Theme.DESIGN_W, dh = Theme.DESIGN_H;

        Stage(Context c, View content) {
            super(c);
            this.content = content;
            addView(content);
            setOnApplyWindowInsetsListener((v, in) -> {
                Insets i = in.getInsets(WindowInsets.Type.displayCutout());
                if (!i.equals(cut)) {
                    cut = i;
                    requestLayout();
                }
                return WindowInsets.CONSUMED;
            });
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            int aw = Math.max(1, w - cut.left - cut.right), ah = Math.max(1, h - cut.top - cut.bottom);
            scale = Math.min(aw / (float)Theme.DESIGN_W, ah / (float)Theme.DESIGN_H);
            dw = Math.round(aw / scale);
            dh = Math.round(ah / scale);
            content.measure(MeasureSpec.makeMeasureSpec(dw, MeasureSpec.EXACTLY),
                            MeasureSpec.makeMeasureSpec(dh, MeasureSpec.EXACTLY));
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            content.layout(0, 0, dw, dh);
            content.setPivotX(0);
            content.setPivotY(0);
            content.setScaleX(scale);
            content.setScaleY(scale);
            content.setTranslationX(cut.left);
            content.setTranslationY(cut.top);
        }
    }

    // ---- Backgrounds ---------------------------------------------------------------------------------------------

    private abstract static class SimpleDrawable extends Drawable {
        @Override
        public void setAlpha(int alpha) {}

        @Override
        public void setColorFilter(ColorFilter cf) {}

        /** Abstract in Drawable, so it has to be there, though the framework no longer reads it. */
        @Override
        @Deprecated
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /** The screen behind everything: a dark gradient, a soft green glow, a faint dot grid and a thin bezel. */
    static final class Backdrop extends SimpleDrawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint dot = fill(Theme.alpha(Theme.TEXT, 0.045f));
        private final Paint bezel = stroke(Theme.alpha(Theme.LINE, 0.9f), 2);
        private final RectF r = new RectF();

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            float w = b.width(), h = b.height();
            paint.setShader(new LinearGradient(0, 0, 0, h, Theme.BG_TOP, Theme.BG_BOTTOM, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(new RadialGradient(w * 0.86f, h * 0.08f, w * 0.55f, Theme.alpha(Theme.ACCENT, 0.11f),
                                               0, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(new RadialGradient(w * 0.06f, h * 1.02f, w * 0.42f, Theme.alpha(Theme.ACCENT, 0.05f),
                                               0, Shader.TileMode.CLAMP));
            c.drawRect(b, paint);
            paint.setShader(null);
            float step = 48;
            for (float y = step; y < h; y += step) {
                for (float x = step; x < w; x += step)
                    c.drawCircle(x, y, 1.5f, dot);
            }
            r.set(18, 18, w - 18, h - 18);
            c.drawRoundRect(r, 34, 34, bezel);
        }
    }

    /** A panel: dark glass, a thin outline, and two green corner ticks. */
    static final class Panel extends SimpleDrawable {
        private final Paint bg = fill(Theme.PANEL);
        private final Paint line = stroke(Theme.LINE, 1.5f);
        private final Paint tick = stroke(Theme.alpha(Theme.ACCENT, 0.7f), 3);
        private final RectF r = new RectF();

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            r.set(b.left + 1, b.top + 1, b.right - 1, b.bottom - 1);
            c.drawRoundRect(r, Theme.RADIUS, Theme.RADIUS, bg);
            c.drawRoundRect(r, Theme.RADIUS, Theme.RADIUS, line);
            float k = 26, i = 12;
            c.drawLine(b.left + i, b.top + i, b.left + i + k, b.top + i, tick);
            c.drawLine(b.left + i, b.top + i, b.left + i, b.top + i + k, tick);
            c.drawLine(b.right - i, b.bottom - i, b.right - i - k, b.bottom - i, tick);
            c.drawLine(b.right - i, b.bottom - i, b.right - i, b.bottom - i - k, tick);
        }
    }

    /** A thin horizontal rule. */
    static final class Rule extends View {
        private final Paint p = fill(Theme.LINE);

        Rule(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas c) {
            p.setShader(new LinearGradient(0, 0, getWidth(), 0, new int[] { Theme.alpha(Theme.LINE, 0f), Theme.LINE,
                                           Theme.LINE, Theme.alpha(Theme.LINE, 0f) },
                                           new float[] { 0, 0.08f, 0.92f, 1 }, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, getWidth(), getHeight(), p);
        }
    }

    // ---- Icons -----------------------------------------------------------------------------------------------------

    enum Icon { PLAY, SLIDERS, DISC, INFO, FOLDER, KEY, COPY, SWAP, TRASH, ARROW, CHECK, BACK, RESET }

    /** Draws an icon in a box of side s centred on (cx, cy), with the stroke paint p (its colour is kept). */
    static void drawIcon(Canvas c, Icon icon, float cx, float cy, float s, Paint p) {
        float h = s / 2;
        Path path = new Path();
        Paint f = new Paint(p);
        f.setStyle(Paint.Style.FILL);
        switch (icon) {
        case PLAY:
            path.moveTo(cx - h * 0.45f, cy - h * 0.62f);
            path.lineTo(cx + h * 0.62f, cy);
            path.lineTo(cx - h * 0.45f, cy + h * 0.62f);
            path.close();
            c.drawPath(path, f);
            break;
        case SLIDERS:
            for (int i = -1; i <= 1; i++) {
                float y = cy + i * h * 0.6f, kx = cx + (i == 0 ? h * 0.35f : -h * 0.3f);
                c.drawLine(cx - h * 0.8f, y, cx + h * 0.8f, y, p);
                c.drawCircle(kx, y, h * 0.2f, f);
            }
            break;
        case DISC:
            c.drawCircle(cx, cy, h * 0.82f, p);
            c.drawCircle(cx, cy, h * 0.22f, p);
            c.drawArc(cx - h * 0.55f, cy - h * 0.55f, cx + h * 0.55f, cy + h * 0.55f, 200, 60, false, p);
            break;
        case INFO:
            c.drawCircle(cx, cy, h * 0.82f, p);
            c.drawLine(cx, cy - h * 0.05f, cx, cy + h * 0.42f, p);
            c.drawCircle(cx, cy - h * 0.36f, p.getStrokeWidth() * 0.75f, f);
            break;
        case FOLDER:
            path.moveTo(cx - h * 0.85f, cy + h * 0.6f);
            path.lineTo(cx - h * 0.85f, cy - h * 0.6f);
            path.lineTo(cx - h * 0.25f, cy - h * 0.6f);
            path.lineTo(cx - h * 0.05f, cy - h * 0.38f);
            path.lineTo(cx + h * 0.85f, cy - h * 0.38f);
            path.lineTo(cx + h * 0.85f, cy + h * 0.6f);
            path.close();
            c.drawPath(path, p);
            break;
        case KEY:
            c.drawCircle(cx - h * 0.4f, cy, h * 0.36f, p);
            c.drawLine(cx - h * 0.04f, cy, cx + h * 0.85f, cy, p);
            c.drawLine(cx + h * 0.55f, cy, cx + h * 0.55f, cy + h * 0.32f, p);
            c.drawLine(cx + h * 0.82f, cy, cx + h * 0.82f, cy + h * 0.26f, p);
            break;
        case COPY:
            c.drawRoundRect(cx - h * 0.75f, cy - h * 0.45f, cx + h * 0.35f, cy + h * 0.8f, h * 0.12f, h * 0.12f, p);
            path.moveTo(cx - h * 0.35f, cy - h * 0.65f);
            path.lineTo(cx - h * 0.35f, cy - h * 0.8f);
            path.lineTo(cx + h * 0.75f, cy - h * 0.8f);
            path.lineTo(cx + h * 0.75f, cy + h * 0.4f);
            path.lineTo(cx + h * 0.55f, cy + h * 0.4f);
            c.drawPath(path, p);
            break;
        case SWAP:
            c.drawLine(cx - h * 0.75f, cy - h * 0.3f, cx + h * 0.75f, cy - h * 0.3f, p);
            c.drawLine(cx + h * 0.45f, cy - h * 0.6f, cx + h * 0.75f, cy - h * 0.3f, p);
            c.drawLine(cx + h * 0.75f, cy + h * 0.3f, cx - h * 0.75f, cy + h * 0.3f, p);
            c.drawLine(cx - h * 0.45f, cy + h * 0.6f, cx - h * 0.75f, cy + h * 0.3f, p);
            break;
        case TRASH:
            c.drawLine(cx - h * 0.8f, cy - h * 0.5f, cx + h * 0.8f, cy - h * 0.5f, p);
            c.drawLine(cx - h * 0.25f, cy - h * 0.75f, cx + h * 0.25f, cy - h * 0.75f, p);
            path.moveTo(cx - h * 0.6f, cy - h * 0.5f);
            path.lineTo(cx - h * 0.48f, cy + h * 0.8f);
            path.lineTo(cx + h * 0.48f, cy + h * 0.8f);
            path.lineTo(cx + h * 0.6f, cy - h * 0.5f);
            c.drawPath(path, p);
            break;
        case ARROW:
            c.drawLine(cx - h * 0.7f, cy, cx + h * 0.7f, cy, p);
            c.drawLine(cx + h * 0.25f, cy - h * 0.45f, cx + h * 0.7f, cy, p);
            c.drawLine(cx + h * 0.25f, cy + h * 0.45f, cx + h * 0.7f, cy, p);
            break;
        case CHECK:
            c.drawLine(cx - h * 0.6f, cy, cx - h * 0.15f, cy + h * 0.45f, p);
            c.drawLine(cx - h * 0.15f, cy + h * 0.45f, cx + h * 0.65f, cy - h * 0.45f, p);
            break;
        case BACK:
            c.drawLine(cx + h * 0.7f, cy, cx - h * 0.7f, cy, p);
            c.drawLine(cx - h * 0.25f, cy - h * 0.45f, cx - h * 0.7f, cy, p);
            c.drawLine(cx - h * 0.25f, cy + h * 0.45f, cx - h * 0.7f, cy, p);
            break;
        case RESET:
            c.drawArc(cx - h * 0.7f, cy - h * 0.7f, cx + h * 0.7f, cy + h * 0.7f, -60, 300, false, p);
            c.drawLine(cx + h * 0.35f, cy - h * 0.61f, cx + h * 0.35f, cy - h * 0.2f, p);
            c.drawLine(cx + h * 0.35f, cy - h * 0.61f, cx - h * 0.05f, cy - h * 0.61f, p);
            break;
        }
    }

    /** The app's mark: a rounded square around a glowing diamond. Generic, drawn in code. */
    static final class BrandMark extends View {
        private final Paint frame = stroke(Theme.ACCENT, 3.5f);
        private final Paint gem = fill(Theme.ACCENT);
        private final RectF r = new RectF();
        private final Path p = new Path();

        BrandMark(Context c) {
            super(c);
            gem.setShadowLayer(14, 0, 0, Theme.alpha(Theme.ACCENT, 0.7f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float s = Math.min(getWidth(), getHeight()), cx = getWidth() / 2f, cy = getHeight() / 2f;
            r.set(cx - s * 0.42f, cy - s * 0.42f, cx + s * 0.42f, cy + s * 0.42f);
            c.drawRoundRect(r, s * 0.16f, s * 0.16f, frame);
            float d = s * 0.2f;
            p.reset();
            p.moveTo(cx, cy - d);
            p.lineTo(cx + d, cy);
            p.lineTo(cx, cy + d);
            p.lineTo(cx - d, cy);
            p.close();
            c.drawPath(p, gem);
        }
    }

    // ---- MenuRow: the one selectable element (menu items, setting rows, big cards, buttons) -------------------------

    interface Cycler {
        void step(int dir);
    }

    /**
     * A selectable row, drawn on Canvas: icon, title, subtitle (one or several lines), and on the right either a
     * badge or a value with « < > » (a setting). Focus shows a green outline with a soft glow and gradient; left and
     * right step a setting's value (and so do taps on its arrows). Dim rows stay focusable so that a press can say
     * why nothing happens.
     */
    static final class MenuRow extends View {
        Icon icon;
        String title = "", subtitle, badge, value;
        int badgeColor = Theme.TEXT_DIM, subMaxLines = 1, pips, pip = -1;
        boolean dim;
        Cycler cycler;

        private float focusT;
        private ValueAnimator anim;
        private final Paint fillP = fill(0), glowP = stroke(Theme.ACCENT, 2.5f), ringP = stroke(Theme.LINE, 2.5f);
        private final Paint iconP = stroke(Theme.TEXT_DIM, 3.5f), chevP = fill(Theme.TEXT_FAINT), pipP = fill(0);
        private final TextPaint titleP = textPaint(Theme.T_ROW, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint subP = textPaint(Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint valueP = textPaint(Theme.T_BODY + 2, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint badgeP = textPaint(Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT_DIM);
        private final RectF r = new RectF();
        private final Path tri = new Path();
        private StaticLayout subLayout;
        private int subLayoutW = -1;
        private String subLayoutText;
        private float leftArrowX = -1;
        private boolean downOnLeftArrow;

        MenuRow(Context c) {
            super(c);
            setFocusable(true);
            setClickable(true);
            glowP.setShadowLayer(18, 0, 0, Theme.alpha(Theme.ACCENT, 0.6f));
        }

        MenuRow set(Icon icon, String title, String subtitle) {
            this.icon = icon;
            this.title = title == null ? "" : title;
            this.subtitle = subtitle;
            invalidate();
            return this;
        }

        void setValue(String value, int pip, int pips) {
            this.value = value;
            this.pip = pip;
            this.pips = pips;
            invalidate();
        }

        void setBadge(String badge, int color) {
            this.badge = badge;
            this.badgeColor = color;
            invalidate();
        }

        void setDim(boolean dim) {
            this.dim = dim;
            invalidate();
        }

        void setSubtitle(String s) {
            subtitle = s;
            invalidate();
        }

        @Override
        protected void onFocusChanged(boolean gained, int direction, Rect previous) {
            super.onFocusChanged(gained, direction, previous);
            if (anim != null)
                anim.cancel();
            anim = ValueAnimator.ofFloat(focusT, gained ? 1 : 0);
            anim.setDuration(140);
            anim.addUpdateListener(a -> {
                focusT = (float)a.getAnimatedValue();
                invalidate();
            });
            anim.start();
        }

        @Override
        protected void drawableStateChanged() {
            super.drawableStateChanged();
            invalidate();
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent e) {
            if (cycler != null && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)) {
                cycler.step(keyCode == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : 1);
                return true;
            }
            return super.onKeyDown(keyCode, e);
        }

        /** A tap that starts and ends on the left arrow steps back; any other tap is a click (one step forward). */
        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getAction() == MotionEvent.ACTION_DOWN)
                downOnLeftArrow = cycler != null && leftArrowX >= 0 && Math.abs(e.getX() - leftArrowX) < 48;
            if (downOnLeftArrow && e.getAction() == MotionEvent.ACTION_UP && Math.abs(e.getX() - leftArrowX) < 48) {
                downOnLeftArrow = false;
                MotionEvent cancel = MotionEvent.obtain(e);
                cancel.setAction(MotionEvent.ACTION_CANCEL);
                super.onTouchEvent(cancel);   // clears the pressed state without a click
                cancel.recycle();
                cycler.step(-1);
                return true;
            }
            return super.onTouchEvent(e);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), pad = 12;
            float t = focusT;
            r.set(pad, pad, w - pad, h - pad);
            if (t > 0.01f) {
                fillP.setShader(new LinearGradient(0, 0, w, 0, Theme.alpha(Theme.ACCENT, 0.20f * t),
                                                   Theme.alpha(Theme.ACCENT, 0.03f * t), Shader.TileMode.CLAMP));
                c.drawRoundRect(r, Theme.ROW_RADIUS, Theme.ROW_RADIUS, fillP);
                fillP.setShader(null);
                glowP.setColor(Theme.alpha(Theme.ACCENT, t));
                c.drawRoundRect(r, Theme.ROW_RADIUS, Theme.ROW_RADIUS, glowP);
            }
            if (isPressed()) {
                fillP.setColor(Theme.alpha(Theme.ACCENT, 0.12f));
                c.drawRoundRect(r, Theme.ROW_RADIUS, Theme.ROW_RADIUS, fillP);
            }

            float x = pad + 30;
            int fg = dim ? Theme.TEXT_FAINT : Theme.TEXT;
            if (icon != null) {
                float rad = Math.min(h * 0.29f, 40);
                float cx = x + rad, cy = h / 2;
                ringP.setColor(blend(Theme.LINE, Theme.ACCENT, dim ? t * 0.5f : t));
                c.drawCircle(cx, cy, rad, ringP);
                iconP.setColor(dim ? Theme.TEXT_FAINT : blend(Theme.TEXT_DIM, Theme.ACCENT, t));
                drawIcon(c, icon, cx, cy, rad * 0.95f, iconP);
                x = cx + rad + 30;
            }

            // Right side first: it decides how much room the texts get.
            float right = w - pad - 34;
            leftArrowX = -1;
            if (value != null) {
                float vw = valueP.measureText(value);
                float arrow = 12, gapA = 22;
                float vx = right - arrow - gapA - vw;
                float cy = h / 2 + (pips > 1 ? -6 : 0);
                valueP.setColor(dim ? Theme.TEXT_FAINT : blend(Theme.TEXT, Theme.ACCENT, t * 0.25f));
                Paint.FontMetrics fm = valueP.getFontMetrics();
                c.drawText(value, vx, cy - (fm.ascent + fm.descent) / 2, valueP);
                if (cycler != null) {
                    chevP.setColor(blend(Theme.TEXT_FAINT, Theme.ACCENT, t));
                    float lx = vx - gapA - arrow, rx = right - arrow;
                    triangle(c, lx, cy, arrow, -1);
                    triangle(c, rx, cy, arrow, 1);
                    leftArrowX = lx + arrow / 2;
                    right = lx - 24;
                } else {
                    right = vx - 24;
                }
                if (pips > 1) {
                    float pw = 18, pg = 6, total = pips * pw + (pips - 1) * pg;
                    float px = vx + vw / 2 - total / 2, py = cy + 26;
                    for (int i = 0; i < pips; i++) {
                        pipP.setColor(i == pip ? (dim ? Theme.TEXT_FAINT : Theme.ACCENT) : Theme.LINE);
                        c.drawRoundRect(px, py, px + pw, py + 4, 2, 2, pipP);
                        px += pw + pg;
                    }
                }
            } else if (badge != null && !badge.isEmpty()) {
                badgeP.setColor(badgeColor);
                float bw = badgeP.measureText(badge);
                Paint.FontMetrics fm = badgeP.getFontMetrics();
                c.drawText(badge, right - bw, h / 2 - (fm.ascent + fm.descent) / 2, badgeP);
                right -= bw + 30;
            }

            float avail = Math.max(10, right - x);
            titleP.setColor(fg);
            String ti = TextUtils.ellipsize(title, titleP, avail, TextUtils.TruncateAt.END).toString();
            Paint.FontMetrics tf = titleP.getFontMetrics();
            float titleH = -tf.ascent;
            boolean hasSub = subtitle != null && !subtitle.isEmpty();
            if (!hasSub) {
                c.drawText(ti, x, h / 2 - (tf.ascent + tf.descent) / 2, titleP);
                return;
            }
            int lw = (int)avail;
            if (subLayout == null || subLayoutW != lw || !subtitle.equals(subLayoutText)) {
                subLayout = StaticLayout.Builder.obtain(subtitle, 0, subtitle.length(), subP, lw)
                        .setAlignment(Layout.Alignment.ALIGN_NORMAL).setLineSpacing(0, 1.22f)
                        .setIncludePad(false).setMaxLines(subMaxLines).setEllipsize(TextUtils.TruncateAt.END)
                        .build();
                subLayoutW = lw;
                subLayoutText = subtitle;
            }
            subP.setColor(dim ? Theme.TEXT_FAINT : Theme.TEXT_DIM);
            float gap = 12, block = titleH + gap + subLayout.getHeight();
            float top = (h - block) / 2;
            c.drawText(ti, x, top + titleH, titleP);
            c.save();
            c.translate(x, top + titleH + gap);
            subLayout.draw(c);
            c.restore();
        }

        private void triangle(Canvas c, float x, float cy, float s, int dir) {
            tri.reset();
            if (dir < 0) {
                tri.moveTo(x + s, cy - s);
                tri.lineTo(x, cy);
                tri.lineTo(x + s, cy + s);
            } else {
                tri.moveTo(x, cy - s);
                tri.lineTo(x + s, cy);
                tri.lineTo(x, cy + s);
            }
            tri.close();
            c.drawPath(tri, chevP);
        }
    }

    // ---- Chip: a status pill ---------------------------------------------------------------------------------------

    static final class Chip extends View {
        private String label = "";
        private int color = Theme.ACCENT;
        private final Paint dotP = fill(Theme.ACCENT), bgP = fill(0), lineP = stroke(0, 1.5f);
        private final TextPaint tp = textPaint(Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT);
        private final RectF r = new RectF();

        Chip(Context c) {
            super(c);
        }

        void set(String label, int color) {
            this.label = label == null ? "" : label;
            this.color = color;
            requestLayout();
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(label.isEmpty() ? 0 : Math.round(tp.measureText(label) + 78), 52);
        }

        @Override
        protected void onDraw(Canvas c) {
            if (label.isEmpty())
                return;
            float h = getHeight();
            r.set(1, 1, getWidth() - 1, h - 1);
            bgP.setColor(Theme.alpha(color, 0.09f));
            lineP.setColor(Theme.alpha(color, 0.5f));
            c.drawRoundRect(r, h / 2, h / 2, bgP);
            c.drawRoundRect(r, h / 2, h / 2, lineP);
            dotP.setColor(color);
            dotP.setShadowLayer(8, 0, 0, Theme.alpha(color, 0.8f));
            c.drawCircle(28, h / 2, 6, dotP);
            Paint.FontMetrics fm = tp.getFontMetrics();
            c.drawText(label, 48, h / 2 - (fm.ascent + fm.descent) / 2, tp);
        }
    }

    // ---- Hint: a controller button and what it does (also a touch target) ----------------------------------------

    static final class Hint extends View {
        private final String glyph, label;
        private final Paint face = fill(Theme.GLYPH_FACE);
        private final TextPaint ink = textPaint(24, Theme.MEDIUM, Theme.GLYPH_INK);
        private final TextPaint tp = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);
        private final RectF r = new RectF();

        Hint(Context c, String glyph, String label, View.OnClickListener click) {
            super(c);
            this.glyph = glyph;
            this.label = label;
            setOnClickListener(click);
            setFocusable(false);
        }

        private float glyphW() {
            return glyph.length() <= 1 ? 46 : ink.measureText(glyph) + 30;
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(glyphW() + 16 + tp.measureText(label) + 8), 64);
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight(), gw = glyphW();
            face.setColor(isPressed() ? Theme.ACCENT : Theme.GLYPH_FACE);
            r.set(0, h / 2 - 23, gw, h / 2 + 23);
            c.drawRoundRect(r, 23, 23, face);
            Paint.FontMetrics fm = ink.getFontMetrics();
            float gx = gw / 2 - ink.measureText(glyph) / 2;
            c.drawText(glyph, gx, h / 2 - (fm.ascent + fm.descent) / 2, ink);
            Paint.FontMetrics lm = tp.getFontMetrics();
            c.drawText(label, gw + 16, h / 2 - (lm.ascent + lm.descent) / 2, tp);
        }

        @Override
        protected void drawableStateChanged() {
            super.drawableStateChanged();
            invalidate();
        }
    }

    // ---- Toggle: a button glyph, a label and an on/off switch ------------------------------------------------------

    static final class Toggle extends View {
        private final String glyph, label;
        private boolean on;
        private final Paint face = fill(Theme.GLYPH_FACE), track = fill(0), knob = fill(Theme.TEXT);
        private final TextPaint ink = textPaint(24, Theme.MEDIUM, Theme.GLYPH_INK);
        private final TextPaint tp = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);
        private final RectF r = new RectF();

        Toggle(Context c, String glyph, String label) {
            super(c);
            this.glyph = glyph;
            this.label = label;
            setClickable(true);
            setFocusable(false);
        }

        void setOn(boolean on) {
            this.on = on;
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(46 + 18 + tp.measureText(label) + 24 + 72 + 4), 64);
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight();
            c.drawCircle(23, h / 2, 23, face);
            Paint.FontMetrics fm = ink.getFontMetrics();
            c.drawText(glyph, 23 - ink.measureText(glyph) / 2, h / 2 - (fm.ascent + fm.descent) / 2, ink);
            Paint.FontMetrics lm = tp.getFontMetrics();
            float lx = 46 + 18;
            c.drawText(label, lx, h / 2 - (lm.ascent + lm.descent) / 2, tp);
            float sx = lx + tp.measureText(label) + 24;
            r.set(sx, h / 2 - 18, sx + 72, h / 2 + 18);
            track.setColor(on ? Theme.ACCENT_DEEP : Theme.LINE);
            c.drawRoundRect(r, 18, 18, track);
            knob.setColor(on ? Theme.ACCENT : Theme.TEXT_DIM);
            c.drawCircle(on ? sx + 54 : sx + 18, h / 2, 13, knob);
        }
    }

    // ---- Ring: the import progress -----------------------------------------------------------------------------------

    static final class Ring extends View {
        private float frac = -1;
        private String big = "", label = "", sub = "";
        private final Paint trackP = stroke(0xFF1A241F, 18), arcP = stroke(Theme.ACCENT, 18);
        private final Paint tickP = stroke(Theme.LINE, 3), innerP = stroke(Theme.alpha(Theme.ACCENT, 0.25f), 1.5f);
        private final TextPaint bigP = textPaint(118, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint labelP = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint subP = textPaint(Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT);
        private final RectF r = new RectF();

        Ring(Context c) {
            super(c);
            arcP.setShadowLayer(20, 0, 0, Theme.alpha(Theme.ACCENT, 0.65f));
        }

        /** The arc's colour (the accent; the Prepare screen's failed / stopped states use ERROR / WARN). */
        void setColor(int color) {
            arcP.setColor(color);
            arcP.setShadowLayer(20, 0, 0, Theme.alpha(color, 0.65f));
            invalidate();
        }

        /** frac below 0: indeterminate (a turning arc). */
        void set(float frac, String big, String label, String sub) {
            this.frac = frac;
            this.big = big == null ? "" : big;
            this.label = label == null ? "" : label;
            this.sub = sub == null ? "" : sub;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            float R = Math.min(getWidth(), getHeight()) / 2f - 56;
            int n = 72;
            for (int i = 0; i < n; i++) {
                double a = Math.toRadians(-90 + i * 360.0 / n);
                boolean lit = frac >= 0 && i < Math.round(frac * n);
                tickP.setColor(lit ? Theme.alpha(arcP.getColor(), 0.9f) : Theme.alpha(Theme.LINE, 0.9f));
                float r0 = R + 30, r1 = R + (i % 6 == 0 ? 48 : 40);
                c.drawLine(cx + (float)Math.cos(a) * r0, cy + (float)Math.sin(a) * r0,
                           cx + (float)Math.cos(a) * r1, cy + (float)Math.sin(a) * r1, tickP);
            }
            r.set(cx - R, cy - R, cx + R, cy + R);
            c.drawArc(r, 0, 360, false, trackP);
            c.drawCircle(cx, cy, R - 30, innerP);
            if (frac >= 0) {
                if (frac > 0.002f)
                    c.drawArc(r, -90, 360 * Math.min(1, frac), false, arcP);
            } else {
                float start = (SystemClock.uptimeMillis() % 1400) * 360f / 1400f - 90;
                c.drawArc(r, start, 80, false, arcP);
                postInvalidateOnAnimation();
            }
            Paint.FontMetrics bf = bigP.getFontMetrics();
            float by = cy - 18;
            c.drawText(big, cx - bigP.measureText(big) / 2, by, bigP);
            float ly = by - bf.descent + 58;
            String l = TextUtils.ellipsize(label, labelP, R * 1.5f, TextUtils.TruncateAt.END).toString();
            c.drawText(l, cx - labelP.measureText(l) / 2, ly, labelP);
            String s = TextUtils.ellipsize(sub, subP, R * 1.4f, TextUtils.TruncateAt.END).toString();
            c.drawText(s, cx - subP.measureText(s) / 2, ly + 42, subP);
        }
    }

    // ---- Meter: a thin bar (storage) ---------------------------------------------------------------------------------

    static final class Meter extends View {
        private float frac;
        private int color = Theme.ACCENT;
        private final Paint track = fill(Theme.LINE), bar = fill(Theme.ACCENT);

        Meter(Context c) {
            super(c);
        }

        void set(float frac, int color) {
            this.frac = Math.max(0, Math.min(1, frac));
            this.color = color;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            c.drawRoundRect(0, 0, w, h, h / 2, h / 2, track);
            bar.setColor(color);
            if (frac > 0)
                c.drawRoundRect(0, 0, Math.max(h, w * frac), h, h / 2, h / 2, bar);
        }
    }
}
