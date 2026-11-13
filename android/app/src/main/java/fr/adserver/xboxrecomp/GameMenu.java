package fr.adserver.xboxrecomp;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.EnumMap;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.LongSupplier;

/**
 * The in-game menu (docs/android/app-shell.md « In-game menu »): a drawer that slides in from the left over the game
 * (Eden / BananaNative style), in a View layer added over SDL's surface with addContentView (the surface is never
 * recreated). The launcher's look: a flush, flat sheet, text-only MenuRows with the gliding focus, its sounds and
 * haptics.
 *
 * <ul>
 *   <li>Opens by: Android Back (not a gamepad's: that one is the pad's Select for the game, as before), the
 *       gamepad's Guide / Mode button (the Xbox pad has none, so no game uses it), Select + Start held
 *       {@link MenuTriggers#COMBO_MS}, a swipe in from the left edge, or the companion panel's « Menu ».</li>
 *   <li>While open the game is held ({@link Host#hold}: the lifecycle's MENU reason, as in the background: the flip
 *       parks, the audio device pauses), and every key and joystick event goes to the drawer only. Opening releases
 *       in the game every key it saw go down and centres every stick it saw move, and the companion's buttons;
 *       closing releases the companion's again. A key whose down came while open acts on its release, so nothing of
 *       it reaches the game.</li>
 *   <li>Items: Resume, Settings (performance overlay, game volume, bottom screen: all live; this game's language,
 *       widescreen and 480p: next start; Share log), Controls, Screenshot, Back to launcher (the game stays alive and held in the
 *       background), Quit game (a second press within {@link #QUIT_ARM_MS} confirms). The header shows the game's
 *       title and « Paused · 0:42 played ».</li>
 * </ul>
 *
 * Everything native goes through {@link Host}, so the JVM tests drive it with a fake.
 */
final class GameMenu {
    /** What the menu needs from the game's activity. */
    interface Host {
        /** Holds (true) or lets go of (false) the game: MenuNative.nativeHold. */
        void hold(boolean on);

        /** Hands a (made-up) key event to SDL as if it had come from the device: releases. */
        void sendKeyToGame(KeyEvent e);

        /** Hands a (made-up) joystick event to SDL: centred sticks. */
        void sendMotionToGame(MotionEvent e);

        /** The menu took a touch gesture the game had seen begin: SDL gets an ACTION_CANCEL. */
        void cancelGameTouch(MotionEvent e);

        /** The companion panel's buttons released (input_overlay: nothing held). */
        void releaseOverlay();

        /** SDL's surface takes the input focus back. */
        void restoreGameFocus();

        /** True when the key event comes from a gamepad SDL reads (its Back is the pad's Select). */
        boolean fromGamepad(KeyEvent e);

        void setVolume(int percent);

        /** { fps x 100, draws }, or null. */
        int[] stats();

        boolean hasSecondScreen();

        void bottomScreenChanged();

        void screenshot();

        void shareLog();

        void backToLauncher();

        void quit();
    }

    static final long ANIM_MS = 200, QUIT_ARM_MS = 4000, FLASH_MS = 3000, PERF_TICK_MS = 500, SESSION_TICK_MS = 1000;
    /** A Back within this long after a swipe opened the menu is the system's back gesture ending: ignored. */
    static final long SWIPE_BACK_GUARD_MS = 600;
    static final long NAV_FIRST_MS = 380, NAV_REPEAT_MS = 130;
    /** The drawer's width on the 1920 x 1080 design grid: a flush sheet against the left edge. */
    static final int DRAWER_W = 560;
    /** How far the panel reaches past the left edge (none: it is flush). */
    static final int HIDDEN = 0;
    static final int ROW_H = Theme.ROW_H;
    static final int VOLUME_STEP = 10;

    enum Page { MAIN, SETTINGS, CONTROLS }

    enum Item { RESUME, SETTINGS, CONTROLS, SCREENSHOT, SHARE_LOG, LAUNCHER, QUIT, PERF, VOLUME, BOTTOM, LANGUAGE,
                WIDESCREEN, P480, BACK }

    private final Activity activity;
    private final Host host;
    private final LongSupplier clock;
    private final long startedAt;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final UiFeedback feedback;

    final FrameLayout root;
    final View scrim;
    final FrameLayout content, drawer;
    final Widgets.Glide glide;
    private final LinearLayout list;
    private final ScrollView scroll;
    private final TextView titleV, sessionV, pageV, perfV;
    private final LinearLayout hints;
    private final FrameLayout flashBox;
    private final Widgets.Chip flashV;
    private final Map<Item, Widgets.MenuRow> rows = new EnumMap<>(Item.class);

    private boolean open, started, animate = true, launcherPending;
    private Page page = Page.MAIN;
    private String openedBy = "";
    private long openedAt, quitArmedAt = -1;
    private boolean perfOn;
    /** The game's title ID for the per-title language (titleId()). */
    private boolean titleIdRead;
    private String titleIdValue;
    private int volume, volumeBeforeMute = 100;
    private ValueAnimator anim;

    /** Keys whose down went to the game and that are still held: released in the game when the menu opens. */
    private final Map<Integer, KeyEvent> held = new HashMap<>();
    /** Keys that went down while the menu was open: only their release acts. */
    private final Set<Integer> downWhileOpen = new HashSet<>();
    /** The last joystick event per device that went to the game: centred when the menu opens. */
    private final Map<Integer, MotionEvent> lastMotion = new HashMap<>();
    private final MenuTriggers.EdgeSwipe swipe;
    private final MenuTriggers.Combo combo = new MenuTriggers.Combo();
    private final Runnable comboFire = () -> {
        if (combo.held())
            open("Select+Start");
    };
    private int navDir;
    private final Runnable navRepeat = new Runnable() {
        @Override
        public void run() {
            if (open && navDir != 0) {
                navStep(navDir);
                handler.postDelayed(this, NAV_REPEAT_MS);
            }
        }
    };
    private final Runnable sessionTick = new Runnable() {
        @Override
        public void run() {
            updateSession();
            if (open)
                handler.postDelayed(this, SESSION_TICK_MS);
        }
    };
    private final Runnable perfTick = new Runnable() {
        @Override
        public void run() {
            updatePerf();
            if (perfOn && started)
                handler.postDelayed(this, PERF_TICK_MS);
        }
    };
    private final Runnable flashEnd = this::hideFlash;
    private final Runnable quitDisarm = this::disarmQuit;

    GameMenu(Activity activity, Host host, LongSupplier clock) {
        this.activity = activity;
        this.host = host;
        this.clock = clock;
        this.startedAt = clock.getAsLong();
        this.feedback = new UiFeedback(activity);
        float density = activity.getResources().getDisplayMetrics().density;
        swipe = new MenuTriggers.EdgeSwipe(20 * density);

        root = new FrameLayout(activity);
        scrim = new View(activity);
        scrim.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                                                 new int[] { 0xA6000000, 0x66000000, 0x4D000000 }));
        scrim.setOnClickListener(v -> {
            feedback.play(UiFeedback.Cue.BACK, v);
            close();
        });
        scrim.setVisibility(View.GONE);
        root.addView(scrim, match());

        content = new FrameLayout(activity);
        content.setClipChildren(false);
        root.addView(new Widgets.Stage(activity, content), match());

        // The drawer: a flat sheet, flush with the left edge (dark under it too, over a bright game).
        drawer = new FrameLayout(activity);
        GradientDrawable under = new GradientDrawable();
        under.setColor(Theme.alpha(Theme.BG_BOTTOM, 0.7f));
        drawer.setBackground(new android.graphics.drawable.LayerDrawable(
                new android.graphics.drawable.Drawable[] { under, new Widgets.Panel(0) }));
        drawer.setClickable(true);   // touches on it never reach the game
        drawer.setPadding(HIDDEN + 64, Theme.S6 - Theme.S2, Theme.S4, Theme.MARGIN_BOTTOM);
        FrameLayout.LayoutParams dlp = new FrameLayout.LayoutParams(DRAWER_W + HIDDEN, ViewGroup.LayoutParams.MATCH_PARENT);
        dlp.leftMargin = -HIDDEN;
        drawer.setVisibility(View.GONE);
        content.addView(drawer, dlp);

        LinearLayout col = new LinearLayout(activity);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setClipChildren(false);
        drawer.addView(col, match());

        // The header: the game's title, then « Paused · 0:42 played ».
        titleV = Theme.oneLine(Theme.text(activity, Theme.T_ROW, Theme.MEDIUM, Theme.TEXT));
        col.addView(titleV, new LinearLayout.LayoutParams(-1, -2));
        sessionV = Theme.oneLine(Theme.text(activity, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM));
        col.addView(sessionV, lpTop(-1, -2, Theme.S2));
        pageV = Theme.oneLine(Theme.label(activity, ""));
        LinearLayout.LayoutParams plp = lpTop(-1, -2, Theme.S5);
        col.addView(pageV, plp);

        scroll = new ScrollView(activity);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        scroll.setClipChildren(false);
        scroll.setClipToPadding(false);
        list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setClipChildren(false);
        scroll.addView(list, new FrameLayout.LayoutParams(-1, -2));
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(-1, 0, 1);
        slp.topMargin = Theme.S5 - Theme.S1;
        slp.leftMargin = -Theme.ROW_INSET;   // the rows' text on the header's edge, their focus plates in the margin
        col.addView(scroll, slp);

        hints = new LinearLayout(activity);
        hints.setOrientation(LinearLayout.HORIZONTAL);
        hints.setGravity(Gravity.CENTER_VERTICAL);
        hints.addView(new Widgets.Hint(activity, "A", s(R.string.MENU_HINT_SELECT), v -> clickFocused()));
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(-2, -2);
        hlp.leftMargin = Theme.S4;
        hints.addView(new Widgets.Hint(activity, "B", s(R.string.MENU_RESUME), v -> back()), hlp);
        col.addView(hints, new LinearLayout.LayoutParams(-1, Theme.FOOTER_H));

        glide = new Widgets.Glide(activity);
        content.addView(glide, match());

        // Over the game, with the menu closed too: the performance figures and short messages.
        perfV = Theme.text(activity, Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT);
        perfV.setBackground(pill());
        perfV.setPadding(22, 12, 22, 12);
        perfV.setVisibility(View.GONE);
        FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        flp.topMargin = 28;
        flp.rightMargin = 32;
        content.addView(perfV, flp);

        flashBox = new FrameLayout(activity);
        flashBox.setBackground(pill());
        flashBox.setPadding(6, 6, 6, 6);
        flashV = new Widgets.Chip(activity);
        flashBox.addView(flashV, new FrameLayout.LayoutParams(-2, -2));
        flashBox.setVisibility(View.GONE);
        FrameLayout.LayoutParams xlp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        xlp.topMargin = 36;
        content.addView(flashBox, xlp);

        root.getViewTreeObserver().addOnGlobalFocusChangeListener((was, now) -> {
            if (!open)
                return;
            if (was != null && now instanceof Widgets.MenuRow)
                feedback.play(UiFeedback.Cue.MOVE, now);
            if (quitArmedAt >= 0 && now != rows.get(Item.QUIT))
                disarmQuit();
        });

        perfOn = GameStore.perfOverlay(activity);
        volume = GameStore.gameVolume(activity);
        if (volume > 0)
            volumeBeforeMute = volume;
        host.setVolume(volume);
        updatePerf();
    }

    GameMenu(Activity activity, Host host) {
        this(activity, host, SystemClock::uptimeMillis);
    }

    /** Puts the layer over the activity's content (SDL's layout): from onCreate, after SDLActivity's. */
    void attach() {
        activity.addContentView(root, match());
    }

    // ---- State -------------------------------------------------------------------------------------------------

    boolean isOpen() {
        return open;
    }

    Page page() {
        return page;
    }

    boolean perfOn() {
        return perfOn;
    }

    int volume() {
        return volume;
    }

    boolean quitArmed() {
        return quitArmedAt >= 0;
    }

    /** Tests: no slide (the drawer is in place at once). */
    void setAnimate(boolean on) {
        animate = on;
    }

    Widgets.MenuRow row(Item item) {
        return rows.get(item);
    }

    // ---- Lifecycle (from the activity) -------------------------------------------------------------------------

    void onStart() {
        started = true;
        handler.removeCallbacks(perfTick);
        if (perfOn)
            handler.post(perfTick);
    }

    /** After SDLActivity.onStop (the background hold is in place by then): a pending Back to launcher lets go. */
    void onStop() {
        started = false;
        handler.removeCallbacks(perfTick);
        settleLauncher();
    }

    void onResume() {
        settleLauncher();
    }

    private void settleLauncher() {
        if (!launcherPending)
            return;
        launcherPending = false;
        host.hold(false);
    }

    void release() {
        handler.removeCallbacksAndMessages(null);
        feedback.release();
        for (MotionEvent m : lastMotion.values())
            m.recycle();
        lastMotion.clear();
    }

    // ---- Open / close ------------------------------------------------------------------------------------------

    void toggle(String why) {
        if (open)
            close();
        else
            open(why);
    }

    void open(String why) {
        if (open)
            return;
        open = true;
        openedBy = why;
        openedAt = clock.getAsLong();
        downWhileOpen.clear();
        Log.i(LauncherActivity.TAG, "shell: menu open (" + why + ")");
        releaseGameInput();
        host.hold(true);
        launcherPending = false;
        disarmQuit();
        flashBox.setVisibility(View.GONE);
        titleV.setText(gameTitle());
        updateSession();
        handler.removeCallbacks(sessionTick);
        handler.postDelayed(sessionTick, SESSION_TICK_MS);
        showPage(Page.MAIN, null);
        scrim.setVisibility(View.VISIBLE);
        drawer.setVisibility(View.VISIBLE);
        slide(true, null);
        feedback.play(UiFeedback.Cue.SELECT, drawer);
        // From a key: the focus on Resume. From a touch (swipe, panel): no focus until a key comes.
        if (!"swipe".equals(why) && !"panel".equals(why))
            focus(rows.get(Item.RESUME));
    }

    void close() {
        close(true);
    }

    private void close(boolean slideOut) {
        if (!open)
            return;
        open = false;
        Log.i(LauncherActivity.TAG, "shell: menu closed");
        stopNav();
        disarmQuit();
        handler.removeCallbacks(sessionTick);
        downWhileOpen.clear();
        host.releaseOverlay();
        host.restoreGameFocus();
        host.hold(false);
        if (slideOut) {
            slide(false, null);
        } else {
            if (anim != null)
                anim.cancel();
            hideNow();
        }
    }

    private void hideNow() {
        drawer.setVisibility(View.GONE);
        scrim.setVisibility(View.GONE);
        drawer.setTranslationX(0);
        scrim.setAlpha(1);
    }

    private void slide(boolean in, Runnable then) {
        if (anim != null)
            anim.cancel();
        if (!animate) {
            if (in) {
                drawer.setTranslationX(0);
                scrim.setAlpha(1);
            } else {
                hideNow();
            }
            if (then != null)
                then.run();
            return;
        }
        float from = in ? 0 : 1, to = in ? 1 : 0;
        float start = drawer.getVisibility() == View.VISIBLE && anim != null ? progress() : from;
        anim = ValueAnimator.ofFloat(start, to);
        anim.setDuration(ANIM_MS);
        anim.setInterpolator(new DecelerateInterpolator(1.6f));
        anim.addUpdateListener(a -> {
            float t = (float)a.getAnimatedValue();
            drawer.setTranslationX(-(DRAWER_W + HIDDEN) * (1 - t));
            scrim.setAlpha(t);
        });
        anim.addListener(new AnimatorListenerAdapter() {
            private boolean cancelled;

            @Override
            public void onAnimationCancel(Animator a) {
                cancelled = true;
            }

            @Override
            public void onAnimationEnd(Animator a) {
                if (cancelled)
                    return;
                if (!in)
                    hideNow();
                if (then != null)
                    then.run();
            }
        });
        drawer.setTranslationX(-(DRAWER_W + HIDDEN) * (1 - start));
        scrim.setAlpha(start);
        anim.start();
    }

    private float progress() {
        return 1 + drawer.getTranslationX() / (DRAWER_W + HIDDEN);
    }

    /**
     * Before the game is held: every key the game saw go down gets its release, every stick it saw move is centred,
     * the companion's buttons are let go, and a Select + Start in progress is forgotten.
     */
    private void releaseGameInput() {
        long now = SystemClock.uptimeMillis();
        for (KeyEvent d : held.values())
            host.sendKeyToGame(KeyEvent.changeAction(KeyEvent.changeTimeRepeat(d, now, 0), KeyEvent.ACTION_UP));
        held.clear();
        for (MotionEvent m : lastMotion.values()) {
            MotionEvent z = centred(m, now);
            host.sendMotionToGame(z);
            z.recycle();
            m.recycle();
        }
        lastMotion.clear();
        combo.reset();
        handler.removeCallbacks(comboFire);
        host.releaseOverlay();
    }

    /** A joystick event from m's device with every axis at rest (sticks centred, triggers and hat released). */
    static MotionEvent centred(MotionEvent m, long now) {
        MotionEvent.PointerProperties[] pp = { new MotionEvent.PointerProperties() };
        pp[0].id = 0;
        pp[0].toolType = MotionEvent.TOOL_TYPE_UNKNOWN;
        MotionEvent.PointerCoords[] pc = { new MotionEvent.PointerCoords() };
        return MotionEvent.obtain(m.getDownTime(), now, MotionEvent.ACTION_MOVE, 1, pp, pc, 0, 0, 1f, 1f,
                                  m.getDeviceId(), 0, m.getSource(), 0);
    }

    // ---- Input -------------------------------------------------------------------------------------------------

    /**
     * Every key event, before SDL (ShellActivity.dispatchKeyEvent). True: the menu took it and SDL must not see it;
     * false: on to SDL as usual.
     */
    boolean dispatchKey(KeyEvent e) {
        int k = e.getKeyCode();
        int action = e.getAction();
        boolean down = action == KeyEvent.ACTION_DOWN, up = action == KeyEvent.ACTION_UP;
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE)
            return false;   // the system's
        if (!open) {
            boolean back = k == KeyEvent.KEYCODE_BACK && !host.fromGamepad(e);
            if (back || k == KeyEvent.KEYCODE_BUTTON_MODE) {
                if (down && e.getRepeatCount() == 0)
                    open(back ? "Back" : "Guide");
                return true;   // down and up: never the game's
            }
            if (down) {
                if (e.getRepeatCount() == 0)
                    held.put(k, new KeyEvent(e));   // the framework recycles e after dispatch
            } else if (up) {
                held.remove(k);
            }
            // A gamepad's Back is its Select for SDL (and so for the combo).
            int ck = k == KeyEvent.KEYCODE_BACK ? KeyEvent.KEYCODE_BUTTON_SELECT : k;
            if (e.getRepeatCount() == 0 && combo.key(ck, down, clock.getAsLong())) {
                handler.removeCallbacks(comboFire);
                if (combo.held())
                    handler.postDelayed(comboFire, MenuTriggers.COMBO_MS);
            }
            return false;
        }
        if (down) {
            if (k == KeyEvent.KEYCODE_BACK && "swipe".equals(openedBy)
                && clock.getAsLong() - openedAt < SWIPE_BACK_GUARD_MS)
                return true;   // the system's back gesture finishing the swipe that opened the menu
            if (e.getRepeatCount() == 0)
                downWhileOpen.add(k);
            keyDown(k);
        } else if (up) {
            if (downWhileOpen.remove(k) && !e.isCanceled())
                keyUp(k);
        }
        return true;
    }

    private void keyDown(int k) {
        switch (k) {
        case KeyEvent.KEYCODE_DPAD_UP:
            move(-1);
            break;
        case KeyEvent.KEYCODE_DPAD_DOWN:
            move(1);
            break;
        case KeyEvent.KEYCODE_DPAD_LEFT:
            stepFocused(-1);
            break;
        case KeyEvent.KEYCODE_DPAD_RIGHT:
            stepFocused(1);
            break;
        default:
            break;
        }
    }

    private void keyUp(int k) {
        switch (k) {
        case KeyEvent.KEYCODE_BUTTON_A:
        case KeyEvent.KEYCODE_DPAD_CENTER:
        case KeyEvent.KEYCODE_ENTER:
        case KeyEvent.KEYCODE_NUMPAD_ENTER:
        case KeyEvent.KEYCODE_SPACE:
            clickFocused();
            break;
        case KeyEvent.KEYCODE_BUTTON_B:
        case KeyEvent.KEYCODE_BACK:
        case KeyEvent.KEYCODE_ESCAPE:
            back();
            break;
        case KeyEvent.KEYCODE_BUTTON_START:
        case KeyEvent.KEYCODE_BUTTON_MODE:
            feedback.play(UiFeedback.Cue.BACK, drawer);
            close();
            break;
        default:
            break;
        }
    }

    /**
     * Every generic motion event (ShellActivity.dispatchGenericMotionEvent). Closed: joystick events go on to the game
     * and are remembered (to centre them at the next open). Open: the hat and the left stick move the focus (and
     * step a setting sideways), and nothing reaches the game.
     */
    boolean dispatchMotion(MotionEvent e) {
        boolean joy = (e.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0;
        if (!open) {
            if (joy) {
                MotionEvent old = lastMotion.put(e.getDeviceId(), MotionEvent.obtain(e));
                if (old != null)
                    old.recycle();
            }
            return false;
        }
        if (joy && e.getActionMasked() == MotionEvent.ACTION_MOVE)
            joystick(e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y),
                     e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y));
        return true;
    }

    /** 1 up, 2 down, 3 left, 4 right; a direction steps at once, then repeats while held. */
    void joystick(float hatX, float hatY, float x, float y) {
        float vx = Math.abs(hatX) > Math.abs(x) ? hatX : x, vy = Math.abs(hatY) > Math.abs(y) ? hatY : y;
        int d = 0;
        if (Math.abs(vy) >= 0.5f && Math.abs(vy) >= Math.abs(vx))
            d = vy < 0 ? 1 : 2;
        else if (Math.abs(vx) >= 0.5f)
            d = vx < 0 ? 3 : 4;
        if (d == navDir)
            return;
        handler.removeCallbacks(navRepeat);
        navDir = d;
        if (d != 0) {
            navStep(d);
            handler.postDelayed(navRepeat, NAV_FIRST_MS);
        }
    }

    private void stopNav() {
        navDir = 0;
        handler.removeCallbacks(navRepeat);
    }

    private void navStep(int d) {
        if (d == 1)
            move(-1);
        else if (d == 2)
            move(1);
        else
            stepFocused(d == 3 ? -1 : 1);
    }

    /**
     * Every touch event (ShellActivity.dispatchTouchEvent). Closed: a swipe in from the left edge opens the menu (the
     * game's touch is cancelled); anything else goes on. Open: the drawer's views take it (a tap on the dimmed game
     * closes the menu).
     */
    boolean dispatchTouch(MotionEvent e) {
        if (open && !swipe.taken())
            return false;
        MenuTriggers.Touch t = swipe.onTouch(e.getActionMasked(), e.getX(), e.getY(), root.getWidth());
        if (t == MenuTriggers.Touch.TAKE) {
            host.cancelGameTouch(e);
            open("swipe");
            return true;
        }
        return t == MenuTriggers.Touch.CONSUME;
    }

    // ---- Navigation --------------------------------------------------------------------------------------------

    private List<Widgets.MenuRow> focusables() {
        List<Widgets.MenuRow> out = new ArrayList<>();
        for (int i = 0; i < list.getChildCount(); i++) {
            View v = list.getChildAt(i);
            if (v instanceof Widgets.MenuRow && v.getVisibility() == View.VISIBLE)
                out.add((Widgets.MenuRow)v);
        }
        return out;
    }

    private Widgets.MenuRow focused() {
        View f = root.findFocus();
        return f instanceof Widgets.MenuRow ? (Widgets.MenuRow)f : null;
    }

    Widgets.MenuRow focusedRow() {
        return focused();
    }

    private void focus(View v) {
        if (v != null && !v.requestFocusFromTouch())
            v.requestFocus();
    }

    private void move(int dir) {
        List<Widgets.MenuRow> fs = focusables();
        if (fs.isEmpty())
            return;
        Widgets.MenuRow f = focused();
        int i = fs.indexOf(f);
        if (i < 0) {
            focus(fs.get(0));
            return;
        }
        int j = Math.max(0, Math.min(fs.size() - 1, i + dir));
        if (j != i)
            focus(fs.get(j));
    }

    private void stepFocused(int dir) {
        Widgets.MenuRow f = focused();
        if (f != null && f.cycler != null)
            f.cycler.step(dir);
    }

    private void clickFocused() {
        Widgets.MenuRow f = focused();
        if (f == null) {
            move(0);
            return;
        }
        f.performClick();
    }

    private void back() {
        if (!open)
            return;
        feedback.play(UiFeedback.Cue.BACK, drawer);
        if (page != Page.MAIN) {
            Item from = page == Page.SETTINGS ? Item.SETTINGS : Item.CONTROLS;
            showPage(Page.MAIN, from);
            return;
        }
        close();
    }

    // ---- Pages -------------------------------------------------------------------------------------------------

    /** The hints under the list: A Select, and B Resume on the main page, B Back on the others. */
    private void setBackHint(int label) {
        if (hints.getChildCount() > 1)
            hints.removeViewAt(1);
        LinearLayout.LayoutParams hlp = new LinearLayout.LayoutParams(-2, -2);
        hlp.leftMargin = Theme.S4;
        hints.addView(new Widgets.Hint(activity, "B", s(label), v -> back()), hlp);
    }

    /** The B hint's label (tests). */
    String backHintForTest() {
        return ((Widgets.Hint)hints.getChildAt(1)).labelForTest();
    }

    private void showPage(Page p, Item focusItem) {
        boolean hadFocus = focused() != null;
        page = p;
        list.removeAllViews();
        rows.clear();
        disarmQuit();
        setBackHint(p == Page.MAIN ? R.string.MENU_RESUME : R.string.MENU_HINT_BACK);
        switch (p) {
        case MAIN:
            pageV.setVisibility(View.GONE);
            row(Item.RESUME, s(R.string.MENU_RESUME));
            row(Item.SETTINGS, s(R.string.MENU_SETTINGS));
            row(Item.CONTROLS, s(R.string.MENU_CONTROLS));
            row(Item.SCREENSHOT, s(R.string.MENU_SCREENSHOT));
            row(Item.LAUNCHER, s(R.string.MENU_LAUNCHER));
            list.addView(new View(activity), new LinearLayout.LayoutParams(-1, Theme.S3));   // Quit stands apart
            row(Item.QUIT, s(R.string.MENU_QUIT));
            break;
        case SETTINGS: {
            pageV.setText(s(R.string.MENU_SETTINGS));
            pageV.setVisibility(View.VISIBLE);
            Widgets.MenuRow r = row(Item.PERF, s(R.string.MENU_PERF));
            r.setChoices(new String[] { s(R.string.MENU_ON), s(R.string.MENU_OFF) });
            r.cycler = dir -> setPerf(!perfOn);
            r = row(Item.VOLUME, s(R.string.MENU_VOLUME));
            r.setChoices(new String[] { volumeLabel(100), s(R.string.MENU_MUTED) });
            r.cycler = dir -> setVolume(volume + dir * VOLUME_STEP);
            if (host.hasSecondScreen()) {
                r = row(Item.BOTTOM, s(R.string.MENU_BOTTOM));
                r.setChoices(new String[] { s(R.string.VAL_PANEL), s(R.string.VAL_BLACK), s(R.string.MENU_SCREEN_OFF) });
                r.cycler = this::stepBottom;
            }
            if (titleId() != null) {
                r = row(Item.LANGUAGE, s(R.string.MENU_LANGUAGE));
                String[] labels = new String[GameLanguage.MENU_ORDER.length + 1];
                labels[0] = s(R.string.MENU_LANGUAGE_FOLLOW);
                for (int i = 0; i < GameLanguage.MENU_ORDER.length; i++)
                    labels[i + 1] = GameLanguage.NAMES[GameLanguage.MENU_ORDER[i]];
                r.setChoices(labels);
                r.cycler = this::stepLanguage;
                // XBA-134: this game's widescreen and 480p, as in Settings or its own.
                String[] video = { s(R.string.MENU_VIDEO_FOLLOW), s(R.string.MENU_ON), s(R.string.MENU_OFF) };
                r = row(Item.WIDESCREEN, s(R.string.MENU_WIDESCREEN));
                r.setChoices(video);
                r.cycler = dir -> stepVideo(GameVideo.Setting.WIDESCREEN, dir);
                r = row(Item.P480, s(R.string.MENU_480P));
                r.setChoices(video);
                r.cycler = dir -> stepVideo(GameVideo.Setting.P480, dir);
            }
            row(Item.SHARE_LOG, s(R.string.MENU_SHARE_LOG));
            row(Item.BACK, s(R.string.MENU_BACK));
            updateValues();
            break;
        }
        case CONTROLS:
            pageV.setText(s(R.string.MENU_CONTROLS));
            pageV.setVisibility(View.VISIBLE);
            int[][] kv = { { R.string.CTL_MENU, R.string.CTL_MENU_X }, { R.string.CTL_COMBO, R.string.CTL_COMBO_X },
                           { R.string.CTL_FACE, R.string.CTL_FACE_X },
                           { R.string.CTL_L1, R.string.CTL_L1_X }, { R.string.CTL_R1, R.string.CTL_R1_X },
                           { R.string.CTL_TRIGGERS, R.string.CTL_TRIGGERS_X },
                           { R.string.CTL_START, R.string.CTL_START_X }, { R.string.CTL_SELECT, R.string.CTL_SELECT_X },
                           { R.string.CTL_DPAD, R.string.CTL_DPAD_X }, { R.string.CTL_STICKS, R.string.CTL_STICKS_X } };
            // XBA-131: the player's remapping (the launcher's Controls), first: each changed control, then the usual list.
            PadMapping map = PadMapping.load(RecompConfig.load(GameStore.externalFilesDir(activity)));
            if (!map.isDefault()) {
                kvRow(s(R.string.MENU_CTL_REMAPPED),
                      activity.getString(R.string.MENU_CTL_REMAPPED_X, map.changedCount()));
                for (PadMapping.Control c : PadMapping.Control.values()) {
                    if (map.get(c) != c.def)
                        kvRow(s(ControlsLabels.source(map.get(c))), s(ControlsLabels.control(c)));
                }
            }
            for (int[] r : kv)
                kvRow(s(r[0]), s(r[1]));
            row(Item.BACK, s(R.string.MENU_BACK));
            break;
        }
        scroll.scrollTo(0, 0);
        if (hadFocus || focusItem != null) {
            Widgets.MenuRow target = focusItem != null ? rows.get(focusItem) : null;
            if (target == null) {
                List<Widgets.MenuRow> fs = focusables();
                target = p == Page.CONTROLS ? rows.get(Item.BACK) : fs.isEmpty() ? null : fs.get(0);
            }
            if (hadFocus || p == Page.MAIN)
                focus(target);
        }
    }

    private Widgets.MenuRow row(Item item, String title) {
        Widgets.MenuRow r = new Widgets.MenuRow(activity);
        r.set(title, null);
        r.onPress = () -> feedback.play(UiFeedback.Cue.SELECT, r);
        r.setOnClickListener(v -> onItem(item));
        rows.put(item, r);
        list.addView(r, new LinearLayout.LayoutParams(-1, ROW_H));
        return r;
    }

    /** A line of the Controls table: the device's control, what the game gets. */
    private void kvRow(String key, String value) {
        LinearLayout l = new LinearLayout(activity);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        l.setPadding(Theme.ROW_INSET, Theme.S1, Theme.ROW_INSET, Theme.S1);
        l.setMinimumHeight(60);
        TextView k = Theme.oneLine(Theme.text(activity, Theme.T_BODY, Theme.MEDIUM, Theme.TEXT, key));
        l.addView(k, new LinearLayout.LayoutParams(0, -2, 1));
        TextView v = Theme.text(activity, Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM, value);
        v.setMaxLines(2);
        v.setEllipsize(android.text.TextUtils.TruncateAt.END);
        v.setLineSpacing(0, 1.15f);
        v.setGravity(Gravity.END);
        l.addView(v, new LinearLayout.LayoutParams(0, -2, 1.6f));
        list.addView(l, new LinearLayout.LayoutParams(-1, -2));
    }

    private void onItem(Item item) {
        switch (item) {
        case RESUME:
            close();
            break;
        case SETTINGS:
            showPage(Page.SETTINGS, Item.PERF);
            break;
        case CONTROLS:
            showPage(Page.CONTROLS, Item.BACK);
            break;
        case SCREENSHOT:
            // Closed first: the player sees the game the picture is of (the BMP is the game's frame alone anyway).
            Log.i(LauncherActivity.TAG, "shell: menu: screenshot");
            close();
            handler.postDelayed(host::screenshot, animate ? ANIM_MS + 20 : 0);
            break;
        case SHARE_LOG:
            Log.i(LauncherActivity.TAG, "shell: menu: share log");
            host.shareLog();
            break;
        case LAUNCHER:
            backToLauncher();
            break;
        case QUIT:
            quitPressed();
            break;
        case PERF:
            setPerf(!perfOn);
            break;
        case VOLUME:
            setVolume(volume == 0 ? volumeBeforeMute : 0);
            break;
        case BOTTOM:
            stepBottom(1);
            break;
        case LANGUAGE:
            stepLanguage(1);
            break;
        case WIDESCREEN:
            stepVideo(GameVideo.Setting.WIDESCREEN, 1);
            break;
        case P480:
            stepVideo(GameVideo.Setting.P480, 1);
            break;
        case BACK:
            back();
            break;
        }
    }

    /**
     * The game stays alive (the runtime maps its memory once per process: a new start would mean a new process) and
     * held: the drawer goes at once, the MENU hold stays until onStop, where the background hold has taken over, so
     * the game never runs in between. The launcher's Play brings this task back, and the game goes on.
     */
    private void backToLauncher() {
        Log.i(LauncherActivity.TAG, "shell: menu: back to launcher (the game waits)");
        open = false;
        stopNav();
        disarmQuit();
        handler.removeCallbacks(sessionTick);
        downWhileOpen.clear();
        if (anim != null)
            anim.cancel();
        hideNow();
        host.releaseOverlay();
        host.restoreGameFocus();
        launcherPending = true;
        host.backToLauncher();
    }

    private void quitPressed() {
        long now = clock.getAsLong();
        if (quitArmedAt >= 0 && now - quitArmedAt <= QUIT_ARM_MS) {
            Log.i(LauncherActivity.TAG, "shell: menu: quit (confirmed)");
            host.releaseOverlay();
            host.quit();
            return;
        }
        quitArmedAt = now;
        Widgets.MenuRow r = rows.get(Item.QUIT);
        if (r != null) {
            r.setTitle(s(R.string.MENU_QUIT_CONFIRM));
            r.setSubtitle(s(R.string.MENU_QUIT_CONFIRM_SUB));
        }
        feedback.play(UiFeedback.Cue.ERROR, r);
        handler.removeCallbacks(quitDisarm);
        handler.postDelayed(quitDisarm, QUIT_ARM_MS);
    }

    private void disarmQuit() {
        if (quitArmedAt < 0)
            return;
        quitArmedAt = -1;
        handler.removeCallbacks(quitDisarm);
        Widgets.MenuRow r = rows.get(Item.QUIT);
        if (r != null) {
            r.setTitle(s(R.string.MENU_QUIT));
            r.setSubtitle(null);
        }
    }

    // ---- Settings page -----------------------------------------------------------------------------------------

    void setPerf(boolean on) {
        perfOn = on;
        GameStore.setPerfOverlay(activity, on);
        Log.i(LauncherActivity.TAG, "shell: menu: performance overlay " + (on ? "on" : "off"));
        handler.removeCallbacks(perfTick);
        updatePerf();
        if (on && started)
            handler.postDelayed(perfTick, PERF_TICK_MS);
        updateValues();
    }

    void setVolume(int v) {
        v = Math.max(0, Math.min(100, v));
        if (v == volume)
            return;
        volume = v;
        if (v > 0)
            volumeBeforeMute = v;
        host.setVolume(v);
        GameStore.setGameVolume(activity, v);
        updateValues();
    }

    private void stepBottom(int dir) {
        GameStore.BottomScreen[] all = GameStore.BottomScreen.values();
        GameStore.BottomScreen cur = GameStore.bottomScreen(activity);
        GameStore.BottomScreen next = all[((cur.ordinal() + dir) % all.length + all.length) % all.length];
        GameStore.setBottomScreen(activity, next);
        Log.i(LauncherActivity.TAG, "shell: menu: bottom screen " + next);
        host.bottomScreenChanged();
        updateValues();
    }

    /** The game's title ID, read once from its XBE; null hides the Language row (nothing to key the choice on). */
    private String titleId() {
        if (!titleIdRead) {
            titleIdRead = true;
            titleIdValue = GameLanguage.currentTitleId(activity);
        }
        return titleIdValue;
    }

    /** The Language row's position: 0 = as in Settings, then GameLanguage.MENU_ORDER. */
    private int languagePos() {
        int idx = GameLanguage.indexOf(GameLanguage.titleChoice(activity, titleId()));
        for (int i = 0; i < GameLanguage.MENU_ORDER.length; i++) {
            if (GameLanguage.MENU_ORDER[i] == idx)
                return i + 1;
        }
        return 0;
    }

    /** This game's language (XBA-77): remembered per title ID, applied by ShellActivity at the next start. */
    private void stepLanguage(int dir) {
        int n = GameLanguage.MENU_ORDER.length + 1;
        int pos = ((languagePos() + dir) % n + n) % n;
        String code = pos == 0 ? null : GameLanguage.CODES[GameLanguage.MENU_ORDER[pos - 1]];
        GameLanguage.setTitleChoice(activity, titleId(), code);
        Log.i(LauncherActivity.TAG, "shell: menu: language " + (code == null ? "as in Settings" : code)
                                    + " for " + titleId() + " (next start)");
        updateValues();
    }

    /** This game's widescreen or 480p (XBA-134): remembered per title ID, applied by ShellActivity at next start. */
    private void stepVideo(GameVideo.Setting which, int dir) {
        Boolean on = GameVideo.next(GameVideo.titleChoice(activity, titleId(), which), dir);
        GameVideo.setTitleChoice(activity, titleId(), which, on);
        Log.i(LauncherActivity.TAG, "shell: menu: " + which.env + " "
                                    + (on == null ? "as in Settings" : on ? "on" : "off")
                                    + " for " + titleId() + " (next start)");
        updateValues();
    }

    private String videoLabel(GameVideo.Setting which) {
        Boolean on = GameVideo.titleChoice(activity, titleId(), which);
        return s(on == null ? R.string.MENU_VIDEO_FOLLOW : on ? R.string.MENU_ON : R.string.MENU_OFF);
    }

    private void updateValues() {
        Widgets.MenuRow r = rows.get(Item.PERF);
        if (r != null)
            r.setValue(s(perfOn ? R.string.MENU_ON : R.string.MENU_OFF));
        r = rows.get(Item.VOLUME);
        if (r != null)
            r.setValue(volume == 0 ? s(R.string.MENU_MUTED) : volumeLabel(volume));
        r = rows.get(Item.BOTTOM);
        if (r != null) {
            GameStore.BottomScreen b = GameStore.bottomScreen(activity);
            int label = b == GameStore.BottomScreen.PANEL ? R.string.VAL_PANEL
                        : b == GameStore.BottomScreen.BLACK ? R.string.VAL_BLACK : R.string.MENU_SCREEN_OFF;
            r.setValue(s(label));
        }
        r = rows.get(Item.LANGUAGE);
        if (r != null) {
            int pos = languagePos();
            r.setValue(pos == 0 ? s(R.string.MENU_LANGUAGE_FOLLOW)
                                : GameLanguage.NAMES[GameLanguage.MENU_ORDER[pos - 1]]);
        }
        r = rows.get(Item.WIDESCREEN);
        if (r != null)
            r.setValue(videoLabel(GameVideo.Setting.WIDESCREEN));
        r = rows.get(Item.P480);
        if (r != null)
            r.setValue(videoLabel(GameVideo.Setting.P480));
    }

    private String volumeLabel(int v) {
        return activity.getString(R.string.MENU_PERCENT, v);
    }

    // ---- Header, overlay, messages -----------------------------------------------------------------------------

    private String gameTitle() {
        String t = GameStore.chosenTitle(activity);
        return t == null || t.isEmpty() ? s(R.string.app_name) : t;
    }

    private void updateSession() {
        sessionV.setText(activity.getString(R.string.MENU_STATUS, hoursMinutes(clock.getAsLong() - startedAt)));
    }

    /** « 0:42 », « 1:05 »: hours and minutes. */
    static String hoursMinutes(long ms) {
        long m = Math.max(0, ms) / 60_000;
        return String.format(Locale.ROOT, "%d:%02d", m / 60, m % 60);
    }

    private void updatePerf() {
        if (!perfOn) {
            perfV.setVisibility(View.GONE);
            return;
        }
        int[] st = host.stats();
        String text;
        if (st == null || st[0] <= 0) {
            text = s(R.string.PANEL_FPS_NONE);
        } else {
            float fps = st[0] / 100f;
            text = activity.getString(R.string.PANEL_FPS, String.format(Locale.ROOT, "%.1f", fps),
                                      String.format(Locale.ROOT, "%.1f", 1000f / fps));
        }
        perfV.setText(text);
        perfV.setVisibility(View.VISIBLE);
    }

    String perfText() {
        return perfV.getVisibility() == View.VISIBLE ? perfV.getText().toString() : null;
    }

    /** A short message at the top of the screen (« Saved … »), for FLASH_MS, over the game or the menu. */
    void flash(String text, boolean ok) {
        flashV.set(text, ok ? Theme.OK : Theme.WARN);
        flashBox.setVisibility(View.VISIBLE);
        handler.removeCallbacks(flashEnd);
        handler.postDelayed(flashEnd, FLASH_MS);
        if (ok)
            feedback.play(UiFeedback.Cue.DONE, null);
    }

    private void hideFlash() {
        flashBox.setVisibility(View.GONE);
    }

    String flashText() {
        return flashBox.getVisibility() == View.VISIBLE ? flashV.label() : null;
    }

    // ---- Helpers -----------------------------------------------------------------------------------------------

    private String s(int id) {
        return activity.getString(id);
    }

    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }

    private static LinearLayout.LayoutParams lpTop(int w, int h, int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.topMargin = top;
        return p;
    }

    private static GradientDrawable pill() {
        GradientDrawable g = new GradientDrawable();
        g.setColor(Theme.alpha(Theme.BG_BOTTOM, 0.8f));
        g.setCornerRadius(30);
        return g;
    }
}
