package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Looper;
import android.view.InputDevice;
import android.view.View;
import android.widget.FrameLayout;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.time.Duration;

/**
 * XBA-42: the on-screen pad over a game on a 2400 x 1080 phone. Real MotionEvents (several pointers at once) through
 * TouchPadView to a fake overlay (OverlayMixer's sink, what goes to input_overlay.h); shown and hidden by
 * TouchControls (Auto with and without a controller, a controller used, the companion panel, Off, Always); rendered to
 * build/screenshots/touch-*.png over a stand-in 4:3 game frame.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w2400dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TouchControlsTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    /** The overlay as the native side would hold it. */
    static final class Overlay implements OverlayMixer.Sink {
        boolean on;
        int buttons, analog, lt, rt, lx, ly, rx, ry, sends;

        @Override
        public void pad(int buttons, int analogMask, int lt, int rt, int lx, int ly, int rx, int ry) {
            on = true;
            this.buttons = buttons;
            this.analog = analogMask;
            this.lt = lt;
            this.rt = rt;
            this.lx = lx;
            this.ly = ly;
            this.rx = rx;
            this.ry = ry;
            sends++;
        }

        @Override
        public void off() {
            on = false;
            buttons = analog = lt = rt = lx = ly = rx = ry = 0;
        }
    }

    /** A 640 x 480 frame letterboxed as the presenter shows it, so the screenshots show what the pad covers. */
    static final class FakeGame extends View {
        private final Paint p = new Paint();

        FakeGame(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas c) {
            c.drawColor(0xFF000000);
            float h = getHeight(), w = h * 4 / 3, x = (getWidth() - w) / 2;
            for (int i = 0; i < 12; i++) {
                p.setColor(0xFF000000 | (40 + i * 12) << 16 | (70 + i * 10) << 8 | (110 + i * 8));
                c.drawRect(x, h * i / 12, x + w, h * (i + 1) / 12, p);
            }
            p.setColor(0xFF3A5A2A);
            c.drawRect(x, h * 0.62f, x + w, h, p);
        }
    }

    private Activity activity;
    private FrameLayout host;
    private Overlay overlay;
    private OverlayMixer mixer;
    private TouchControls touch;
    private boolean controller;

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        host = new FrameLayout(activity);
        activity.setContentView(host);
        host.addView(new FakeGame(activity), new FrameLayout.LayoutParams(2400, 1080));
        overlay = new Overlay();
        mixer = new OverlayMixer(overlay);
        touch = new TouchControls(activity, mixer, () -> controller);
        host.addView(touch.view, new FrameLayout.LayoutParams(2400, 1080));
        layout();
    }

    private void layout() {
        idle(0);
        host.measure(View.MeasureSpec.makeMeasureSpec(2400, View.MeasureSpec.EXACTLY),
                     View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
        host.layout(0, 0, 2400, 1080);
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private float[] at(int id) {
        TouchPad.Control c = touch.view.pad.control(id);
        return new float[] { c.cx, c.cy };
    }

    private void shot(String name) throws Exception {
        File f = TestSupport.screenshot(host, "touch-" + name);
        assertTrue(f.length() > 0);
    }

    @Test
    public void autoWithoutAControllerAndMultiTouch() throws Exception {
        touch.start();
        layout();
        assertTrue(touch.shown());
        assertTrue("on with nothing held: a connected pad 0", overlay.on);
        assertEquals(0, overlay.buttons);
        shot("1-default");

        PanelTesting fingers = new PanelTesting(touch.view);
        float[] ls = at(TouchPad.LSTICK), rs = at(TouchPad.RSTICK);
        float r = touch.view.pad.control(TouchPad.LSTICK).r;
        fingers.press(0, ls);
        fingers.press(1, rs);
        fingers.press(2, at(TouchPad.A));
        fingers.press(3, at(TouchPad.RT));
        fingers.move(0, new float[] { ls[0] - r, ls[1] });            // left stick full left
        fingers.move(1, new float[] { rs[0], rs[1] + r });            // right stick full down
        assertEquals(-32767, overlay.lx);
        assertEquals(-32767, overlay.ry);
        assertEquals(1 << TouchPad.AN_A, overlay.analog);
        assertEquals(255, overlay.rt);
        shot("2-four-fingers");
        fingers.lift(2);
        assertEquals(0, overlay.analog);
        assertEquals(-32767, overlay.lx);                              // the others still held
        int sends = overlay.sends;
        fingers.move(0, new float[] { ls[0] - r * 1.5f, ls[1] });      // past the edge: same value, nothing sent
        assertEquals(sends, overlay.sends);
        fingers.cancel();
        assertTrue(overlay.on);
        assertEquals(0, overlay.lx);
        assertEquals(0, overlay.rt);
    }

    /** Wave 4: a library check's run (XBA-130) never shows the on-screen pad, even in Auto with no controller. */
    @Test
    public void neverDuringALibraryCheck() {
        touch.checkRun(true);
        touch.start();
        assertFalse(touch.shown());
        assertFalse("no pad 0 of the overlay beside the check's scripted pad 1", overlay.on);
        touch.onTouch();
        touch.onInputDeviceRemoved(1);
        assertFalse(touch.shown());
        touch.checkRun(false);
        assertTrue("an ordinary start: Auto shows it again", touch.shown());
    }

    @Test
    public void hiddenWithAControllerAndWhenOneIsUsed() {
        controller = true;
        touch.start();
        assertFalse(touch.shown());
        assertFalse(overlay.on);
        controller = false;
        touch.onInputDeviceRemoved(7);
        assertTrue(touch.shown());
        // A pad that Android did not list as one sends input: hidden; a touch with no controller brings it back.
        touch.onInput(InputDevice.SOURCE_GAMEPAD);
        assertFalse(touch.shown());
        assertFalse(overlay.on);
        touch.onInput(InputDevice.SOURCE_TOUCHSCREEN);
        assertFalse(touch.shown());
        touch.onTouch();
        assertTrue(touch.shown());
        layout();
        // Released when hidden while held.
        PanelTesting fingers = new PanelTesting(touch.view);
        fingers.press(0, at(TouchPad.B));
        assertEquals(1 << TouchPad.AN_B, overlay.analog);
        controller = true;
        touch.onInputDeviceAdded(8);
        assertFalse(touch.shown());
        assertFalse(overlay.on);
        assertEquals(0, touch.view.state().analogMask);
    }

    @Test
    public void neverWithTheCompanionPanelOffAndAlways() throws Exception {
        touch.start();
        assertTrue(touch.shown());
        touch.panelShowing(true);   // the Thor's bottom screen shows the panel
        assertFalse(touch.shown());
        mixer.panel(TouchPad.XB_START, 0);
        assertTrue("the panel's own buttons still reach the overlay", overlay.on);
        assertEquals(TouchPad.XB_START, overlay.buttons);
        touch.panelShowing(false);
        mixer.panelOff();
        assertTrue(touch.shown());

        TouchSettings s = new TouchSettings();
        s.mode = TouchSettings.Mode.OFF;
        assertTrue(s.save(activity));
        touch.stop();
        touch.start();
        assertFalse(touch.shown());
        assertFalse(overlay.on);

        s.mode = TouchSettings.Mode.ON;
        s.opacity = 100;
        s.size = 125;
        s.save(activity);
        controller = true;
        touch.stop();
        touch.start();
        layout();
        assertTrue("Always: even with a controller", touch.shown());
        shot("3-large-opaque");
        s.opacity = 30;
        s.size = 80;
        s.save(activity);
        touch.stop();
        touch.start();
        layout();
        shot("4-small-faint");
        touch.stop();
        assertFalse(touch.shown());
        assertFalse(overlay.on);
    }
}
