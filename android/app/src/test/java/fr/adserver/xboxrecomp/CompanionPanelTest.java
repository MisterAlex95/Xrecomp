package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.os.Looper;
import android.widget.FrameLayout;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.time.Duration;
import java.util.Collections;

/**
 * D3: the in-game companion panel at 1240 x 1080 (the AYN Thor's bottom screen), rendered to
 * build/screenshots/companion-*.png; multi-touch, slide, cancel, detach, hold-to-quit and dimming checked through a
 * fake sink (what would go to input_overlay.h).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1240dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class CompanionPanelTest {
    private static final int WHITE_MASK = 1 << CompanionPanel.XB_WHITE, BLACK_MASK = 1 << CompanionPanel.XB_BLACK;

    private Activity activity;
    private FrameLayout host;
    private PanelTesting.Sink sink;
    private CompanionPanel panel;
    private PanelTesting touch;

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        host = new FrameLayout(activity);
        activity.setContentView(host);
        sink = new PanelTesting.Sink();
        panel = new CompanionPanel(activity, sink, () -> new int[] { 5994, 412 });
        host.addView(panel, new FrameLayout.LayoutParams(1240, 1080));
        idle(0);
        PanelTesting.layout(panel);
        touch = new PanelTesting(panel);
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private float[] at(int id) {
        return PanelTesting.centre(panel.button(id));
    }

    private void shot(String name) throws Exception {
        File f = TestSupport.screenshot(panel, "companion-" + name);
        assertTrue(f.length() > 0);
    }

    @Test
    public void attachTurnsTheOverlayOn() throws Exception {
        assertEquals(0, sink.buttons);       // overlay(0, 0): active, nothing held
        assertEquals(0, sink.analog);
        idle(1000);                          // the first stats tick
        shot("game");
    }

    @Test
    public void multiTouch() throws Exception {
        touch.press(0, at(CompanionPanel.WHITE));
        assertEquals(0, sink.buttons);
        assertEquals(WHITE_MASK, sink.analog);
        touch.press(1, at(CompanionPanel.START));
        assertEquals(CompanionPanel.XB_START, sink.buttons);
        assertEquals(WHITE_MASK, sink.analog);
        touch.press(2, at(CompanionPanel.BLACK));
        assertEquals(WHITE_MASK | BLACK_MASK, sink.analog);
        shot("game-pressed");
        touch.lift(0);                       // White up, Start and Black still held
        assertEquals(CompanionPanel.XB_START, sink.buttons);
        assertEquals(BLACK_MASK, sink.analog);
        touch.lift(2);
        assertEquals(0, sink.analog);
        touch.lift(1);
        assertEquals(0, sink.buttons);
        assertArrayEquals(new int[] { 0, 0 }, panel.padState());
        assertTrue(sink.actions.isEmpty());
    }

    @Test
    public void slideBetweenButtons() {
        touch.press(0, at(CompanionPanel.BACK));
        assertEquals(CompanionPanel.XB_BACK, sink.buttons);
        touch.move(0, at(CompanionPanel.START));
        assertEquals(CompanionPanel.XB_START, sink.buttons);
        touch.move(0, new float[] { 150, 900 });   // off every button
        assertEquals(0, sink.buttons);
        touch.lift(0);
        assertEquals(0, sink.buttons);
    }

    @Test
    public void cancelReleasesEverything() {
        touch.press(0, at(CompanionPanel.WHITE));
        touch.press(1, at(CompanionPanel.BACK));
        assertEquals(CompanionPanel.XB_BACK, sink.buttons);
        touch.cancel();
        assertEquals(0, sink.buttons);
        assertEquals(0, sink.analog);
        assertArrayEquals(new int[] { 0, 0 }, panel.padState());
    }

    @Test
    public void detachReleasesAndTurnsTheOverlayOff() {
        touch.press(0, at(CompanionPanel.START));
        touch.press(1, at(CompanionPanel.BLACK));
        host.removeView(panel);              // dismiss / onStop
        assertEquals(0, sink.buttons);
        assertEquals(0, sink.analog);
        assertEquals(1, sink.releases);
    }

    @Test
    public void tapsFireOnReleaseOverTheButton() {
        touch.tap(at(CompanionPanel.LAUNCHER));
        assertEquals(Collections.singletonList(CompanionPanel.LAUNCHER), sink.actions);
        // Down on Screenshot, up elsewhere: nothing.
        touch.press(0, at(CompanionPanel.SCREENSHOT));
        touch.move(0, new float[] { 620, 950 });
        touch.lift(0);
        assertEquals(1, sink.actions.size());
    }

    @Test
    public void menuTapOpensTheInGameMenu() {
        touch.tap(at(CompanionPanel.MENU));
        assertEquals(Collections.singletonList(CompanionPanel.MENU), sink.actions);
        assertEquals(0, sink.buttons);       // no pad button with it
    }

    @Test
    public void quitNeedsAHold() throws Exception {
        touch.press(0, at(CompanionPanel.QUIT));
        idle(400);
        shot("game-quit-hold");
        touch.lift(0);
        idle(1000);
        assertTrue("a short press does not quit", sink.actions.isEmpty());
        touch.press(0, at(CompanionPanel.QUIT));
        idle(PanelView.HOLD_MS + 20);
        assertEquals(Collections.singletonList(CompanionPanel.QUIT), sink.actions);
        touch.lift(0);
        assertEquals(1, sink.actions.size());
    }

    @Test
    public void perfToggle() throws Exception {
        touch.tap(at(CompanionPanel.PERF));
        assertTrue(panel.perfOn());
        idle(1000);
        shot("game-perf");
        touch.tap(at(CompanionPanel.PERF));
        assertFalse(panel.perfOn());
    }

    @Test
    public void dimsAfterTenSecondsAndShifts() throws Exception {
        idle(9000);
        assertFalse(panel.isDimmed());
        idle(2000);
        assertTrue(panel.isDimmed());
        shot("game-dimmed");
        // The touch that wakes it fires no action; pad buttons still work while dimmed.
        touch.tap(at(CompanionPanel.LAUNCHER));
        assertFalse(panel.isDimmed());
        assertTrue(sink.actions.isEmpty());
        touch.tap(at(CompanionPanel.LAUNCHER));
        assertEquals(1, sink.actions.size());
        idle(11_000);
        assertTrue(panel.isDimmed());
        touch.press(0, at(CompanionPanel.WHITE));
        assertEquals(WHITE_MASK, sink.analog);
        touch.lift(0);
        // A minute on, the layout has moved by a few pixels (burn-in).
        int before = panel.shiftIndex();
        idle(60_000);
        assertTrue(panel.shiftIndex() != before);
    }

    @Test
    public void flashShowsInTheStrip() throws Exception {
        panel.flash(activity.getString(R.string.PANEL_SHOT_SAVED, "20261110-153000.bmp"));
        shot("game-flash");
        idle(CompanionPanel.FLASH_MS + 1000);
    }
}
