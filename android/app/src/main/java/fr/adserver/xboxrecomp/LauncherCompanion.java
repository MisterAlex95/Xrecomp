package fr.adserver.xboxrecomp;

import android.app.Activity;
import android.content.Context;
import android.util.Log;
import android.view.View;

/**
 * The launcher's second screen in a few calls (docs/android/app-shell.md « Dual screen »): owns the ":launcher"
 * process's CompanionScreen and shows LauncherPanel (Home), LibraryPanel (the game library) or PreparingPanel (a
 * Prepare runs). Wiring, in
 * LauncherActivity:
 * <pre>
 *   private LauncherCompanion companion;
 *   // onCreate, after buildStage():
 *   companion = new LauncherCompanion(this, new LauncherPanel.Actions() {
 *       public void play() { launchShell(false); }
 *       public void settings() { go(Screen.SETTINGS); }
 *       public void changeGame() { go(Screen.SOURCE); }
 *   });
 *   protected void onStart() { super.onStart(); companion.start(); }
 *   protected void onStop() { companion.stop(); super.onStop(); }
 *   // where Home is built:      companion.art(titleImage); companion.home(gameTitle(dir), chipText, chipColor, canPlay);
 *   // where Preparing is built: companion.preparing(titleId(), gameTitle(dir));
 * </pre>
 * Without home()/preparing() it shows Home with the remembered title and no status. Hold-to-cancel on the Preparing
 * panel sends PrepareService's cancel itself.
 */
final class LauncherCompanion {
    private final Activity activity;
    private final LauncherPanel.Actions actions;
    private final CompanionScreen screen;
    private String title, status = "", tid;
    private int statusColor = Theme.TEXT_DIM;
    private android.graphics.Bitmap art;
    private boolean canPlay = true, preparing;
    /** The library (LibraryPanel) instead of Home; its actions, the primary button's label (null: greyed). */
    private boolean library;
    private LibraryPanel.Actions libraryActions;
    private String libraryAction;
    /** Home's third shortcut: « Change game », or « Library » when the game library is on. */
    private String changeLabel;

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

    /** Home: the identity card's title and status (e.g. the launcher's chip), Play greyed when it cannot start. */
    void home(String title, String status, int statusColor, boolean canPlay) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        this.canPlay = canPlay;
        boolean was = preparing || library;
        preparing = false;
        library = false;
        View v = screen.content();
        if (!was && v instanceof LauncherPanel)
            ((LauncherPanel)v).set(this.title, this.status, statusColor, canPlay);
        else
            screen.rebuild();
    }

    /** The actions of the library's panel (set once, before library()). */
    void libraryActions(LibraryPanel.Actions a) {
        libraryActions = a;
    }

    /** Home's third shortcut's label (« Library » when the game library is on); null: « Change game ». */
    void changeLabel(String label) {
        if (java.util.Objects.equals(label, changeLabel))
            return;
        changeLabel = label;
        View v = screen.content();
        if (v instanceof LauncherPanel)
            ((LauncherPanel)v).setChangeLabel(label);
    }

    /**
     * The library: the focused game's card (title "" for none), its state, its art (null: none) and the primary
     * button's label (null: greyed).
     */
    void library(String title, String status, int statusColor, android.graphics.Bitmap art, String action) {
        this.title = title == null ? "" : title;
        this.status = status == null ? "" : status;
        this.statusColor = statusColor;
        this.art = art;
        libraryAction = action;
        boolean was = library && !preparing;
        library = true;
        preparing = false;
        View v = screen.content();
        if (was && v instanceof LibraryPanel) {
            ((LibraryPanel)v).setArt(art);
            ((LibraryPanel)v).set(this.title, this.status, statusColor, action);
        } else {
            screen.rebuild();
        }
    }

    boolean libraryForTest() {
        return library && !preparing;
    }

    /** The game's title image for Home's identity card (null: none); shown with the next home(). */
    void art(android.graphics.Bitmap b) {
        art = b;
        View v = screen.content();
        if (v instanceof LauncherPanel)
            ((LauncherPanel)v).setArt(b);
    }

    /** A Prepare runs for titleId: the Preparing panel, which follows status.json and prepare.log on its own. */
    void preparing(String titleId, String title) {
        boolean same = preparing && titleId != null && titleId.equals(tid);
        tid = titleId;
        if (title != null)
            this.title = title;
        preparing = true;
        library = false;
        if (!same)
            screen.rebuild();
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
        if (library) {
            LibraryPanel l = new LibraryPanel(c, libraryActions);
            l.setArt(art);
            l.set(title, status, statusColor, libraryAction);
            return l;
        }
        LauncherPanel p = new LauncherPanel(c, actions);
        if (changeLabel != null)
            p.setChangeLabel(changeLabel);
        p.setArt(art);
        p.set(title, status, statusColor, canPlay);
        return p;
    }
}
