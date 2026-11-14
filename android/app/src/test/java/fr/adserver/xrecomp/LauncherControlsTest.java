package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * XBA-131: the Controls screen remaps the pad, with a pad only (D-pad to move, left / right or A to change, Y to
 * reset), into recomp.cfg's RECOMP_PAD_MAP; rendered to build/screenshots/controls-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherControlsTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private Application app;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
    }

    @After
    public void tearDown() {
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private static void key(LauncherActivity a, int code) {
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, code));
        a.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, code));
        idle(300);
    }

    private LauncherActivity controls() {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        a.goForTest(LauncherActivity.Screen.CONTROLS);
        idle(400);
        a.forceFocusForTest();
        idle(300);
        return a;
    }

    private void shot(LauncherActivity a, String name) throws Exception {
        idle(400);
        File f = TestSupport.screenshot(a.getWindow().getDecorView(), name);
        assertTrue(f.length() > 0);
    }

    private File cfgFile() {
        return new File(GameStore.externalFilesDir(app), RecompConfig.FILE_NAME);
    }

    private String padMapLine() {
        return RecompConfig.load(GameStore.externalFilesDir(app)).get(PadMapping.KEY);
    }

    private static List<Widgets.MenuRow> rows(View v, List<Widgets.MenuRow> out) {
        if (v instanceof Widgets.MenuRow)
            out.add((Widgets.MenuRow)v);
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup)v;
            for (int i = 0; i < g.getChildCount(); i++)
                rows(g.getChildAt(i), out);
        }
        return out;
    }

    private static Widgets.MenuRow row(LauncherActivity a, String title) {
        for (Widgets.MenuRow r : rows(a.getWindow().getDecorView(), new ArrayList<>()))
            if (title.equals(r.title))
                return r;
        return null;
    }

    /** Moves the focus down with the D-pad until the row titled title has it. */
    private static void focusRow(LauncherActivity a, String title) {
        for (int i = 0; i < 40 && !(a.focusForTest() instanceof Widgets.MenuRow
                                    && title.equals(((Widgets.MenuRow)a.focusForTest()).title)); i++)
            key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        assertTrue("focus on " + title, a.focusForTest() instanceof Widgets.MenuRow
                                         && title.equals(((Widgets.MenuRow)a.focusForTest()).title));
    }

    @Test
    public void everyXboxControlHasARow() throws Exception {
        LauncherActivity a = controls();
        assertEquals(LauncherActivity.Screen.CONTROLS, a.screenForTest());
        for (PadMapping.Control c : PadMapping.Control.values())
            assertNotNull(c.name(), row(a, app.getString(ControlsLabels.control(c))));
        assertNotNull(row(a, app.getString(R.string.XB_LSTICK)));
        assertNotNull(row(a, app.getString(R.string.XB_RSTICK)));
        assertNotNull(row(a, app.getString(R.string.CTL_RESET)));
        assertEquals(app.getString(R.string.SRC_L1), row(a, app.getString(R.string.XB_BLACK)).value);
        assertEquals(app.getString(R.string.SRC_L2), row(a, app.getString(R.string.XB_LT)).value);
        assertEquals("the first row has the focus", app.getString(R.string.XB_A),
                     ((Widgets.MenuRow)a.focusForTest()).title);
        assertTrue(a.descriptionForTest(), a.descriptionForTest().contains(app.getString(R.string.SRC_SOUTH)));
        shot(a, "controls-1-default");
        assertNull("nothing written until something changes", padMapLine());
    }

    @Test
    public void remapWithThePadAndReset() throws Exception {
        LauncherActivity a = controls();
        // A: right once -> B (east).
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("a=east", padMapLine());
        assertEquals(app.getString(R.string.SRC_EAST), row(a, app.getString(R.string.XB_A)).value);
        // B: down, left -> A (south): the pair swapped.
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);
        assertEquals("a=east,b=south", padMapLine());
        // Black on L2: down to Black, A steps it once (L1 -> R1), again (R1 -> L2).
        focusRow(a, app.getString(R.string.XB_BLACK));
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        key(a, KeyEvent.KEYCODE_BUTTON_A);
        assertEquals("a=east,b=south,black=left_trigger", padMapLine());
        assertTrue(a.descriptionForTest(), a.descriptionForTest().contains(app.getString(R.string.SRC_L2)));
        assertTrue("L2 also gives the left trigger: said",
                   a.descriptionForTest().contains(app.getString(R.string.XB_LT)));
        // The right stick off.
        focusRow(a, app.getString(R.string.XB_RSTICK));
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("a=east,b=south,black=left_trigger,right_stick=none", padMapLine());
        assertTrue(a.noteForTest(), a.noteForTest().equals(app.getString(R.string.SETTINGS_SAVED)));
        shot(a, "controls-2-remapped");

        // A fresh screen reads it back.
        LauncherActivity b = controls();
        assertEquals(app.getString(R.string.SRC_EAST), row(b, app.getString(R.string.XB_A)).value);
        assertEquals(PadMapping.Stick.NONE, b.padMapForTest().rightStick);

        // Y: back to the usual buttons, the line removed (the file's other lines stay).
        key(b, KeyEvent.KEYCODE_BUTTON_Y);
        assertNull(padMapLine());
        assertEquals(app.getString(R.string.CTL_RESET_DONE), b.noteForTest());
        assertEquals(app.getString(R.string.SRC_SOUTH), row(b, app.getString(R.string.XB_A)).value);
        assertTrue(cfgFile().isFile());
    }

    @Test
    public void frenchScreen() throws Exception {
        RuntimeEnvironment.setQualifiers("fr-w1920dp-h1080dp-land-mdpi");
        TestSupport.write(cfgFile(), "RECOMP_PAD_MAP=a=east,b=south,white=right_trigger,right_trigger=right_shoulder\n");
        LauncherActivity a = controls();
        assertEquals("R2", row(a, "Blanc").value);
        shot(a, "controls-3-fr");
    }
}
