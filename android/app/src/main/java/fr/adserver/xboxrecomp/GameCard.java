package fr.adserver.xboxrecomp;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.View;

/**
 * One game of the library grid (docs/android/app-shell.md « Game library »): its title image (TitleArt's cached PNG)
 * or a monogram on the title ID's hue, its title on two lines, and its state chip (GameState). The grid's last tile,
 * « Library folder », is a card with an icon instead of art. Focusable like a MenuRow: it scales up a little when
 * focused, and the launcher's Glide draws the highlight around it. Colours are read from Theme when drawn (the
 * library is always in the stock colours).
 */
final class GameCard extends View {
    /** The art square, in design pixels, and the card's height under it (title and chip). */
    static final int ART = 232, W = 280, H = 404;

    private Bitmap art;
    private String initials = "";
    private int monoColor = Theme.ACCENT_DEEP;
    private Widgets.Icon icon;
    private String title = "", chip = "";
    private int chipColor = Theme.TEXT_DIM;
    private boolean dim;
    private float focusT;
    private ValueAnimator anim;

    private final Paint fillP = new Paint(Paint.ANTI_ALIAS_FLAG), lineP = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bmpP = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint iconP = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final TextPaint titleP = new TextPaint(Paint.ANTI_ALIAS_FLAG), chipP = new TextPaint(Paint.ANTI_ALIAS_FLAG),
            monoP = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF(), artR = new RectF();
    private final Path clip = new Path();
    private StaticLayout titleLayout;
    private int titleLayoutW = -1;

    GameCard(Context c) {
        super(c);
        setFocusable(true);
        setClickable(true);
        lineP.setStyle(Paint.Style.STROKE);
        lineP.setStrokeWidth(2);
        iconP.setStyle(Paint.Style.STROKE);
        iconP.setStrokeWidth(5);
        iconP.setStrokeCap(Paint.Cap.ROUND);
        iconP.setStrokeJoin(Paint.Join.ROUND);
        titleP.setTextSize(Theme.T_SMALL + 2);
        titleP.setTypeface(Theme.MEDIUM);
        chipP.setTextSize(Theme.T_CAPS);
        chipP.setTypeface(Theme.MEDIUM);
        monoP.setTypeface(Theme.MEDIUM);
    }

    /** A game: its art (null: the monogram of title on the colour of key), its title and its chip. */
    GameCard setGame(Bitmap art, String key, String title, String chip, int chipColor) {
        this.art = art;
        this.icon = null;
        this.title = title == null ? "" : title;
        this.initials = TitleArt.initials(this.title);
        this.monoColor = TitleArt.monogramColor(key == null ? this.title : key);
        this.chip = chip == null ? "" : chip;
        this.chipColor = chipColor;
        titleLayoutW = -1;
        invalidate();
        return this;
    }

    /** A tile with an icon (« Library folder »). */
    GameCard setTile(Widgets.Icon icon, String title, String sub) {
        this.art = null;
        this.icon = icon;
        this.title = title == null ? "" : title;
        this.chip = sub == null ? "" : sub;
        this.chipColor = Theme.TEXT_DIM;
        titleLayoutW = -1;
        invalidate();
        return this;
    }

    void setDim(boolean d) {
        dim = d;
        invalidate();
    }

    void setChip(String s, int color) {
        chip = s == null ? "" : s;
        chipColor = color;
        invalidate();
    }

    String titleForTest() {
        return title;
    }

    String chipForTest() {
        return chip;
    }

    boolean hasArtForTest() {
        return art != null;
    }

    /** The art square in the card's own coordinates (the Glide's ring goes around it). */
    void artRect(RectF out) {
        float w = getWidth(), pad = 12;
        float side = Math.min(ART, w - 2 * pad - 24);
        float ax = (w - side) / 2, ay = pad + 14;
        out.set(ax, ay, ax + side, ay + side);
    }

    @Override
    protected void onFocusChanged(boolean gained, int direction, Rect previous) {
        super.onFocusChanged(gained, direction, previous);
        if (anim != null)
            anim.cancel();
        anim = ValueAnimator.ofFloat(focusT, gained ? 1 : 0);
        anim.setDuration(Theme.ANIM_FOCUS_MS);
        anim.addUpdateListener(a -> {
            focusT = (float)a.getAnimatedValue();
            float s = 1 + (Theme.FOCUS_SCALE * 1.02f - 1) * focusT;
            setScaleX(s);
            setScaleY(s);
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
    protected void onDraw(Canvas c) {
        float w = getWidth(), pad = 12;
        float t = focusT;
        r.set(pad, pad, w - pad, getHeight() - pad);
        if (isPressed()) {
            fillP.setShader(null);
            fillP.setColor(Theme.alpha(Theme.ACCENT, 0.12f));
            c.drawRoundRect(r, Theme.ROW_RADIUS, Theme.ROW_RADIUS, fillP);
        }
        float side = Math.min(ART, w - 2 * pad - 24);
        float ax = (w - side) / 2, ay = pad + 14;
        artR.set(ax, ay, ax + side, ay + side);
        float rad = 22;
        clip.reset();
        clip.addRoundRect(artR, rad, rad, Path.Direction.CW);
        c.save();
        c.clipPath(clip);
        if (art != null) {
            fillP.setShader(null);
            fillP.setColor(0xFF000000);
            c.drawRect(artR, fillP);
            float s = Math.max(artR.width() / art.getWidth(), artR.height() / art.getHeight());
            float dw = art.getWidth() * s, dh = art.getHeight() * s;
            c.drawBitmap(art, null, new RectF(artR.centerX() - dw / 2, artR.centerY() - dh / 2,
                                              artR.centerX() + dw / 2, artR.centerY() + dh / 2), bmpP);
        } else if (icon != null) {
            fillP.setShader(new LinearGradient(0, artR.top, 0, artR.bottom, Theme.PANEL_HI, Theme.BG_BOTTOM,
                                               Shader.TileMode.CLAMP));
            c.drawRect(artR, fillP);
            fillP.setShader(null);
            iconP.setColor(Theme.blend(Theme.TEXT_DIM, Theme.ACCENT, t));
            Widgets.drawIcon(c, icon, artR.centerX(), artR.centerY(), side * 0.30f, iconP);
        } else {
            float[] hsv = new float[3];
            android.graphics.Color.colorToHSV(monoColor, hsv);
            int c1 = Theme.hsv(hsv[0], 0.62f, 0.50f), c2 = Theme.hsv(hsv[0] + 34, 0.70f, 0.16f);
            fillP.setShader(new LinearGradient(artR.left, artR.top, artR.right, artR.bottom, c1, c2,
                                               Shader.TileMode.CLAMP));
            c.drawRect(artR, fillP);
            fillP.setShader(null);
            String s = initials.isEmpty() ? "?" : initials;
            monoP.setColor(Theme.alpha(Theme.TEXT_BRIGHT, 0.94f));
            monoP.setTextSize(side * 0.38f);
            float tw = monoP.measureText(s);
            if (tw > side * 0.8f)
                monoP.setTextSize(side * 0.38f * side * 0.8f / tw);
            Paint.FontMetrics fm = monoP.getFontMetrics();
            c.drawText(s, artR.centerX() - monoP.measureText(s) / 2, artR.centerY() - (fm.ascent + fm.descent) / 2,
                       monoP);
        }
        if (dim) {
            fillP.setShader(null);
            fillP.setColor(0x88000000);
            c.drawRect(artR, fillP);
        }
        c.restore();
        lineP.setColor(Theme.blend(Theme.alpha(Theme.TEXT, 0.14f), Theme.ACCENT, t));
        c.drawRoundRect(artR, rad, rad, lineP);

        // Title: two lines at most, ellipsized.
        float tx = pad + 16, tw = w - 2 * tx;
        int tws = Math.max(1, (int)tw);
        if (titleLayout == null || titleLayoutW != tws) {
            titleLayout = StaticLayout.Builder.obtain(title, 0, title.length(), titleP, tws)
                    .setMaxLines(2).setEllipsize(TextUtils.TruncateAt.END).setAlignment(Layout.Alignment.ALIGN_NORMAL)
                    .setIncludePad(false).build();
            titleLayoutW = tws;
        }
        titleP.setColor(dim ? Theme.TEXT_FAINT : Theme.blend(Theme.TEXT, Theme.TEXT_BRIGHT, t));
        c.save();
        c.translate(tx, artR.bottom + 16);
        titleLayout.draw(c);
        c.restore();

        if (!chip.isEmpty()) {
            float cy = artR.bottom + 16 + 2 * titleP.getFontSpacing() + 24;
            fillP.setShader(null);
            fillP.setColor(chipColor);
            c.drawCircle(tx + 6, cy, 6, fillP);
            chipP.setColor(dim ? Theme.TEXT_FAINT : Theme.TEXT_DIM);
            Paint.FontMetrics fm = chipP.getFontMetrics();
            c.drawText(Widgets.fit(chip, chipP, tw - 22), tx + 22, cy - (fm.ascent + fm.descent) / 2, chipP);
        }
    }
}
