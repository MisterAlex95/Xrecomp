package fr.adserver.xboxrecomp;

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
    private boolean used, panel, shown;

    TouchControls(Activity a, OverlayMixer mixer, Devices devices) {
        this.activity = a;
        this.mixer = mixer;
        this.devices = devices;
        view = new TouchPadView(a, mixer::touch);
        view.setVisibility(View.GONE);
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

    void update(String why) {
        show(TouchSettings.visible(settings.mode, devices.any(), used, panel), why);
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
