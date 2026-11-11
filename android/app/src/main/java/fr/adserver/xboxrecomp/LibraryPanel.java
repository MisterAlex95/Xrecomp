package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;

/**
 * The launcher's second screen on the library (docs/android/app-shell.md « Game library », « Dual screen »): the
 * focused game's identity card (title image, title, state) and three touch shortcuts: the game's primary action (Play,
 * Import, Prepare...), Rescan and Settings. Pure black, the same dim rules as LauncherPanel; shown through
 * LauncherCompanion.library().
 */
final class LibraryPanel extends PanelView {
    interface Actions {
        void primary();

        void rescan();

        void settings();
    }

    static final int PRIMARY = 1, RESCAN = 2, SETTINGS = 3;

    private final Actions actions;
    private String title = "", status = "", batteryText = "";
    private int statusColor = Theme.TEXT_DIM, thermal = -1;
    private final RectF card = new RectF(36, 140, 1204, 470);
    private final RectF artBox = new RectF(84, 176, 342, 434);
    private final Paint artP = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Path artClip = new Path();
    private Bitmap art;

    LibraryPanel(Context c, Actions actions) {
        super(c, LauncherPanel.DIM_MS);
        this.actions = actions;
        artClip.addRoundRect(artBox, 22, 22, Path.Direction.CW);
        Button primary = new Button(PRIMARY, Kind.TAP, 36, 510, 520, 860, PanelIcons.Icon.PLAY,
                                    c.getString(R.string.PLAY), null);
        primary.accent = true;
        buttons.add(primary);
        buttons.add(new Button(RESCAN, Kind.TAP, 548, 510, 872, 860, PanelIcons.Icon.DISC,
                               c.getString(R.string.LIB_RESCAN), null));
        buttons.add(new Button(SETTINGS, Kind.TAP, 900, 510, 1204, 860, PanelIcons.Icon.SETTINGS,
                               c.getString(R.string.SETTINGS), null));
    }

    /**
     * The focused game's card: title ("" = none: the library folder or an empty library), its state and colour, the
     * primary button's label (null: greyed, with the last label).
     */
    void set(String title, String status, int statusColor, String action) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        Button p = button(PRIMARY);
        p.enabled = action != null;
        if (action != null)
            p.label = action;
        invalidate();
    }

    void setArt(Bitmap b) {
        if (b != art) {
            art = b;
            invalidate();
        }
    }

    boolean hasArtForTest() {
        return art != null;
    }

    String titleForTest() {
        return title;
    }

    @Override
    protected void onAction(Button b) {
        if (actions == null)
            return;
        switch (b.id) {
        case PRIMARY:
            actions.primary();
            break;
        case RESCAN:
            actions.rescan();
            break;
        case SETTINGS:
            actions.settings();
            break;
        default:
            break;
        }
    }

    @Override
    protected boolean onTick() {
        Context c = getContext();
        String bat = battery(c);
        int th = thermal(c);
        boolean changed = !bat.equals(batteryText) || th != thermal;
        batteryText = bat;
        thermal = th;
        return changed;
    }

    @Override
    protected void drawPanel(Canvas c, boolean dimmed) {
        Context ctx = getContext();
        if (dimmed) {
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.alpha(Theme.TEXT_FAINT, 0.32f));
            c.drawRoundRect(card, 28, 28, stroke);
            for (Button b : buttons)
                drawButton(c, b, true);
            return;
        }
        drawStrip(c, null, batteryText, thermal);
        drawCard(c, card);
        float tx = 84;
        if (art != null) {
            c.save();
            c.clipPath(artClip);
            c.drawBitmap(art, null, artBox, artP);
            c.restore();
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.LINE);
            c.drawRoundRect(artBox, 22, 22, stroke);
            tx = 372;
        }
        text.setTextAlign(Paint.Align.LEFT);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_CAPS);
        text.setLetterSpacing(0.22f);
        text.setColor(Theme.ACCENT);
        c.drawText(ctx.getString(R.string.LIB_TITLE).toUpperCase(java.util.Locale.ROOT), tx, 212, text);
        text.setLetterSpacing(0);
        text.setTypeface(Theme.LIGHT);
        text.setTextSize(Theme.T_HERO);
        text.setColor(Theme.TEXT);
        String t = title.isEmpty() ? ctx.getString(R.string.LIB_PANEL_NONE) : title;
        drawFit(c, t, tx, 318, card.right - 48 - tx);
        if (!status.isEmpty())
            drawStatus(c, tx, 410, statusColor, status, Theme.TEXT_DIM);
        for (Button b : buttons)
            drawButton(c, b, false);
    }
}
