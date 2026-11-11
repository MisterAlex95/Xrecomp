package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;

/**
 * The launcher's companion panel on the second screen (docs/android/app-shell.md « Dual screen »): an identity card
 * (the game's title image, title and status) and three big touch shortcuts, Play, Settings and Change game. Shown through
 * LauncherCompanion; nothing in the bottom centre (palms).
 */
final class LauncherPanel extends PanelView {
    interface Actions {
        void play();

        void settings();

        void changeGame();
    }

    static final int PLAY = 1, SETTINGS = 2, CHANGE = 3;
    static final long DIM_MS = 10_000;

    private final Actions actions;
    private String title = "", status = "", batteryText = "";
    private int statusColor = Theme.TEXT_DIM, thermal = -1;
    private final RectF card = new RectF(36, 140, 1204, 470);
    /** The title image on the card: 258 x 258, left; the text moves right of it. */
    private final RectF artBox = new RectF(84, 176, 342, 434);
    private final android.graphics.Paint artP = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG
                                                                           | android.graphics.Paint.ANTI_ALIAS_FLAG);
    private android.graphics.Bitmap art;

    LauncherPanel(Context c, Actions actions) {
        super(c, DIM_MS);
        this.actions = actions;
        Button play = new Button(PLAY, Kind.TAP, 36, 510, 520, 860, PanelIcons.Icon.PLAY, c.getString(R.string.PLAY),
                                 null);
        play.accent = true;
        buttons.add(play);
        buttons.add(new Button(SETTINGS, Kind.TAP, 548, 510, 872, 860, PanelIcons.Icon.SETTINGS,
                               c.getString(R.string.SETTINGS), null));
        buttons.add(new Button(CHANGE, Kind.TAP, 900, 510, 1204, 860, PanelIcons.Icon.DISC,
                               c.getString(R.string.CHANGE_GAME), null));
    }

    /** The identity card: title ("" = none set up), status line and its colour; Play greyed when it cannot start. */
    void set(String title, String status, int statusColor, boolean playEnabled) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        button(PLAY).enabled = playEnabled;
        invalidate();
    }

    /** The game's title image on the identity card (null: none, the text starts at the card's edge). */
    void setArt(android.graphics.Bitmap b) {
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
        case PLAY:
            actions.play();
            break;
        case SETTINGS:
            actions.settings();
            break;
        case CHANGE:
            actions.changeGame();
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
            android.graphics.Path clip = new android.graphics.Path();
            clip.addRoundRect(artBox, 22, 22, android.graphics.Path.Direction.CW);
            c.save();
            c.clipPath(clip);
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
        c.drawText(ctx.getString(R.string.app_name).toUpperCase(java.util.Locale.ROOT), tx, 212, text);
        text.setLetterSpacing(0);
        text.setTypeface(Theme.LIGHT);
        text.setTextSize(Theme.T_HERO);
        text.setColor(Theme.TEXT);
        String t = title.isEmpty() ? ctx.getString(R.string.PANEL_NO_GAME) : title;
        drawFit(c, t, tx, 318, card.right - 48 - tx);
        if (!status.isEmpty())
            drawStatus(c, tx, 410, statusColor, status, Theme.TEXT_DIM);
        for (Button b : buttons)
            drawButton(c, b, false);
    }

}
