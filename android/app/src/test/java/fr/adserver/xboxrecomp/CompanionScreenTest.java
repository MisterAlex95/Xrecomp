package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.content.Context;
import android.os.Looper;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowDisplayManager;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;

/**
 * D4: the companion Presentation's lifecycle with Robolectric displays: shown when a second display is there (or
 * comes), dismissed when it goes, with « Bottom screen: Off » and in onStop; Black shows an empty window; Plan A is
 * not focusable, Plan B (RECOMP_COMPANION_FOCUS=forward) is; the panel's buttons are released when it goes.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class CompanionScreenTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private ActivityController<Activity> controller;
    private Activity activity;
    private PanelTesting.Sink sink;
    private CompanionScreen screen;

    @Before
    public void setUp() {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        controller = Robolectric.buildActivity(Activity.class).setup();
        activity = controller.get();
        sink = new PanelTesting.Sink();
        screen = new CompanionScreen(activity, "test",
                                     c -> new CompanionPanel(c, sink, () -> new int[] { 6000, 100 }), null);
    }

    private static void idle() {
        shadowOf(Looper.getMainLooper()).idle();
    }

    private static int addBottom() {
        int id = ShadowDisplayManager.addDisplay("w1240dp-h1080dp-mdpi");
        idle();
        return id;
    }

    @Test
    public void singleScreenShowsNothing() {
        screen.start();
        idle();
        assertFalse(screen.isShowing());
        assertFalse(DualScreen.available(activity, Display.DEFAULT_DISPLAY));
        screen.stop();
    }

    @Test
    public void showsOnTheSecondDisplayAndFollowsIt() {
        int id = addBottom();
        assertTrue(DualScreen.available(activity, Display.DEFAULT_DISPLAY));
        screen.start();
        idle();
        assertTrue(screen.isShowing());
        assertEquals(id, screen.shownOn());
        View v = screen.content();
        assertTrue(v instanceof CompanionPanel);
        assertEquals(0, sink.buttons);   // attached: overlay on
        // Plan A: never focusable.
        WindowManager.LayoutParams lp = ((CompanionScreen.Panel)panelDialog()).getWindow().getAttributes();
        assertTrue((lp.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) != 0);

        // The display goes: dismissed, buttons released.
        new PanelTesting(v).press(0, PanelTesting.centre(((CompanionPanel)v).button(CompanionPanel.START)));
        assertEquals(CompanionPanel.XB_START, sink.buttons);
        ShadowDisplayManager.removeDisplay(id);
        idle();
        assertFalse(screen.isShowing());
        assertEquals(0, sink.buttons);
        assertEquals(1, sink.releases);

        // It comes back: shown again.
        int id2 = addBottom();
        assertTrue(screen.isShowing());
        assertEquals(id2, screen.shownOn());
        screen.stop();
    }

    @Test
    public void addedWhileRunning() {
        screen.start();
        idle();
        assertFalse(screen.isShowing());
        int id = addBottom();
        assertTrue(screen.isShowing());
        assertEquals(id, screen.shownOn());
        screen.stop();
    }

    @Test
    public void stopDismissesAndReleases() {
        addBottom();
        screen.start();
        idle();
        CompanionPanel p = (CompanionPanel)screen.content();
        PanelTesting t = new PanelTesting(p);
        t.press(0, PanelTesting.centre(p.button(CompanionPanel.WHITE)));
        t.press(1, PanelTesting.centre(p.button(CompanionPanel.BACK)));
        assertEquals(CompanionPanel.XB_BACK, sink.buttons);
        screen.stop();   // ShellActivity.onStop
        idle();
        assertFalse(screen.isShowing());
        assertEquals(0, sink.buttons);
        assertEquals(0, sink.analog);
        assertEquals(1, sink.releases);
        screen.start();  // onStart again: back
        idle();
        assertTrue(screen.isShowing());
        screen.stop();
    }

    @Test
    public void bottomScreenSetting() {
        addBottom();
        Context app = RuntimeEnvironment.getApplication();
        assertEquals(GameStore.BottomScreen.PANEL, GameStore.bottomScreen(app));
        assertTrue(GameStore.setBottomScreen(app, GameStore.BottomScreen.OFF));
        assertEquals(GameStore.BottomScreen.OFF, GameStore.bottomScreen(app));
        screen.start();
        idle();
        assertFalse("Off: nothing", screen.isShowing());

        GameStore.setBottomScreen(app, GameStore.BottomScreen.BLACK);
        screen.refresh();
        assertTrue(screen.isShowing());
        assertNull("Black: an empty window", screen.content());

        GameStore.setBottomScreen(app, GameStore.BottomScreen.PANEL);
        screen.refresh();
        assertTrue(screen.content() instanceof CompanionPanel);

        GameStore.setBottomScreen(app, GameStore.BottomScreen.OFF);
        screen.refresh();
        assertFalse(screen.isShowing());
        screen.stop();
    }

    @Test
    public void planBIsFocusableAndForwards() throws Exception {
        File ext = GameStore.externalFilesDir(activity);
        assertNotNull(ext);
        TestSupport.write(new File(ext, RecompConfig.FILE_NAME), DualScreen.CFG_FOCUS + "=forward\n");
        int[] forwarded = { 0 };
        CompanionScreen s = new CompanionScreen(activity, "test", c -> new CompanionPanel(c, sink, null),
                                                new CompanionScreen.KeyTarget() {
            @Override
            public boolean forwardKey(android.view.KeyEvent e) {
                forwarded[0]++;
                return true;
            }

            @Override
            public boolean forwardMotion(android.view.MotionEvent e) {
                return false;
            }
        });
        addBottom();
        s.start();
        idle();
        assertTrue(s.isShowing());
        assertTrue(s.holdsFocus());
        screen = s;
        CompanionScreen.Panel d = (CompanionScreen.Panel)panelDialog();
        assertFalse((d.getWindow().getAttributes().flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) != 0);
        d.dispatchKeyEvent(new android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN,
                                                     android.view.KeyEvent.KEYCODE_BUTTON_A));
        assertEquals(1, forwarded[0]);
        s.stop();
        assertFalse(s.holdsFocus());
    }

    @Test
    public void launcherCompanionSwitchesPanels() {
        addBottom();
        LauncherCompanion lc = new LauncherCompanion(activity, null);
        lc.start();
        idle();
        assertTrue(lc.screen().content() instanceof LauncherPanel);
        lc.home("Sample Game", "Ready", Theme.ACCENT, true);
        assertEquals("Sample Game", ((LauncherPanel)lc.screen().content()).titleForTest());
        lc.preparing("4D530004", "Sample Game");
        assertTrue(lc.screen().content() instanceof PreparingPanel);
        lc.home("Sample Game", "Ready", Theme.ACCENT, true);
        assertTrue(lc.screen().content() instanceof LauncherPanel);
        lc.stop();
        assertFalse(lc.screen().isShowing());
    }

    /** The Presentation shown, found through the content view's window. */
    private android.app.Dialog panelDialog() {
        View root = screen.content() != null ? screen.content().getRootView() : null;
        assertNotNull(root);
        for (android.app.Dialog d : org.robolectric.shadows.ShadowDialog.getShownDialogs()) {
            if (d.isShowing() && d.getWindow() != null && d.getWindow().getDecorView() == root)
                return d;
        }
        throw new AssertionError("no Presentation found");
    }
}
