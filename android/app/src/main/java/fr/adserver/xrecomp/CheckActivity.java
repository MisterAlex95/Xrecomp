package fr.adserver.xrecomp;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * « Check my library » (XBA-130, docs/android/app-shell.md « Library check »), in the launcher's process. Three
 * states: what a check would do (how many games, how many to prepare, how long; Start for all of them or for the
 * selected game alone), a check under way (the title, its step, the results so far; Pause, Stop, Share report), and
 * the summary once it is done. The check itself runs in CheckService; this screen reads check.json twice a second.
 *
 * <p>While a check runs this screen must be in front for it to start the next game or Prepare (Android lets an app
 * start an activity or a foreground service only from its screen): it keeps the screen on, dimmed. The launcher sends
 * the player here while a check is active (LauncherActivity.onCreate), also when a game leaves by itself.
 */
public class CheckActivity extends Activity {
    static final String TAG = "xboxrecomp";
    static final String EXTRA_ONLY = "fr.adserver.xrecomp.CHECK_ONLY";
    static final long POLL_MS = 500, CONFIRM_MS = 5000;
    /** A Prepare's rough length for the estimate (docs/android/prepare.md « Measurements »: 8-10 min on a Thor). */
    static final int PREPARE_MIN = 12;

    private static volatile boolean shown;

    private final Handler poller = new Handler(Looper.getMainLooper());
    private final Runnable pollTick = new Runnable() {
        @Override
        public void run() {
            refresh();
            poller.postDelayed(this, POLL_MS);
        }
    };

    private LinearLayout body, buttons, results;
    private TextView headline, status, detail, note;
    private Widgets.Meter meter;
    private String lastKey = "";
    private long armedAt;
    private String armed = "";
    /** The plan the intro shows (titles to check, the selected game). */
    private List<LibraryCheck.Title> planAll = new ArrayList<>();
    private LibraryCheck.Title selected;

    /** The check screen is in front (CheckPlatform.canStart). */
    static boolean isShown() {
        return shown;
    }

    static void setShownForTest(boolean on) {
        shown = on;
    }

    static Intent intent(Context c) {
        return new Intent(c, CheckActivity.class);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(Theme.BG_BOTTOM));
        WindowManager.LayoutParams lp = w.getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        w.setAttributes(lp);
        Theme.apply(Theme.Colors.STOCK);
        build();
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        shown = true;
        hideBars();
        LibraryCheck.State s = LibraryCheck.read(getFilesDir());
        if (s != null && s.active() && CheckService.driver() == null)
            startService(false);   // left by a process that died (app crash, reboot): carry on
        poller.removeCallbacks(pollTick);
        poller.post(pollTick);
    }

    @Override
    protected void onPause() {
        shown = false;
        poller.removeCallbacks(pollTick);
        super.onPause();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        refresh();
    }

    private void hideBars() {
        getWindow().getDecorView();
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    // ---- Layout -----------------------------------------------------------------------------------------------------

    private void build() {
        FrameLayout host = new FrameLayout(this);
        Widgets.Backdrop backdrop = new Widgets.Backdrop();
        host.setBackground(backdrop);
        body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(Theme.MARGIN_X, Theme.MARGIN_TOP, Theme.MARGIN_X, Theme.MARGIN_BOTTOM);
        host.addView(body, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                        ViewGroup.LayoutParams.MATCH_PARENT));
        TextView top = Theme.text(this, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, getString(R.string.CHECK_TITLE));
        body.addView(top, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, Theme.TOP_BAR_H));
        top.setGravity(Gravity.CENTER_VERTICAL);
        headline = Theme.text(this, Theme.T_DISPLAY, Theme.MEDIUM, Theme.TEXT);
        body.addView(headline, wrap(Theme.S4));
        status = Theme.oneLine(Theme.text(this, Theme.T_ROW, Theme.REGULAR, Theme.TEXT_DIM));
        body.addView(status, wrap(Theme.S2));
        meter = new Widgets.Meter(this);
        LinearLayout.LayoutParams mp = new LinearLayout.LayoutParams(1200, 12);
        mp.topMargin = Theme.S3;
        body.addView(meter, mp);
        detail = Theme.body(this, "");
        body.addView(detail, wrap(Theme.S3));
        results = new LinearLayout(this);
        results.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1);
        rp.topMargin = Theme.S3;
        body.addView(results, rp);
        buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        body.addView(buttons, wrap(Theme.S3));
        note = Theme.oneLine(Theme.text(this, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM));
        body.addView(note, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Theme.FOOTER_H));
        note.setGravity(Gravity.CENTER_VERTICAL);
        setContentView(new Widgets.Stage(this, host));
    }

    private static LinearLayout.LayoutParams wrap(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                    ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = top;
        return p;
    }

    private Widgets.PillButton button(String label, boolean primary, Runnable action) {
        Widgets.PillButton b = new Widgets.PillButton(this).set(null, label, primary);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                                                                    Theme.BUTTON_H);
        p.rightMargin = Theme.S3;
        buttons.addView(b, p);
        return b;
    }

    // ---- What it shows ----------------------------------------------------------------------------------------

    /** Reads check.json and rebuilds what changed (the buttons only when the screen's kind changes). */
    void refresh() {
        LibraryCheck.State s = LibraryCheck.read(getFilesDir());
        String kind = s == null || (!s.active() && !shownDone(s)) ? "intro" : s.active() ? "run:" + s.state : "done";
        if (!kind.equals(lastKey)) {
            lastKey = kind;
            buttons.removeAllViews();
            if (kind.equals("intro"))
                buildIntro();
            else if (kind.startsWith("run"))
                buildRun(s);
            else
                buildDone();
            if (buttons.getChildCount() > 0)
                buttons.getChildAt(0).requestFocus();
        }
        if (kind.equals("intro"))
            return;
        int n = s.titles.size();
        headline.setText(s.active() ? getString(R.string.CHECK_PROGRESS, Math.min(s.index + 1, n), n)
                                    : getString(R.string.CHECK_DONE_TITLE, n));
        status.setText(statusLine(this, s));
        status.setTextColor(LibraryCheck.PAUSED.equals(s.state) ? Theme.WARN : Theme.TEXT_DIM);
        meter.set(n == 0 ? 0 : s.graded() / (float)n, Theme.ACCENT);
        detail.setText(summaryLine(this, s));
        fillResults(s);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (s.active()) {   // overnight: the screen stays on, dimmed
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (lp.screenBrightness != 0.1f) {
                lp.screenBrightness = 0.1f;
                getWindow().setAttributes(lp);
            }
        }
    }

    /** A check that ended is shown until the player leaves it (then the intro again). */
    private boolean shownDone(LibraryCheck.State s) {
        return (LibraryCheck.DONE.equals(s.state) || LibraryCheck.STOPPED.equals(s.state))
               && !getIntent().getBooleanExtra("intro", false);
    }

    private void buildIntro() {
        planAll = plan(null);
        int prep = 0, boot = 0;
        File f = getFilesDir();
        String tk = Toolchain.toolkitDigest(this);
        for (LibraryCheck.Title t : planAll) {
            if (LibraryCheck.needsPrepare(f, t.tid, t.xbeSha256, tk))
                prep++;
            if (t.bootable())
                boot++;
        }
        String sel = Library.read(Library.gamesFile(f)).selected;
        selected = null;
        for (LibraryCheck.Title t : planAll) {
            if (t.tid.equals(sel))
                selected = t;
        }
        headline.setText(getResources().getQuantityString(R.plurals.CHECK_GAMES, planAll.size(), planAll.size()));
        long min = LibraryCheck.estimateMinutes(prep, boot, PREPARE_MIN);
        status.setText(getString(R.string.CHECK_PLAN, prep, boot, formatMinutes(min)));
        status.setTextColor(Theme.TEXT_DIM);
        meter.set(0, Theme.ACCENT);
        detail.setText(getString(R.string.CHECK_INTRO));
        results.removeAllViews();
        int notHere = planAll.size() - boot;
        if (notHere > 0)
            addResultLine(getResources().getQuantityString(R.plurals.CHECK_NOT_IMPORTED, notHere, notHere), "",
                          Theme.TEXT_DIM);
        if (planAll.isEmpty()) {
            detail.setText(getString(R.string.CHECK_NOTHING));
        } else {
            button(getString(R.string.CHECK_START_ALL, planAll.size()), true, () -> onStartPressed(null));
            if (selected != null && planAll.size() > 1)
                button(getString(R.string.CHECK_START_ONE, selected.title), false, () -> onStartPressed(selected.tid));
        }
        if (LibraryCheck.resultsFile(f).isFile())
            button(getString(R.string.CHECK_SHARE), false, this::shareReport);
        setNote(new CheckPlatform(this, null).charging() ? "" : getString(R.string.CHECK_NOT_CHARGING));
    }

    private void buildRun(LibraryCheck.State s) {
        boolean paused = LibraryCheck.PAUSED.equals(s.state);
        button(getString(paused ? R.string.CHECK_CONTINUE : R.string.CHECK_PAUSE), true,
               () -> send(paused ? CheckService.ACTION_RESUME : CheckService.ACTION_PAUSE));
        button(getString(R.string.CHECK_STOP), false, this::onStopPressed);
        button(getString(R.string.CHECK_SHARE), false, this::shareReport);
        setNote(getString(R.string.CHECK_KEEP_HERE));
    }

    private void buildDone() {
        button(getString(R.string.CHECK_SHARE), true, this::shareReport);
        button(getString(R.string.CHECK_AGAIN), false, () -> {
            getIntent().putExtra("intro", true);
            lastKey = "";
            refresh();
        });
        button(getString(R.string.CHECK_DONE), false, this::leave);
        setNote(getString(R.string.CHECK_REPORT_NOTE));
    }

    private void fillResults(LibraryCheck.State s) {
        results.removeAllViews();
        // The latest graded titles first, at most eight lines.
        int shownLines = 0;
        for (int i = s.titles.size() - 1; i >= 0 && shownLines < 8; i--) {
            LibraryCheck.Title t = s.titles.get(i);
            if (!LibraryCheck.GRADED.equals(t.phase))
                continue;
            addResultLine(t.title + " · " + (t.stage.isEmpty() ? "?" : t.stage),
                          t.blocker, stageColor(t.stage));
            shownLines++;
        }
    }

    private void addResultLine(String left, String right, int color) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        TextView a = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.MEDIUM, color, left));
        row.addView(a, new LinearLayout.LayoutParams(right.isEmpty() ? ViewGroup.LayoutParams.MATCH_PARENT : 620,
                                                     ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView b = Theme.oneLine(Theme.text(this, Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM, right));
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        bp.leftMargin = Theme.S3;
        row.addView(b, bp);
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                     ViewGroup.LayoutParams.WRAP_CONTENT);
        rp.bottomMargin = Theme.S2;
        results.addView(row, rp);
    }

    static int stageColor(String stage) {
        switch (stage) {
        case "sustained":
        case "responsive":
            return Theme.OK;
        case "renders":
        case "boots":
            return Theme.TEXT;
        case "prepares":
            return Theme.WARN;
        default:
            return Theme.ERROR;
        }
    }

    private void setNote(String s) {
        note.setText(s);
    }

    /** « 2 h 40 min », « 25 min ». */
    static String formatMinutes(long m) {
        if (m < 60)
            return m + " min";
        return String.format(Locale.ROOT, "%d h %02d", m / 60, m % 60);
    }

    /** One line on where the check is (the screen and the notification). */
    static String statusLine(Context c, LibraryCheck.State s) {
        if (s == null)
            return "";
        if (LibraryCheck.DONE.equals(s.state))
            return c.getString(R.string.CHECK_STATUS_DONE);
        if (LibraryCheck.STOPPED.equals(s.state))
            return c.getString(R.string.CHECK_STATUS_STOPPED, s.graded(), s.titles.size());
        LibraryCheck.Title t = s.current();
        String title = t == null ? "" : t.title;
        if (LibraryCheck.PAUSED.equals(s.state)) {
            switch (s.reason) {
            case LibraryCheck.AWAY:
                return c.getString(R.string.CHECK_PAUSED_AWAY);
            case LibraryCheck.HOT:
                return c.getString(R.string.CHECK_PAUSED_HOT);
            case LibraryCheck.BATTERY:
                return c.getString(R.string.CHECK_PAUSED_BATTERY);
            default:
                return c.getString(R.string.CHECK_PAUSED);
            }
        }
        if (t == null)
            return "";
        switch (t.phase) {
        case LibraryCheck.PREPARING: {
            PrepareProtocol.Status st = PrepareProtocol.readStatus(PrepareService.statusFile(c, t.tid));
            String step = st == null ? "" : PrepareService.stepName(c, st.id)
                                            + String.format(Locale.getDefault(), " · %d%%", st.pct);
            return c.getString(R.string.CHECK_STATUS_PREPARING, title, step);
        }
        case LibraryCheck.BOOTING: {
            long el = s.run == null ? 0 : Math.max(0, (System.currentTimeMillis() - s.run.launchedAt) / 1000);
            return c.getString(R.string.CHECK_STATUS_BOOTING, title, Math.min(el, s.seconds), s.seconds);
        }
        default:
            return c.getString(R.string.CHECK_STATUS_NEXT, title);
        }
    }

    /** « sustained 2 · responsive 1 · ... »: the stages so far, best first. */
    static String summaryLine(Context c, LibraryCheck.State s) {
        Map<String, Integer> m = s.counts();
        StringBuilder b = new StringBuilder();
        for (int i = LibraryCheck.STAGES.length - 1; i >= 0; i--) {
            int n = m.get(LibraryCheck.STAGES[i]);
            if (n == 0)
                continue;
            b.append(b.length() == 0 ? "" : " · ").append(LibraryCheck.STAGES[i]).append(' ').append(n);
        }
        return b.length() == 0 ? c.getString(R.string.CHECK_NO_RESULT_YET) : b.toString();
    }

    // ---- Actions ----------------------------------------------------------------------------------------------------

    /** The library's titles to check (the launcher's records and scan, as the grid shows them). */
    private List<LibraryCheck.Title> plan(String only) {
        File f = getFilesDir();
        LibraryScreen.Env env = new LibraryScreen.Env();
        env.toolchain = Toolchain.available(this);
        env.accessible = Library.folderPath(this) == null || Environment.isExternalStorageManager();
        env.queue = PrepareProtocol.readQueue(PrepareProtocol.queueFile(f));
        List<LibraryScreen.Entry> entries = LibraryScreen.entries(f, Library.read(Library.gamesFile(f)),
                                                                  LibraryIndex.read(Library.indexFile(f)), env);
        return LibraryCheck.plan(entries, f, only);
    }

    /** Start: not charging, the first press warns and a second within CONFIRM_MS starts anyway. */
    private void onStartPressed(String only) {
        if (!new CheckPlatform(this, null).charging() && !confirm("start")) {
            setNote(getString(R.string.CHECK_NOT_CHARGING_CONFIRM));
            note.setTextColor(Theme.WARN);
            return;
        }
        List<LibraryCheck.Title> titles = only == null ? planAll : plan(only);
        if (titles.isEmpty())
            return;
        LibraryCheck.State s = LibraryCheck.start(titles, System.currentTimeMillis());
        try {
            LibraryCheck.write(getFilesDir(), s);
        } catch (java.io.IOException e) {
            Log.w(TAG, "check: cannot write check.json", e);
            setNote(String.valueOf(e.getMessage()));
            return;
        }
        Log.i(TAG, "check: started, " + titles.size() + " titles" + (only == null ? "" : " (" + only + " only)"));
        note.setTextColor(Theme.TEXT_DIM);
        getIntent().removeExtra("intro");
        startService(true);
        lastKey = "";
        refresh();
    }

    private void onStopPressed() {
        if (!confirm("stop")) {
            setNote(getString(R.string.CHECK_STOP_CONFIRM));
            return;
        }
        send(CheckService.ACTION_STOP);
    }

    private boolean confirm(String what) {
        long now = SystemClock.uptimeMillis();
        if (what.equals(armed) && now - armedAt <= CONFIRM_MS) {
            armed = "";
            return true;
        }
        armed = what;
        armedAt = now;
        return false;
    }

    private void startService(boolean fresh) {
        try {
            startForegroundService(CheckService.intent(this, CheckService.ACTION_START)
                                           .putExtra(CheckService.EXTRA_FRESH, fresh));
        } catch (RuntimeException e) {
            Log.w(TAG, "check: cannot start the service", e);
        }
    }

    private void send(String action) {
        try {
            startService(CheckService.intent(this, action));
        } catch (RuntimeException e) {
            Log.w(TAG, "check: cannot reach the service", e);
        }
    }

    /** « Share report »: compat-results.jsonl through the share sheet (LogProvider, read-only grant). */
    void shareReport() {
        File f = LibraryCheck.resultsFile(getFilesDir());
        if (!f.isFile()) {
            setNote(getString(R.string.CHECK_NO_REPORT));
            return;
        }
        android.net.Uri uri = LogProvider.reportUri(this);
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.setClipData(android.content.ClipData.newRawUri(LibraryCheck.RESULTS, uri));
        try {
            startActivity(Intent.createChooser(send, getString(R.string.CHECK_SHARE)));
        } catch (ActivityNotFoundException e) {
            setNote(getString(R.string.SHARE_LOG_NO_APP));
        }
    }

    private void leave() {
        startActivity(new Intent(this, LauncherActivity.class).setAction(LauncherActivity.ACTION_SHOW_LAUNCHER)
                              .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
        finish();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (e.getAction() == KeyEvent.ACTION_UP) {
            switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_BUTTON_A: {
                View v = getCurrentFocus();
                if (v != null)
                    v.performClick();
                return true;
            }
            case KeyEvent.KEYCODE_BUTTON_Y:
                shareReport();
                return true;
            default:
                break;
            }
        }
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_A || e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_Y)
            return true;
        return super.dispatchKeyEvent(e);
    }

    @Override
    public void onBackPressed() {
        LibraryCheck.State s = LibraryCheck.read(getFilesDir());
        if (s != null && s.active()) {   // the check needs this screen: say how to leave it
            setNote(getString(R.string.CHECK_BACK_RUNNING));
            return;
        }
        leave();
    }

    // ---- Tests ------------------------------------------------------------------------------------------------------

    View contentForTest() {
        return body;
    }

    LinearLayout buttonsForTest() {
        return buttons;
    }

    String noteForTest() {
        return note.getText().toString();
    }
}
