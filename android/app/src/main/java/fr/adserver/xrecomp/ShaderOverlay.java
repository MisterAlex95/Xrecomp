package fr.adserver.xrecomp;

import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.util.function.Supplier;

/**
 * The N64 game's start-up line over SDL's still black surface (docs/android/app-shell.md « Shader warm-up »):
 * « Compilation des shaders… 7/13 » and a bar while the Vulkan RDP builds its pipelines (ShaderProgress decides),
 * gone once the start is over. The UI thread polls the native counter every {@link #POLL_MS} (two atomic loads): the
 * game's threads are never waited on. Not touchable, under the menu and the touch pad.
 */
final class ShaderOverlay {
    static final long POLL_MS = 100;

    private final Activity activity;
    private final Supplier<int[]> source;
    private final ShaderProgress progress = new ShaderProgress();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final FrameLayout root;
    private final LinearLayout box;
    private final TextView text;
    private final ProgressBar bar;
    private final Runnable tick = this::poll;

    ShaderOverlay(Activity a, Supplier<int[]> source) {
        activity = a;
        this.source = source;
        root = new FrameLayout(a);
        root.setClickable(false);
        root.setFocusable(false);
        box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = Theme.S2;
        box.setPadding(Theme.S3, pad, Theme.S3, pad);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xC0000000);
        bg.setCornerRadius(Theme.S2);
        box.setBackground(bg);
        text = Theme.body(a, "");
        text.setGravity(Gravity.CENTER);
        box.addView(text, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                                                        LinearLayout.LayoutParams.WRAP_CONTENT));
        bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(Theme.S6 * 3, LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.topMargin = Theme.S1;
        box.addView(bar, blp);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                                                                   FrameLayout.LayoutParams.WRAP_CONTENT,
                                                                   Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
        lp.bottomMargin = Theme.S6;
        root.addView(box, lp);
        root.setVisibility(View.GONE);
    }

    /** Added to the activity's content (before the touch pad and the menu: under them) and polling. */
    void attach() {
        activity.addContentView(root, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                                                                   FrameLayout.LayoutParams.MATCH_PARENT));
        handler.post(tick);
    }

    void detach() {
        handler.removeCallbacks(tick);
        root.setVisibility(View.GONE);
    }

    private void poll() {
        int[] p;
        try {
            p = source.get();
        } catch (UnsatisfiedLinkError | RuntimeException e) {
            p = null;
        }
        ShaderProgress.Frame f = progress.update(android.os.SystemClock.uptimeMillis(), p);
        show(f);
        if (progress.finished()) {
            if (progress.seen())
                Log.i(LauncherActivity.TAG, "shell: shader compiles at the start over (" + progress.lastDone()
                                            + (progress.lastTotal() > 0 ? "/" + progress.lastTotal() : "")
                                            + " pipelines seen)");
            detach();
            return;
        }
        handler.postDelayed(tick, POLL_MS);
    }

    private void show(ShaderProgress.Frame f) {
        if (f.kind == ShaderProgress.Kind.HIDDEN) {
            root.setVisibility(View.GONE);
            return;
        }
        text.setText(ShaderProgress.text(f, activity.getString(R.string.SHADERS_COMPILING),
                                         activity.getString(R.string.SHADERS_COMPILING_N),
                                         activity.getString(R.string.SHADERS_COMPILING_COUNT)));
        bar.setIndeterminate(f.kind == ShaderProgress.Kind.SPINNER);
        if (f.kind == ShaderProgress.Kind.BAR) {
            bar.setMax(f.total);
            bar.setProgress(f.done);
        }
        root.setVisibility(View.VISIBLE);
    }
}
