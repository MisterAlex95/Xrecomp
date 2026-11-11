package fr.adserver.xboxrecomp;

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
 * D5: the launcher's second-screen panels (Home, Preparing) at 1240 x 1080, one PNG per state in
 * build/screenshots/companion-launcher-*.png and companion-preparing-*.png.
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
        return new LauncherPanel(activity, new LauncherPanel.Actions() {
            @Override
            public void play() {
                calls.add("play");
            }

            @Override
            public void settings() {
                calls.add("settings");
            }

            @Override
            public void changeGame() {
                calls.add("change");
            }
        });
    }

    @Test
    public void homeStates() throws Exception {
        LauncherPanel p = put(home());
        p.set("Sample Game", activity.getString(R.string.PANEL_READY), Theme.ACCENT, true);
        shot(p, "companion-launcher-ready");
        PanelTesting t = new PanelTesting(p);
        t.tap(PanelTesting.centre(p.button(LauncherPanel.PLAY)));
        t.tap(PanelTesting.centre(p.button(LauncherPanel.SETTINGS)));
        t.tap(PanelTesting.centre(p.button(LauncherPanel.CHANGE)));
        assertEquals(Arrays.asList("play", "settings", "change"), calls);

        p.set("A Game With A Really Long Title That Will Not Fit On One Line Of The Card",
              activity.getString(R.string.CHIP_NEEDS_PREPARE), Theme.WARN, false);
        shot(p, "companion-launcher-needs-prepare");
        t.tap(PanelTesting.centre(p.button(LauncherPanel.PLAY)));
        assertEquals("Play greyed: no call", 3, calls.size());

        p.set("", "", Theme.TEXT_DIM, false);
        shot(p, "companion-launcher-no-game");

        idle(11_000);
        assertTrue(p.isDimmed());
        shot(p, "companion-launcher-dimmed");
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
        List<String> log = new ArrayList<>();

        @Override
        public PrepareProtocol.Status status() {
            return status;
        }

        @Override
        public List<String> logTail(int lines) {
            return log.subList(Math.max(0, log.size() - lines), log.size());
        }
    }

    @Test
    public void preparingStates() throws Exception {
        FakeSource src = new FakeSource();
        src.status = status(PrepareProtocol.RUNNING, "compile", 4, 42, 760);
        src.log.addAll(Arrays.asList("XBA_PROGRESS step=4/5 id=compile pct=41",
                                     "clang -O2 -c gen/func_00012a40.c -o obj/func_00012a40.o",
                                     "clang -O2 -c gen/func_00012b00.c -o obj/func_00012b00.o",
                                     "XBA_PROGRESS step=4/5 id=compile pct=42 msg=func_00012b00.c (1 894 of 4 512)",
                                     "clang -O2 -c gen/func_00012c80_a_rather_long_function_name_to_cut.c -o obj/x.o"));
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
        src.log.add("clang: error: 3 files failed to compile");
        src.log.add("XBA_RESULT error code=CC_FAILED msg=3 files failed to compile");
        idle(1000);
        assertFalse(p.running());
        assertFalse(p.button(PreparingPanel.CANCEL).enabled);
        shot(p, "companion-preparing-failed");
    }

    @Test
    public void tailReadsTheLastLines() throws Exception {
        File f = new File(activity.getFilesDir(), "t.log");
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 2000; i++)
            sb.append("line ").append(i).append('\n');
        TestSupport.write(f, sb.toString() + "\n\n");
        assertEquals(Arrays.asList("line 1997", "line 1998", "line 1999"), PreparingPanel.tail(f, 3));
        assertTrue(PreparingPanel.tail(new File(activity.getFilesDir(), "none"), 3).isEmpty());
    }
}
