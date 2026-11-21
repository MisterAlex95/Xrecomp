package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertFalse;
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
 * XBA-336: Settings › Advanced › Privacy. The sheet turns « Send reports automatically » on and off (its rows come and
 * go), resets the install id, and gives the focus back to the Privacy row; rendered in en / fr on every stage.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherPrivacyTest {
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
        Telemetry.setAuto(app, false);
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

    private static List<View> views(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                views(((ViewGroup)v).getChildAt(i), out);
        return out;
    }

    private static List<String> titles(View v) {
        List<String> out = new ArrayList<>();
        for (Widgets.MenuRow r : rows(v, new ArrayList<>()))
            out.add(r.title);
        return out;
    }

    private Widgets.MenuRow privacyRow(LauncherActivity a) {
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        key(a, KeyEvent.KEYCODE_BUTTON_L1);   // wraps to Advanced
        for (Widgets.MenuRow r : rows(a.getWindow().getDecorView(), new ArrayList<>()))
            if (app.getString(R.string.PRIVACY).equals(r.title))
                return r;
        return null;
    }

    @Test
    public void thePrivacySheetTurnsTheOptionOnAndOff() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        Widgets.MenuRow p = privacyRow(a);
        assertNotNull("Settings › Advanced › Privacy", p);
        assertEquals(app.getString(R.string.VAL_OFF), p.value);
        p.requestFocusFromTouch();
        assertSame(p, a.focusForTest());
        p.performClick();
        idle(300);
        assertNotNull(a.sheetForTest());
        assertEquals(Arrays.asList(app.getString(R.string.AUTO_REPORTS)), titles(a.sheetForTest()));

        rows(a.sheetForTest(), new ArrayList<>()).get(0).performClick();   // on
        idle(300);
        assertTrue(Telemetry.auto(app));
        String id = Telemetry.installId(app);
        assertNotNull(id);
        assertEquals(app.getString(R.string.VAL_ON), p.value);
        assertEquals(Arrays.asList(app.getString(R.string.AUTO_REPORTS), app.getString(R.string.AUTO_REPORTS_MOBILE),
                                   app.getString(R.string.AUTO_REPORTS_RESET)), titles(a.sheetForTest()));
        List<Widgets.MenuRow> r = rows(a.sheetForTest(), new ArrayList<>());
        r.get(1).performClick();   // also on mobile data
        idle(300);
        assertTrue(AutoReports.mobileData(app));
        r.get(2).performClick();   // reset
        idle(300);
        assertNotNull(a.sheetForTest());
        assertNotEquals(id, Telemetry.installId(app));

        r.get(0).performClick();   // off
        idle(300);
        assertFalse(Telemetry.auto(app));
        assertNull(Telemetry.installId(app));
        assertEquals(0, AutoReports.pending(app));
        assertEquals(app.getString(R.string.VAL_OFF), p.value);
        assertEquals(Arrays.asList(app.getString(R.string.AUTO_REPORTS)), titles(a.sheetForTest()));

        key(a, KeyEvent.KEYCODE_BUTTON_B);
        assertNull(a.sheetForTest());
        assertSame(p, a.focusForTest());
    }

    /** The sheet with the option on, for review: « What is sent » must not be cut on any stage. */
    @Test
    public void renders() throws Exception {
        String[][] sizes = { { "720", "w1280dp-h720dp-land-mdpi" }, { "1080", "w1920dp-h1080dp-land-mdpi" },
                             { "4x3", "w1440dp-h1080dp-land-mdpi" }, { "1x1", "w1080dp-h1080dp-land-mdpi" } };
        for (String loc : new String[] { "en", "fr" })
            for (String[] size : sizes) {
                RuntimeEnvironment.setQualifiers(loc + "-" + size[1]);
                Telemetry.setAuto(app, true);
                LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
                idle(300);
                Widgets.MenuRow p = privacyRow(a);
                p.requestFocusFromTouch();
        assertSame(p, a.focusForTest());
                p.performClick();
                idle(400);
                android.widget.TextView what = null;
                for (View v : views(a.sheetForTest(), new ArrayList<>()))
                    if (v instanceof android.widget.TextView
                        && app.getString(R.string.AUTO_REPORTS_WHAT).contentEquals(((android.widget.TextView)v).getText()))
                        what = (android.widget.TextView)v;
                assertNotNull(what);
                android.text.Layout l = what.getLayout();
                assertNotNull(l);
                assertEquals(loc + " " + size[0] + ": « What is sent » cut", 0, l.getEllipsisCount(l.getLineCount() - 1));
                File f = TestSupport.screenshot(a.getWindow().getDecorView(), "ui-privacy-" + loc + "-" + size[0]);
                assertTrue(f.length() > 0);
                List<Widgets.MenuRow> r = rows(a.sheetForTest(), new ArrayList<>());
                assertEquals(3, r.size());
                for (Widgets.MenuRow row : r) {
                    View box = (View)row.getParent();
                    assertTrue(loc + " " + size[0] + ": " + row.title + " cut", row.getBottom() <= box.getHeight());
                }
                Telemetry.setAuto(app, false);
            }
    }
}
