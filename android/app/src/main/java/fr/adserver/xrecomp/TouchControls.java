package fr.adserver.xrecomp;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

/**
 * The on-screen pad in the game (XBA-42): ShellActivity owns one. It puts a {@link TouchPadView} over SDL's surface
 * (under the in-game menu), shows it per {@link TouchSettings#visible} — Auto: while no controller is connected
 * (InputManager's device list, watched) and none has been used since — and sends its state through the
 * {@link OverlayMixer} to input_overlay.h. Never shown while the companion panel is on the second screen.
 */
final class TouchControls implements InputManager.InputDeviceListener {
    /** Whether a game controller is connected now (PadDevices in the game, a fake in the tests). */
    interface Devices {
        boolean any();
    }

    private final Activity activity;
    private final OverlayMixer mixer;
    private final Devices devices;
    final TouchPadView view;
    private TouchSettings settings = new TouchSettings();
    private boolean used, panel, shown, checked;
    /** The in-game menu's toggle for this session: shown or hidden whatever the mode, null: the mode decides. */
    private Boolean forced;
    /** The running game's key (GameSettings), for its own touch layout (XBA-207); null: the console's. */
    private String gameKey;
    private TouchPad.Layout padLayout = TouchPad.Layout.XBOX;
    /** The touch layout editor while it is open over the game, else null. */
    TouchEditor editor;

    TouchControls(Activity a, OverlayMixer mixer, Devices devices) {
        this.activity = a;
        this.mixer = mixer;
        this.devices = devices;
        view = new TouchPadView(a, mixer::touch);
        view.setVisibility(View.GONE);
    }

    /** The console's layout (XBA-158 / XBA-168 / XBA-174): its own buttons, set before start. */
    void layout(TouchPad.Layout l) {
        padLayout = l;
        view.setLayout(l);
    }

    /** The running game's key: its own touch layout wins over the console's (TouchLayout.resolve). */
    void game(String key) {
        gameKey = key;
    }

    /** The player's layout for this console and game (XBA-207), on the pad. */
    private void loadLayout() {
        TouchLayout.Resolved r = TouchLayout.resolve(activity.getFilesDir(), gameKey, padLayout);
        view.setCustom(r.layout);
        if (!r.layout.isEmpty())
            Log.i(LauncherActivity.TAG, "shell: touch layout " + padLayout + (r.perGame ? " (this game's)" : "")
                                        + ", " + r.layout.entries.size() + " control(s) changed");
    }

    /**
     * Opens the touch layout editor over the game (the in-game menu's « Edit touch layout »): the live pad hidden
     * under it, the saved layout on the pad at once. done runs when it closes (the activity lets the game go).
     */
    void edit(Runnable done) {
        if (editor != null)
            return;
        editor = new TouchEditor(activity, padLayout, gameKey, settings, true, saved -> {
            if (saved != null)
                view.setCustom(saved);
            ViewGroup parent = (ViewGroup)editor.getParent();
            if (parent != null)
                parent.removeView(editor);
            editor = null;
            view.setAlpha(1f);
            done.run();
        });
        view.setAlpha(0f);   // the editor draws its own copy
        view.releaseAll();
        activity.addContentView(editor, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                   ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Adds the view (before the in-game menu's, so the menu draws above it). */
    void attach() {
        activity.addContentView(view, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                                 ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** onStart: the settings again (the launcher may have changed them), the device watch, the decision. */
    void start() {
        settings = TouchSettings.load(activity);
        view.apply(settings);
        loadLayout();
        InputManager im = (InputManager)activity.getSystemService(Context.INPUT_SERVICE);
        if (im != null)
            im.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
        update("start");
    }

    /** onStop: hidden, released, the watch off. */
    void stop() {
        InputManager im = (InputManager)activity.getSystemService(Context.INPUT_SERVICE);
        if (im != null)
            im.unregisterInputDeviceListener(this);
        show(false, "stop");
    }

    /** The companion panel came or went on the second screen. */
    void panelShowing(boolean on) {
        if (panel != on) {
            panel = on;
            update(on ? "panel shown" : "panel gone");
        }
    }

    /**
     * A library check's run (XBA-130): never shown, whatever the setting. The check drives pad 1 from its own script
     * (RECOMP_PAD_LIVE) and measures the game as played with a controller; an on-screen pad would add a pad 0 of its
     * own and cover the screen the check grades.
     */
    void checkRun(boolean on) {
        if (checked != on) {
            checked = on;
            update(on ? "library check" : "not a library check");
        }
    }

    /** A key or a motion reached the activity: a controller's hides an Auto pad until no controller is left. */
    void onInput(int source) {
        boolean pad = (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                      || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (pad && !used) {
            used = true;
            update("controller used");
        }
    }

    void onKey(KeyEvent e) {
        onInput(e.getSource());
    }

    void onMotion(MotionEvent e) {
        onInput(e.getSource());
    }

    /** A touch on the screen with no controller connected brings an Auto pad back after a controller was used. */
    void onTouch() {
        if (used && !devices.any()) {
            used = false;
            update("touch, no controller");
        }
    }

    boolean shown() {
        return shown;
    }

    TouchSettings settings() {
        return settings;
    }

    @Override
    public void onInputDeviceAdded(int id) {
        update("device added");
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        if (!devices.any())
            used = false;
        update("device removed");
    }

    @Override
    public void onInputDeviceChanged(int id) {
        update("device changed");
    }

    /** The in-game menu's toggle: shown or hidden for the rest of the session (never with the panel up). */
    void force(boolean on) {
        forced = on;
        update(on ? "menu: shown" : "menu: hidden");
    }

    void update(String why) {
        boolean want = forced != null ? forced && !panel
                                      : TouchSettings.visible(settings.mode, devices.any(), used, panel);
        show(!checked && want, why);
    }

    private void show(boolean on, String why) {
        if (on == shown && (on ? view.getVisibility() == View.VISIBLE : view.getVisibility() != View.VISIBLE))
            return;
        shown = on;
        view.setVisibility(on ? View.VISIBLE : View.GONE);   // hiding releases every finger
        if (on)
            mixer.touch(view.state());   // on: a connected pad 0 even before a touch
        else
            mixer.touchOff();
        Log.i(LauncherActivity.TAG, "shell: touch controls " + (on ? "shown" : "hidden") + " (" + why + ", "
                                    + settings.mode + ")");
    }
}
