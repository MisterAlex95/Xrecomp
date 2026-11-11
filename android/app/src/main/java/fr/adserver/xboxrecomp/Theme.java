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

    // ---- Colours ------------------------------------------------------------------------------------------------
    // Stock: a deep green-black, one vivid green, warm off-white text. A game's theme (GameTheme, from its title
    // image: GamePalette) replaces the accent, its glows and the neutrals' hue; the text and the semantic colours
    // (OK, WARN, ERROR) never change. The tokens are mutable: apply() sets them all, on the UI thread, before any view
    // is built, and whoever applies a new set rebuilds its views (paints copy the colours when they are made). So no
    // « static final » copy or switch case of a colour token outside this class.
    static int BG_TOP, BG_BOTTOM, PANEL, PANEL_HI, LINE, LINE_SOFT, TEXT,
    /** A focused row's title: a step brighter than TEXT. */
               TEXT_BRIGHT, TEXT_DIM, TEXT_FAINT,
    /** The brand colour (focus, caps headers, values, the Play button); ACCENT_DEEP and ACCENT_FACE darker steps. */
               ACCENT, ACCENT_DEEP, ACCENT_FACE,
    /** A meter's empty track. */
               TRACK,
    /** Status, not brand: ready, done, connected. Always green, whatever the game's colours. */
               OK, WARN, ERROR,
    /** The face of a controller button glyph (A, B, ...), and the letter on it. */
               GLYPH_FACE, GLYPH_INK,
    /** The backdrop's two soft glows (stock: the accent). */
               GLOW, GLOW2;

    /** A full set of colour tokens (GamePalette.NAMES order). */
    static final class Colors {
        static final Colors STOCK = new Colors(GamePalette.STOCK);
        private final int[] v;

        Colors(int[] values) {
            if (values.length != GamePalette.COUNT)
                throw new IllegalArgumentException("expected " + GamePalette.COUNT + " colours");
            v = values.clone();
        }

        int get(int token) {
            return v[token];
        }

        int[] values() {
            return v.clone();
        }

        @Override
        public boolean equals(Object o) {
            return o instanceof Colors && java.util.Arrays.equals(v, ((Colors)o).v);
        }

        @Override
        public int hashCode() {
            return java.util.Arrays.hashCode(v);
        }
    }

    private static Colors current;
    static {
        apply(Colors.STOCK);
    }

    /** Sets every colour token. UI thread, before the views that use them are built (rebuild them after). */
    static void apply(Colors c) {
        current = c;
        BG_TOP = c.get(GamePalette.BG_TOP);
        BG_BOTTOM = c.get(GamePalette.BG_BOTTOM);
        PANEL = c.get(GamePalette.PANEL);
        PANEL_HI = c.get(GamePalette.PANEL_HI);
        LINE = c.get(GamePalette.LINE);
        LINE_SOFT = c.get(GamePalette.LINE_SOFT);
        TEXT = c.get(GamePalette.TEXT);
        TEXT_BRIGHT = c.get(GamePalette.TEXT_BRIGHT);
        TEXT_DIM = c.get(GamePalette.TEXT_DIM);
        TEXT_FAINT = c.get(GamePalette.TEXT_FAINT);
        ACCENT = c.get(GamePalette.ACCENT);
        ACCENT_DEEP = c.get(GamePalette.ACCENT_DEEP);
        ACCENT_FACE = c.get(GamePalette.ACCENT_FACE);
        TRACK = c.get(GamePalette.TRACK);
        OK = c.get(GamePalette.OK);
        WARN = c.get(GamePalette.WARN);
        ERROR = c.get(GamePalette.ERROR);
        GLYPH_FACE = c.get(GamePalette.GLYPH_FACE);
        GLYPH_INK = c.get(GamePalette.GLYPH_INK);
        GLOW = c.get(GamePalette.GLOW);
        GLOW2 = c.get(GamePalette.GLOW2);
    }

    /** Back to the stock colours. */
    static void reset() {
        apply(Colors.STOCK);
    }

    static boolean isStock() {
        return current.equals(Colors.STOCK);
    }

    static Colors current() {
        return current;
    }

    /** A colour from hue (degrees), saturation and value (0..1), opaque. */
    static int hsv(float h, float s, float v) {
        return Color.HSVToColor(new float[] { ((h % 360) + 360) % 360, s, v });
    }

    static int alpha(int color, float a) {
        int al = Math.max(0, Math.min(255, Math.round(Color.alpha(color) * a)));
        return (color & 0x00FFFFFF) | al << 24;
    }

    /** The colour with full opacity. */
    static int opaque(int color) {
        return color | 0xFF000000;
    }

    /** a mixed with b (k = 0: a, 1: b), opaque. */
    static int blend(int a, int b, float k) {
        int out = 0xFF000000;
        for (int sh = 16; sh >= 0; sh -= 8)
            out |= Math.round((a >> sh & 0xff) * (1 - k) + (b >> sh & 0xff) * k) << sh;
        return out;
    }

    // ---- Type ---------------------------------------------------------------------------------------------------
    // « Calm » (docs/android/app-shell.md « Look and motion »): five sizes, two weights, nothing below T_SMALL, no caps
    // style. Secondary text is TEXT_DIM; TEXT_FAINT only says « disabled ».
    static final Typeface REGULAR = Typeface.create("sans-serif", Typeface.NORMAL);
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    static final float T_DISPLAY = 64, T_TITLE = 40, T_ROW = 34, T_BODY = 28, T_SMALL = 24;
    /** The hints' labels (between Small and Body: they are read at a glance). */
    static final float T_HINT = 26;

    // ---- Layout -------------------------------------------------------------------------------------------------
    /** The spacing scale: every gap is one of these. */
    static final int S1 = 8, S2 = 16, S3 = 24, S4 = 32, S5 = 48, S6 = 96;
    static final int MARGIN_X = 96, MARGIN_TOP = S5, MARGIN_BOTTOM = S3;
    /** The top bar (a screen's title, Settings' tabs) and the one footer line (the note left, the hints right). */
    static final int TOP_BAR_H = 96, FOOTER_H = 72;
    /** A list row (label and value), its focus plate's corner, the art tiles' corner. */
    static final int ROW_H = 88, ROW_RADIUS = 16, ART_RADIUS = 24;
    /** A row's text starts this far inside its focus plate: lists sit this much left of the margin. */
    static final int ROW_INSET = 32;
    /** Home: the art tile; a button (Play and the pills next to it). */
    static final int ART = 400, BUTTON_H = 96;
    /** The quiet right column of a list screen (the focused row's description). */
    static final int DESC_W = 600;
    /** An overlay's corner (the in-game menu's flash and overlay pills keep their own). */
    static final int RADIUS = 22;

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

    /** A group label (a Storage game's name): Small, sentence case, TEXT_DIM. */
    static TextView label(Context c, CharSequence s) {
        return text(c, T_SMALL, MEDIUM, TEXT_DIM, s);
    }


    static TextView oneLine(TextView t) {
        t.setSingleLine(true);
        t.setEllipsize(TextUtils.TruncateAt.END);
        return t;
    }

    static TextView body(Context c, CharSequence s) {
        TextView t = text(c, T_BODY, REGULAR, TEXT_DIM, s);
        t.setLineSpacing(0, 1.3f);
        return t;
    }
}
