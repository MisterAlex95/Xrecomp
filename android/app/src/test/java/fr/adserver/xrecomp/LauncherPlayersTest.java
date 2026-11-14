package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
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
import java.util.Arrays;
import java.util.List;

/**
 * XBA-123 and XBA-42 in the launcher's Controls screen: « Players » (which controller each port gets,
 * RECOMP_PAD_PORTS in recomp.cfg) and « On-screen pad » (files/settings/touch_controls), driven with the pad, with
 * a handheld's built-in pad and a Bluetooth pad connected (PadDevices.forTest); build/screenshots/controls-4-*.png.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherPlayersTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static final PadPorts.Device THOR = new PadPorts.Device("2dc8:3106", "Thor Controller", true);
    private static final PadPorts.Device XBOX = new PadPorts.Device("045e:0b13", "Xbox Wireless Controller", false);

    private Application app;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"),
                          TestSupport.xbe(0x4D530004, "Sample Game"));
        PadDevices.forTest = Arrays.asList(XBOX, THOR);
    }

    @After
    public void tearDown() {
        PadDevices.forTest = null;
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

    private static void focusRow(LauncherActivity a, String title) {
        for (int i = 0; i < 60 && !(a.focusForTest() instanceof Widgets.MenuRow
                                    && title.equals(((Widgets.MenuRow)a.focusForTest()).title)); i++)
            key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        assertTrue("focus on " + title, a.focusForTest() instanceof Widgets.MenuRow
                                         && title.equals(((Widgets.MenuRow)a.focusForTest()).title));
    }

    private String portsLine() {
        return RecompConfig.load(GameStore.externalFilesDir(app)).get(PadPorts.KEY);
    }

    @Test
    public void portsShowTheStartAndPinWithThePad() throws Exception {
        LauncherActivity a = controls();
        String port1 = app.getString(R.string.CTL_PORT, 1), port2 = app.getString(R.string.CTL_PORT, 2);
        assertEquals(app.getString(R.string.CTL_PORT_AUTO), row(a, port1).value);
        focusRow(a, port1);
        String d = a.descriptionForTest();
        assertTrue(d, d.contains("port 1, Thor Controller (built in)"));
        assertTrue(d, d.contains("port 2, Xbox Wireless Controller"));
        assertTrue(d, d.contains("port 3, nobody"));
        idle(400);
        TestSupport.screenshot(a.getWindow().getDecorView(), "controls-4-players");
        assertNull("nothing written until something changes", portsLine());

        // Port 1: right once = the Xbox pad (the choices, in Android's order: First come, Xbox, Thor).
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("Xbox Wireless Controller", row(a, port1).value);
        assertEquals("1=045e:0b13:Xbox Wireless Controller", portsLine());
        d = a.descriptionForTest();
        assertTrue(d, d.contains("port 1, Xbox Wireless Controller") && d.contains("port 2, Thor Controller"));
        // Port 2 takes the Xbox pad: port 1 is First come again.
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals("Xbox Wireless Controller", row(a, port2).value);
        assertEquals(app.getString(R.string.CTL_PORT_AUTO), row(a, port1).value);
        assertEquals("2=045e:0b13:Xbox Wireless Controller", portsLine());

        // The pad goes away: the pin stays, marked.
        PadDevices.forTest = Arrays.asList(THOR);
        LauncherActivity b = controls();
        assertEquals(app.getString(R.string.CTL_PORT_ABSENT, "Xbox Wireless Controller"), row(b, port2).value);
    }

    @Test
    public void onScreenPadSettings() throws Exception {
        LauncherActivity a = controls();
        String show = app.getString(R.string.CTL_TOUCH);
        assertEquals(app.getString(R.string.CTL_TOUCH_AUTO), row(a, show).value);
        focusRow(a, show);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);
        assertEquals(app.getString(R.string.CTL_TOUCH_ON), row(a, show).value);
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        key(a, KeyEvent.KEYCODE_DPAD_RIGHT);                     // opacity 50 -> 70
        key(a, KeyEvent.KEYCODE_DPAD_DOWN);
        key(a, KeyEvent.KEYCODE_DPAD_LEFT);                      // size normal -> small
        TouchSettings s = TouchSettings.load(app);
        assertEquals(TouchSettings.Mode.ON, s.mode);
        assertEquals(70, s.opacity);
        assertEquals(80, s.size);
        assertEquals(app.getString(R.string.CTL_SIZE_SMALL), row(a, app.getString(R.string.CTL_TOUCH_SIZE)).value);
        idle(400);
        TestSupport.screenshot(a.getWindow().getDecorView(), "controls-5-touch");
    }
}
