package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;

/**
 * The launcher's second screen (docs/android/app-shell.md « Dual screen »), on Home and on the library: pure black,
 * the game's title image (440 px), its title, and one action as a pill (Play, Prepare, Import…), with the battery
 * small in a corner. Nothing in the bottom centre (palms). On the other screens it shows the art alone. Shown through
 * LauncherCompanion.
 */
final class LauncherPanel extends PanelView {
    interface Actions {
        /** The one action: Home's Play (or what stands for it), the focused library game's primary action. */
        void play();
    }

    static final int PLAY = 1;
    static final long DIM_MS = 10_000;

    private final Actions actions;
    private String title = "", status = "", batteryText = "";
    private int statusColor = Theme.TEXT_DIM;
    private boolean artOnly;
    private final RectF artBox = new RectF(400, 120, 840, 560);
    private final Paint artP = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final Path artClip = new Path();
    private Bitmap art;

    LauncherPanel(Context c, Actions actions) {
        super(c, DIM_MS);
        this.actions = actions;
        artClip.addRoundRect(artBox, Theme.ART_RADIUS, Theme.ART_RADIUS, Path.Direction.CW);
        Button play = new Button(PLAY, Kind.TAP, 380, 716, 860, 844, PanelIcons.Icon.PLAY, c.getString(R.string.PLAY),
                                 null);
        play.accent = true;
        buttons.add(play);
    }

    /**
     * Home: the title ("" = none set up) and whether its action can run; the status is kept for the tests but not
     * drawn (the main screen says it).
     */
    void set(String title, String status, int statusColor, boolean playEnabled) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        artOnly = false;
        button(PLAY).enabled = playEnabled;
        invalidate();
    }

    /** The action's label (Play, Prepare, Import…); null: greyed with the last label. The icon is Play's only. */
    void setAction(String label) {
        Button b = button(PLAY);
        b.enabled = label != null;
        if (label != null)
            b.label = label;
        b.icon = getContext().getString(R.string.PLAY).equals(b.label) ? PanelIcons.Icon.PLAY : null;
        invalidate();
    }

    /** The other screens: the art alone, no title, no action. */
    void setArtOnly(boolean on) {
        artOnly = on;
        button(PLAY).enabled = !on && button(PLAY).enabled;
        invalidate();
    }

    boolean artOnlyForTest() {
        return artOnly;
    }

    /** The game's title image (null: none; the title then sits higher). */
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

    String actionForTest() {
        Button b = button(PLAY);
        return b.enabled ? b.label : null;
    }

    @Override
    protected void onAction(Button b) {
        if (actions != null && b.id == PLAY && !artOnly)
            actions.play();
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
        if (dimmed) {
            if (!artOnly)
                drawButton(c, button(PLAY), true);
            return;
        }
        drawBattery(c, batteryText);
        if (art != null) {
            c.save();
            c.clipPath(artClip);
            c.drawBitmap(art, null, artBox, artP);
            c.restore();
            stroke.setStrokeWidth(2);
            stroke.setColor(Theme.alpha(Theme.TEXT, 0.10f));
            c.drawRoundRect(artBox, Theme.ART_RADIUS, Theme.ART_RADIUS, stroke);
        }
        if (artOnly)
            return;
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Theme.MEDIUM);
        text.setTextSize(Theme.T_TITLE);
        text.setColor(Theme.TEXT);
        String t = title.isEmpty() ? getContext().getString(R.string.PANEL_NO_GAME) : title;
        drawFit(c, t, W / 2, art != null ? 632 : 420, W - 160);
        drawButton(c, button(PLAY), false);
    }
}
