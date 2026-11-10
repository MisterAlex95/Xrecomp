package fr.adserver.xboxrecomp;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.util.List;

/**
 * One companion Presentation per process on the second display (docs/android/app-shell.md « Dual screen »): shown
 * from the activity's onStart, dismissed in its onStop, moved or dismissed when displays come and go. The content
 * comes from a factory (the in-game CompanionPanel, the launcher's panels); with « Bottom screen: Black » it is an
 * empty black window, with « Off » nothing is shown. Nothing happens on a single-screen device.
 *
 * Focus: by default (Plan A) the window is FLAG_NOT_FOCUSABLE, so touching it never takes the gamepad away from the
 * activity. With RECOMP_COMPANION_FOCUS=forward (Plan B) it is focusable and hands every key and motion event it gets
 * to the activity ({@link KeyTarget}); the activity then ignores the focus loss while {@link #holdsFocus()}.
 */
final class CompanionScreen implements DisplayManager.DisplayListener {
    /** The panel's view, built in the Presentation's context (the second display's density and size). */
    interface ContentFactory {
        View create(Context displayContext);
    }

    /** Plan B: where the panel's window sends the key and motion events it receives. */
    interface KeyTarget {
        boolean forwardKey(KeyEvent e);

        boolean forwardMotion(MotionEvent e);
    }

    private final Activity activity;
    private final ContentFactory factory;
    private final KeyTarget keys;
    private final String tag;
    private DisplayManager dm;
    private Panel shown;
    private int shownOn = -1;
    private boolean started;
    private boolean forward;
    private String lastDisplays = "";

    CompanionScreen(Activity activity, String tag, ContentFactory factory, KeyTarget keys) {
        this.activity = activity;
        this.tag = tag;
        this.factory = factory;
        this.keys = keys;
    }

    /** From the activity's onStart. */
    void start() {
        if (started)
            return;
        started = true;
        dm = activity.getSystemService(DisplayManager.class);
        if (dm == null)
            return;
        dm.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
        refresh();
    }

    /** From the activity's onStop: the panel goes (its buttons are released by its onDetachedFromWindow). */
    void stop() {
        if (!started)
            return;
        started = false;
        if (dm != null)
            dm.unregisterDisplayListener(this);
        dismiss();
    }

    boolean isShowing() {
        return shown != null && shown.isShowing();
    }

    int shownOn() {
        return isShowing() ? shownOn : -1;
    }

    /** The content view shown, or null (none, or « Black »). */
    View content() {
        return isShowing() ? shown.content : null;
    }

    /** Plan B with the panel up: its window may hold the input focus, which the activity then ignores losing. */
    boolean holdsFocus() {
        return forward && isShowing();
    }

    /** Builds the panel's content again with the factory (its state changed), in the window already shown. */
    void rebuild() {
        if (isShowing() && shown.content != null)
            shown.swap(factory.create(shown.getContext()));
    }

    /** Looks at the displays and the setting again, and shows, moves or dismisses the panel. */
    void refresh() {
        if (!started || dm == null)
            return;
        List<DualScreen.Disp> all = DualScreen.list(dm);
        StringBuilder sb = new StringBuilder();
        for (DualScreen.Disp d : all)
            sb.append("\n  ").append(d);
        String text = sb.toString();
        if (!text.equals(lastDisplays)) {
            lastDisplays = text;
            Log.i(LauncherActivity.TAG, tag + ": displays:" + text);
        }
        DualScreen.Disp target = DualScreen.choose(all, ownDisplayId(activity));
        GameStore.BottomScreen mode = GameStore.bottomScreen(activity);
        DualScreen.Action a = DualScreen.decide(mode, target, shownOn());
        if (a == DualScreen.Action.NONE && isShowing() && (shown.content == null) != (mode == GameStore.BottomScreen.BLACK))
            a = DualScreen.Action.MOVE;   // Panel <-> Black: build it again
        switch (a) {
        case NONE:
            return;
        case DISMISS:
            Log.i(LauncherActivity.TAG, tag + ": companion off (" + (target == null ? "no second display" : "setting Off") + ")");
            dismiss();
            return;
        case MOVE:
            dismiss();
            // fall through
        case SHOW:
            show(target, mode);
            break;
        }
    }

    /** The display the activity is on (the window manager's when the activity cannot say, as under Robolectric). */
    @SuppressWarnings("deprecation")
    static int ownDisplayId(Activity a) {
        Display d;
        try {
            d = a.getDisplay();
        } catch (UnsupportedOperationException e) {
            d = a.getWindowManager().getDefaultDisplay();
        }
        return d == null ? Display.DEFAULT_DISPLAY : d.getDisplayId();
    }

    private void show(DualScreen.Disp target, GameStore.BottomScreen mode) {
        Display d = dm.getDisplay(target.id);
        if (d == null)
            return;
        forward = DualScreen.forwardFocus(activity);
        View content = mode == GameStore.BottomScreen.PANEL ? factory.create(activity.createDisplayContext(d)) : null;
        Panel p = new Panel(activity, d, content, forward ? keys : null);
        try {
            p.show();
        } catch (WindowManager.InvalidDisplayException | WindowManager.BadTokenException e) {
            Log.w(LauncherActivity.TAG, tag + ": cannot show the companion on " + target, e);
            return;
        }
        shown = p;
        shownOn = target.id;
        Log.i(LauncherActivity.TAG, tag + ": companion " + (content == null ? "black" : "panel") + " on " + target
                                    + (forward ? " (focusable, keys forwarded)" : " (not focusable)"));
    }

    private void dismiss() {
        Panel p = shown;
        shown = null;
        shownOn = -1;
        if (p != null && p.isShowing())
            p.dismiss();
    }

    @Override
    public void onDisplayAdded(int displayId) {
        refresh();
    }

    @Override
    public void onDisplayRemoved(int displayId) {
        refresh();
    }

    @Override
    public void onDisplayChanged(int displayId) {
        refresh();
    }

    /** The Presentation: a pure black window holding the content. */
    static final class Panel extends Presentation {
        View content;
        private final KeyTarget keys;
        private FrameLayout root;

        Panel(Context outer, Display display, View content, KeyTarget keys) {
            super(outer, display);
            this.content = content;
            this.keys = keys;
            Window w = getWindow();
            if (w != null) {
                w.setBackgroundDrawable(new ColorDrawable(Color.BLACK));
                if (keys == null)   // Plan A, before show(): never focusable
                    w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            }
        }

        @Override
        protected void onCreate(android.os.Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            root = new FrameLayout(getContext());
            root.setBackgroundColor(Color.BLACK);
            if (content != null)
                root.addView(content, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                                                                   FrameLayout.LayoutParams.MATCH_PARENT));
            setContentView(root);
            Window w = getWindow();
            WindowInsetsController ic = w == null ? null : w.getInsetsController();
            if (ic != null) {
                ic.hide(WindowInsets.Type.systemBars());
                ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }

        /** Puts another content view in place of the current one (the window stays). */
        void swap(View v) {
            if (root == null)
                return;
            root.removeAllViews();
            content = v;
            if (v != null)
                root.addView(v, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                                                             FrameLayout.LayoutParams.MATCH_PARENT));
        }

        @Override
        public boolean dispatchKeyEvent(KeyEvent e) {
            if (keys != null && keys.forwardKey(e))
                return true;
            return super.dispatchKeyEvent(e);
        }

        @Override
        public boolean dispatchGenericMotionEvent(MotionEvent e) {
            if (keys != null && keys.forwardMotion(e))
                return true;
            return super.dispatchGenericMotionEvent(e);
        }
    }
}
