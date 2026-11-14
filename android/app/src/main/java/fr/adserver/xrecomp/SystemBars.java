package fr.adserver.xrecomp;

import android.app.Activity;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * The game full screen (XBA-99): status bar and navigation (the gesture pill) hidden, a swipe from an edge shows them
 * for a moment, the window drawn under the display cutout. On the AYN Thor the bars stayed and SDL's surface was
 * 1920x970 instead of 1920x1080: SDLActivity hides them with the old systemUiVisibility flags, which a focus change
 * (the companion Presentation on the second display takes and gives it back) clears. Applied in onCreate, onResume,
 * on every focus gain and after the companion screen shows; WindowInsetsController (API 30, our minSdk) keeps the
 * state across focus changes, and applying it again is free.
 */
final class SystemBars {
    private SystemBars() {
    }

    static void hide(Activity a) {
        hide(a.getWindow());
    }

    static void hide(Window w) {
        if (w == null)
            return;
        w.setDecorFitsSystemWindows(false);
        WindowManager.LayoutParams lp = w.getAttributes();
        if (lp.layoutInDisplayCutoutMode != WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES) {
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(lp);
        }
        View decor = w.peekDecorView();
        WindowInsetsController c = decor != null ? decor.getWindowInsetsController() : w.getInsetsController();
        if (c == null)
            return;
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        c.hide(WindowInsets.Type.systemBars());
    }
}
