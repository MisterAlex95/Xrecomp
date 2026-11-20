package fr.adserver.xrecomp;

import android.app.Activity;
import android.content.Context;
import android.util.Log;
import android.view.View;

/**
 * The launcher's second screen in a few calls (docs/android/app-shell.md « Dual screen »): owns the ":launcher"
 * process's CompanionScreen and shows LauncherPanel (Home, and the focused game on the library: art, title, one
 * action; the art alone on the other screens) or PreparingPanel (a Prepare runs). Wiring, in LauncherActivity:
 * <pre>
 *   companion = new LauncherCompanion(this, () -> onPlay());
 *   companion.libraryActions(() -> libraryPrimary(focusedEntry()));
 *   protected void onStart() { super.onStart(); companion.start(); }
 *   protected void onStop() { companion.stop(); super.onStop(); }
 *   // where Home is built:      companion.art(titleImage); companion.home(title, status, color, canPlay, label);
 *   // on the library:           companion.library(title, status, color, art, primaryLabel);
 *   // where Preparing is built: companion.preparing(titleId(), gameTitle(dir));
 *   // on the other screens:     companion.artOnly();
 * </pre>
 * Without home()/preparing() it shows Home with the remembered title. Hold-to-cancel on the Preparing panel sends
 * PrepareService's cancel itself.
 */
final class LauncherCompanion {
    private final Activity activity;
    private final LauncherPanel.Actions actions;
    private final CompanionScreen screen;
    private String title, status = "", tid;
    private int statusColor = Theme.TEXT_DIM;
    private android.graphics.Bitmap art;
    private boolean canPlay = true, preparing, artOnly;
    /** The first start's or Help's drawing instead of the game (GuidePanel). */
    private boolean guide;
    private Schema.Kind guideKind;
    private int guideFound;
    private String guideCaption, guideLeft, guideRight;
    private Runnable guideLeftAction, guideRightAction;
    /** Home's action label (Play, Prepare, Choose…); the library's (null: greyed). */
    private String homeAction, libraryAction;
    /** The focused library game instead of Home, and its action. */
    private boolean library;
    private Runnable libraryPrimary;

    LauncherCompanion(Activity activity, LauncherPanel.Actions actions) {
        this.activity = activity;
        this.actions = actions;
        this.title = GameStore.chosenTitle(activity);
        screen = new CompanionScreen(activity, "launcher", this::create, null);
    }

    void start() {
        screen.start();
    }

    void stop() {
        screen.stop();
    }

    CompanionScreen screen() {
        return screen;
    }

    /** Home: its title and status, Play greyed when it cannot start. */
    void home(String title, String status, int statusColor, boolean canPlay) {
        home(title, status, statusColor, canPlay, null);
    }

    /** Home, with its action's label (null: Play). */
    void home(String title, String status, int statusColor, boolean canPlay, String action) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        this.canPlay = canPlay;
        homeAction = action;
        boolean was = preparing || library || guide;
        guide = false;
        preparing = false;
        library = false;
        artOnly = false;
        View v = screen.content();
        if (!was && v instanceof LauncherPanel)
            fill((LauncherPanel)v);
        else
            screen.rebuild();
    }

    /** The library's action: the focused game's primary one (set once, before library()). */
    void libraryActions(Runnable primary) {
        libraryPrimary = primary;
    }

    /** The library: the focused game (title "" for none), its state, its art (null: none) and action (null: greyed). */
    void library(String title, String status, int statusColor, android.graphics.Bitmap art, String action) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        this.art = art;
        libraryAction = action;
        boolean was = library && !preparing && !guide;
        guide = false;
        library = true;
        preparing = false;
        artOnly = false;
        View v = screen.content();
        if (was && v instanceof LauncherPanel)
            fill((LauncherPanel)v);
        else
            screen.rebuild();
    }

    boolean libraryForTest() {
        return library && !preparing;
    }

    /** Any other screen (Settings, Storage…): the art alone. */
    void artOnly() {
        if (preparing)
            return;   // a running Prepare keeps its panel
        artOnly = true;
        if (guide) {
            guide = false;
            screen.rebuild();
            return;
        }
        View v = screen.content();
        if (v instanceof LauncherPanel)
            ((LauncherPanel)v).setArtOnly(true);
    }

    /** The game's title image (null: none); shown at once, and with the next home(). */
    void art(android.graphics.Bitmap b) {
        art = b;
        View v = screen.content();
        if (v instanceof LauncherPanel)
            ((LauncherPanel)v).setArt(b);
    }

    /** A Prepare runs for titleId: the Preparing panel, which follows status.json on its own. */
    void preparing(String titleId, String title) {
        boolean same = preparing && titleId != null && titleId.equals(tid);
        tid = titleId;
        if (title != null)
            this.title = title;
        preparing = true;
        library = false;
        artOnly = false;
        guide = false;
        if (!same)
            screen.rebuild();
    }

    /**
     * The first start or Help: kind's drawing (count for LOOP), a caption, two touch actions (a null label: none).
     * A running Prepare keeps its panel.
     */
    void guide(Schema.Kind kind, int found, String caption, String left, Runnable leftAction, String right,
               Runnable rightAction) {
        if (preparing)
            return;
        guideKind = kind;
        guideFound = found;
        guideCaption = caption;
        guideLeft = left;
        guideLeftAction = leftAction;
        guideRight = right;
        guideRightAction = rightAction;
        boolean was = guide;
        guide = true;
        library = false;
        artOnly = false;
        View v = screen.content();
        if (was && v instanceof GuidePanel)
            fillGuide((GuidePanel)v);
        else
            screen.rebuild();
    }

    boolean guideForTest() {
        return guide;
    }

    private void fillGuide(GuidePanel p) {
        p.set(guideKind, guideFound, guideCaption, guideLeft, guideLeftAction, guideRight, guideRightAction);
    }

    private void fill(LauncherPanel p) {
        p.setArt(art);
        p.set(title, status, statusColor, library ? libraryAction != null : canPlay);
        p.setAction(library ? libraryAction : homeAction != null ? homeAction : activity.getString(R.string.PLAY));
        if (!library && !canPlay)
            p.button(LauncherPanel.PLAY).enabled = false;
        p.setArtOnly(artOnly);
    }

    private View create(Context c) {
        if (preparing) {
            String id = tid;
            return new PreparingPanel(c, title, PreparingPanel.files(activity, id), () -> {
                Log.i(LauncherActivity.TAG, "launcher: companion: prepare cancel (held)");
                try {
                    activity.startService(PrepareService.cancelIntent(activity, id));
                } catch (RuntimeException e) {
                    Log.w(LauncherActivity.TAG, "launcher: cannot reach the Prepare service", e);
                }
            });
        }
        if (guide) {
            GuidePanel g = new GuidePanel(c);
            fillGuide(g);
            return g;
        }
        LauncherPanel p = new LauncherPanel(c, () -> {
            if (library) {
                if (libraryPrimary != null)
                    libraryPrimary.run();
            } else if (actions != null) {
                actions.play();
            }
        });
        fill(p);
        return p;
    }
}
