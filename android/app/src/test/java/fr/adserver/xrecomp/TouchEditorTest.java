package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.util.concurrent.atomic.AtomicReference;

/**
 * XBA-207: the touch layout editor (TouchEditor) laid out at 1920 x 1080: a drag moves a control, Save writes the
 * console's layout or the game's own, Cancel and B write nothing; screenshots of each console's editor in
 * build/screenshots/ui-touch-editor-<layout>-<en|fr>.png for review.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TouchEditorTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private Activity activity;
    private final AtomicReference<Object> closed = new AtomicReference<>();
    private static final Object CANCELLED = new Object();

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        deleteTree(TouchLayout.dir(activity.getFilesDir()));
    }

    private TouchEditor open(TouchPad.Layout l, String key, boolean overGame) {
        closed.set(null);
        TouchEditor ed = new TouchEditor(activity, l, key, new TouchSettings(), overGame,
                                         saved -> closed.set(saved == null ? CANCELLED : saved));
        lay(ed);
        return ed;
    }

    private static void lay(View ed) {
        ed.measure(View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY),
                   View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY));
        ed.layout(0, 0, 1920, 1080);
    }

    private static void touch(View v, int action, float x, float y) {
        long t = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(t, t, action, x, y, 0);
        v.dispatchTouchEvent(e);
        e.recycle();
    }

    @Test
    public void dragAndSaveForTheConsole() {
        TouchEditor ed = open(TouchPad.Layout.GBA, null, false);
        TouchPad.Control start = ed.padView.pad.control(TouchPad.START);
        float x0 = start.cx, y0 = start.cy;
        touch(ed, MotionEvent.ACTION_DOWN, x0, y0);
        touch(ed, MotionEvent.ACTION_MOVE, x0 + 300, y0 - 400);
        touch(ed, MotionEvent.ACTION_UP, x0 + 300, y0 - 400);
        assertEquals(TouchPad.START, ed.edit.selected.id);
        assertTrue(Math.abs(start.cx - (x0 + 300)) <= ed.edit.gridStep());
        assertTrue(ed.save.performClick());
        Object got = closed.get();
        assertTrue(got instanceof TouchLayout);
        TouchLayout saved = (TouchLayout)got;
        assertEquals(start.cx / 1920f, saved.entries.get(TouchPad.START).x, 1e-3f);
        assertTrue(TouchLayout.consoleFile(activity.getFilesDir(), TouchPad.Layout.GBA).isFile());
        // The next editor (and the game) start from it.
        TouchEditor again = open(TouchPad.Layout.GBA, null, false);
        assertEquals(start.cx, again.padView.pad.control(TouchPad.START).cx, 0.5f);
    }

    @Test
    public void thisGameOnlyAndCancel() {
        TouchEditor ed = open(TouchPad.Layout.PS1, "PS1-SLUS00001", true);
        assertFalse(ed.gameOnly());
        ed.edit.selected = ed.padView.pad.control(TouchPad.SELECT);
        ed.edit.toggleHidden();
        // B cancels: nothing written.
        long t = SystemClock.uptimeMillis();
        ed.key(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_B, 0));
        assertNull(closed.get());
        ed.key(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_B, 0));
        assertEquals(CANCELLED, closed.get());
        assertFalse(TouchLayout.dir(activity.getFilesDir()).exists());

        ed = open(TouchPad.Layout.PS1, "PS1-SLUS00001", true);
        ed.edit.selected = ed.padView.pad.control(TouchPad.SELECT);
        ed.edit.toggleHidden();
        View scope = findText(ed, activity.getString(R.string.TE_SCOPE_CONSOLE));
        assertNotNull(scope);
        scope.performClick();
        assertTrue(ed.gameOnly());
        ed.save();
        File files = activity.getFilesDir();
        assertTrue(TouchLayout.gameFile(files, "PS1-SLUS00001", TouchPad.Layout.PS1).isFile());
        assertFalse(TouchLayout.consoleFile(files, TouchPad.Layout.PS1).exists());
        TouchLayout.Resolved r = TouchLayout.resolve(files, "PS1-SLUS00001", TouchPad.Layout.PS1);
        assertTrue(r.perGame);
        assertTrue(r.layout.entries.get(TouchPad.SELECT).hidden);
        // Reopened: « This game only » again.
        assertTrue(open(TouchPad.Layout.PS1, "PS1-SLUS00001", true).gameOnly());
    }

    @Test
    public void screenshots() throws Exception {
        for (String loc : new String[] { "en", "fr" }) {
            RuntimeEnvironment.setQualifiers(loc + "-w1920dp-h1080dp-land-mdpi");
            activity = Robolectric.buildActivity(Activity.class).setup().get();
            File hint = TestSupport.screenshot(open(TouchPad.Layout.PS1_ANALOG, "PS1-TEST", true),
                                               "ui-touch-editor-hint-" + loc);
            assertTrue(hint.length() > 0);
            for (TouchPad.Layout l : TouchPad.Layout.values()) {
                TouchEditor ed = open(l, l == TouchPad.Layout.N64 ? "N64-TEST" : null, false);
                // One control selected, one hidden: the editor's marks show.
                ed.edit.selected = ed.padView.pad.controls.get(1);
                ed.padView.pad.controls.get(ed.padView.pad.controls.size() - 1).hidden = true;
                ed.refresh();
                lay(ed);
                File f = TestSupport.screenshot(ed, "ui-touch-editor-" + l.name().toLowerCase(java.util.Locale.ROOT)
                                                    + "-" + loc);
                assertTrue(f.length() > 0);
            }
        }
        RuntimeEnvironment.setQualifiers("en-w1920dp-h1080dp-land-mdpi");
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids)
                deleteTree(k);
        f.delete();
    }

    private static View findText(View v, String text) {
        if (v instanceof android.widget.TextView && text.contentEquals(((android.widget.TextView)v).getText()))
            return v;
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup)v;
            for (int i = 0; i < g.getChildCount(); i++) {
                View f = findText(g.getChildAt(i), text);
                if (f != null)
                    return f;
            }
        }
        return null;
    }
}
