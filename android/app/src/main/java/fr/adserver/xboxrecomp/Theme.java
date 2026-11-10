package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.util.TypedValue;
import android.widget.TextView;

/**
 * The launcher's look (XBA-41): colours, type and sizes. Every size is in design pixels of a 1920 x 1080 stage,
 * which LauncherActivity scales to the screen (Widgets.Stage); so a value here looks the same at 1280 x 720 and
 * at 1920 x 1080. System fonts only.
 */
final class Theme {
    private Theme() {}

    /** The design grid: the stage is at least this big, and scaled to fit the screen. */
    static final int DESIGN_W = 1920, DESIGN_H = 1080;

    // ---- Colours: a deep green-black, one vivid green, warm off-white text --------------------------------------
    static final int BG_TOP = 0xFF0C110F;
    static final int BG_BOTTOM = 0xFF060807;
    static final int PANEL = 0xE6111814;
    static final int PANEL_HI = 0xFF18211C;
    static final int LINE = 0xFF27332D;
    static final int LINE_SOFT = 0x6627332D;
    static final int TEXT = 0xFFF3EEE4;
    /** A focused row's title: a step brighter than TEXT. */
    static final int TEXT_BRIGHT = 0xFFFFFFFF;
    static final int TEXT_DIM = 0xFFA9AFA6;
    static final int TEXT_FAINT = 0xFF6C746D;
    static final int ACCENT = 0xFF4FE38C;
    static final int ACCENT_DEEP = 0xFF1F7A48;
    static final int WARN = 0xFFF2BA52;
    static final int ERROR = 0xFFFF6F61;
    /** The face of a controller button glyph (A, B, ...), and the letter on it. */
    static final int GLYPH_FACE = 0xFFEDE6D8;
    static final int GLYPH_INK = 0xFF0C110F;

    /** A colour from hue (degrees), saturation and value (0..1), opaque. */
    static int hsv(float h, float s, float v) {
        return Color.HSVToColor(new float[] { ((h % 360) + 360) % 360, s, v });
    }

    static int alpha(int color, float a) {
        int al = Math.max(0, Math.min(255, Math.round(Color.alpha(color) * a)));
        return (color & 0x00FFFFFF) | al << 24;
    }

    // ---- Type ---------------------------------------------------------------------------------------------------
    static final Typeface REGULAR = Typeface.create("sans-serif", Typeface.NORMAL);
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);
    static final Typeface LIGHT = Typeface.create("sans-serif-light", Typeface.NORMAL);

    static final float T_HERO = 76, T_TITLE = 46, T_HEAD = 40, T_ROW = 38, T_BODY = 29, T_SMALL = 25, T_CAPS = 22;

    // ---- Layout -------------------------------------------------------------------------------------------------
    static final int MARGIN_X = 72, MARGIN_TOP = 36, MARGIN_BOTTOM = 30;
    static final int TOP_BAR_H = 112, BOTTOM_BAR_H = 92, NOTE_H = 64;
    static final int GAP = 40, RADIUS = 22, ROW_RADIUS = 18;
    static final int ROW_H = 112, ROW_H_SMALL = 96, CARD_H = 230;
    /** Home: the hero's art tile, its primary button, the secondary row's tiles. */
    static final int ART = 400, PRIMARY_W = 660, PRIMARY_H = 128, TILE_H = 128;

    // ---- Motion -------------------------------------------------------------------------------------------------
    /** A new screen fades in and slides in from the side it comes from (ANIM_SLIDE design pixels). */
    static final long ANIM_SCREEN_MS = 150;
    static final int ANIM_SLIDE = 40;
    /** The focus highlight glides from the row it leaves to the row it reaches. */
    static final long ANIM_GLIDE_MS = 150;
    /** A row's own focus change (title brightness, scale). */
    static final long ANIM_FOCUS_MS = 140;
    /** A setting's value pulses when it changes. */
    static final long ANIM_TICK_MS = 160;
    /** The focused row is drawn this much bigger. */
    static final float FOCUS_SCALE = 1.02f;

    /** A text view in design pixels. */
    static TextView text(Context c, float size, Typeface face, int color) {
        TextView t = new TextView(c);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, size);
        t.setTypeface(face);
        t.setTextColor(color);
        t.setIncludeFontPadding(false);
        return t;
    }

    static TextView text(Context c, float size, Typeface face, int color, CharSequence s) {
        TextView t = text(c, size, face, color);
        t.setText(s);
        return t;
    }

    /** A spaced-out caps label (section headers). */
    static TextView caps(Context c, CharSequence s, int color) {
        TextView t = text(c, T_CAPS, MEDIUM, color, s);
        t.setAllCaps(true);
        t.setLetterSpacing(0.22f);
        return t;
    }

    static TextView oneLine(TextView t) {
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        return t;
    }

    static TextView body(Context c, CharSequence s) {
        TextView t = text(c, T_BODY, REGULAR, TEXT_DIM, s);
        t.setLineSpacing(0, 1.28f);
        return t;
    }

    /** Body text a size down, for panels that hold longer explanations. */
    static TextView small(Context c, CharSequence s) {
        TextView t = text(c, T_BODY - 2, REGULAR, TEXT_DIM, s);
        t.setLineSpacing(0, 1.26f);
        return t;
    }
}
