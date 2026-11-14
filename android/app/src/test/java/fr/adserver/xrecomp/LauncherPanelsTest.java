package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.os.Looper;
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
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * D5: the launcher's second-screen panels (Home, Preparing, the game library) at 1240 x 1080, one PNG per state in
 * build/screenshots/companion-launcher-*.png, companion-preparing-*.png and companion-library-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1240dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherPanelsTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private Activity activity;
    private FrameLayout host;
    private final List<String> calls = new ArrayList<>();

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        host = new FrameLayout(activity);
        activity.setContentView(host);
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private <T extends View> T put(T v) {
        host.removeAllViews();
        host.addView(v, new FrameLayout.LayoutParams(1240, 1080));
        idle(0);
        PanelTesting.layout(v);
        return v;
    }

    private void shot(View v, String name) throws Exception {
        PanelTesting.layout(v);
        File f = TestSupport.screenshot(v, name);
        assertTrue(f.length() > 0);
    }

    private LauncherPanel home() {
        return new LauncherPanel(activity, () -> calls.add("play"));
    }

    /** Calm: art, title and one action; the status is not drawn, the action greys when it cannot run. */
    @Test
    public void homeStates() throws Exception {
        LauncherPanel p = put(home());
        p.set("Sample Game", activity.getString(R.string.PANEL_READY), Theme.OK, true);
        shot(p, "companion-launcher-ready");
        assertEquals("one action only", 1, p.buttons.size());
        PanelTesting t = new PanelTesting(p);
        t.tap(PanelTesting.centre(p.button(LauncherPanel.PLAY)));
        assertEquals(Arrays.asList("play"), calls);

        p.set("A Game With A Really Long Title That Will Not Fit On One Line Of The Card",
              activity.getString(R.string.CHIP_NEEDS_PREPARE), Theme.WARN, true);
        p.setAction(activity.getString(R.string.PREPARE));
        assertEquals(activity.getString(R.string.PREPARE), p.actionForTest());
        shot(p, "companion-launcher-needs-prepare");
        p.set("Sample Game", activity.getString(R.string.STATE_WRONG), Theme.ERROR, false);
        t.tap(PanelTesting.centre(p.button(LauncherPanel.PLAY)));
        assertEquals("greyed: no call", 1, calls.size());

        p.set("", "", Theme.TEXT_DIM, false);
        shot(p, "companion-launcher-no-game");

        idle(11_000);
        assertTrue(p.isDimmed());
        shot(p, "companion-launcher-dimmed");
    }

    /** A game's theme: its colours, and its title image above the title. */
    @Test
    public void homeThemedWithArt() throws Exception {
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture()), wh);
        Theme.apply(new Theme.Colors(GamePalette.build(GamePalette.analyse(px, wh[0], wh[1]), 212).colors));
        LauncherPanel p = put(home());
        p.setArt(android.graphics.Bitmap.createBitmap(px, wh[0], wh[1], android.graphics.Bitmap.Config.ARGB_8888));
        p.set("Sample Game", activity.getString(R.string.PANEL_READY), Theme.OK, true);
        assertTrue(p.hasArtForTest());
        shot(p, "companion-launcher-themed");
        p.set("A Game With A Really Long Title That Will Not Fit On One Line Of The Card",
              activity.getString(R.string.CHIP_NEEDS_PREPARE), Theme.WARN, false);
        shot(p, "companion-launcher-themed-long");
        // The other screens: the art alone, nothing to press.
        p.setArtOnly(true);
        assertTrue(p.artOnlyForTest());
        assertEquals(null, p.actionForTest());
        shot(p, "companion-launcher-art-only");
        p.setArt(null);
        assertFalse(p.hasArtForTest());
    }

    /** The library: the same panel, the focused game's primary action as its one button. */
    @Test
    public void libraryStates() throws Exception {
        int[] wh = new int[2];
        int[] px = TitleArt.decodeXpr(TestSupport.xpr0(0x0C, 128, 128, TestSupport.dxt1Picture()), wh);
        LauncherPanel p = put(home());
        p.setArt(android.graphics.Bitmap.createBitmap(px, wh[0], wh[1], android.graphics.Bitmap.Config.ARGB_8888));
        p.set("Sample Game", activity.getString(R.string.STATE_READY), Theme.OK, true);
        p.setAction(activity.getString(R.string.PLAY));
        shot(p, "companion-library-ready");
        p.setArt(null);
        p.set("A Game With A Really Long Title That Will Not Fit On One Line Of The Card",
              activity.getString(R.string.LIB_CHIP_NOT_IMPORTED), Theme.TEXT_DIM, true);
        p.setAction(activity.getString(R.string.LIB_IMPORT));
        assertEquals(activity.getString(R.string.LIB_IMPORT), p.button(LauncherPanel.PLAY).label);
        shot(p, "companion-library-not-imported");
        p.setAction(null);
        PanelTesting t = new PanelTesting(p);
        t.tap(PanelTesting.centre(p.button(LauncherPanel.PLAY)));
        assertTrue("no action: greyed, no call", calls.isEmpty());
    }

    private static PrepareProtocol.Status status(String state, String id, int step, int pct, long eta) {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = state;
        s.id = id;
        s.step = step;
        s.pct = pct;
        s.eta = eta;
        return s;
    }

    private static final class FakeSource implements PreparingPanel.Source {
        PrepareProtocol.Status status;

        @Override
        public PrepareProtocol.Status status() {
            return status;
        }
    }

    @Test
    public void preparingStates() throws Exception {
        FakeSource src = new FakeSource();
        src.status = status(PrepareProtocol.RUNNING, "compile", 4, 42, 760);
        int[] cancels = { 0 };
        PreparingPanel p = put(new PreparingPanel(activity, "Sample Game", src, () -> cancels[0]++));
        idle(1000);
        shot(p, "companion-preparing-running");

        // Hold to cancel: a short press does nothing, a hold sends it once.
        PanelTesting t = new PanelTesting(p);
        float[] c = PanelTesting.centre(p.button(PreparingPanel.CANCEL));
        t.press(0, c);
        idle(300);
        t.lift(0);
        assertEquals(0, cancels[0]);
        t.press(0, c);
        idle(400);
        shot(p, "companion-preparing-cancel-hold");
        idle(500);
        t.lift(0);
        assertEquals(1, cancels[0]);
        assertTrue(p.cancelSentForTest());
        shot(p, "companion-preparing-canceling");
        t.press(0, c);
        idle(1000);
        t.lift(0);
        assertEquals("only once", 1, cancels[0]);

        // After 30 s without a touch: the percentage alone.
        idle(25_000);
        assertFalse(p.isDimmed());
        idle(6_000);
        assertTrue(p.isDimmed());
        shot(p, "companion-preparing-dimmed");
    }

    @Test
    public void preparingInstallAndFailed() throws Exception {
        FakeSource src = new FakeSource();
        src.status = status(PrepareProtocol.RUNNING, PrepareProtocol.STEP_INSTALL, 0, 63, -1);
        PreparingPanel p = put(new PreparingPanel(activity, "Sample Game", src, null));
        idle(1000);
        shot(p, "companion-preparing-install");
        src.status = status(PrepareProtocol.FAILED, "compile", 4, 37, -1);
        idle(1000);
        assertFalse(p.running());
        assertFalse(p.button(PreparingPanel.CANCEL).enabled);
        shot(p, "companion-preparing-failed");
    }
}
