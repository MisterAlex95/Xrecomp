package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.view.View;
import android.widget.LinearLayout;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

/**
 * Widgets.PillRow keeps a game's buttons on one line: a library game not imported yet shows Import, Prepare now,
 * Settings, Controls and Manage, which overflowed the hero column (the last pill was cut at the screen's edge).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
public class PillRowTest {
    private static Widgets.PillButton add(Widgets.PillRow row, Widgets.Icon icon, String label, boolean primary) {
        Widgets.PillButton b = new Widgets.PillButton(row.getContext()).set(icon, label, primary);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
            primary ? Math.max(LauncherActivity.PRIMARY_MIN_W, b.naturalWidth()) : LinearLayout.LayoutParams.WRAP_CONTENT,
            Theme.BUTTON_H);
        p.leftMargin = row.getChildCount() == 0 ? 0 : Theme.S2;
        row.addView(b, p);
        return b;
    }

    private static int measure(Widgets.PillRow row, int width) {
        row.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(Theme.BUTTON_H, View.MeasureSpec.EXACTLY));
        int used = 0;
        for (int i = 0; i < row.getChildCount(); i++) {
            View v = row.getChildAt(i);
            used += v.getMeasuredWidth() + ((LinearLayout.LayoutParams)v.getLayoutParams()).leftMargin;
        }
        return used;
    }

    private static Widgets.PillRow fiveButtons(Widgets.PillButton[] out) {
        Context c = RuntimeEnvironment.getApplication();
        Widgets.PillRow row = new Widgets.PillRow(c);
        out[0] = add(row, null, "Import", true);
        out[1] = add(row, null, "Prepare now", false);
        out[2] = add(row, Widgets.Icon.SLIDERS, "Settings", false);
        out[3] = add(row, Widgets.Icon.PAD, "Controls", false);
        out[4] = add(row, Widgets.Icon.DISC, "Manage", false);
        return row;
    }

    @Test
    public void overflowingPillsTurnIntoIconsFromTheEnd() {
        Widgets.PillButton[] b = new Widgets.PillButton[5];
        Widgets.PillRow row = fiveButtons(b);
        int wide = measure(row, 4000);
        for (Widgets.PillButton p : b)
            assertFalse(p.isCompact());
        int avail = wide - Theme.BUTTON_H;   // too narrow for the five labels
        assertTrue(measure(row, avail) <= avail);
        assertTrue(b[4].isCompact());
        assertEquals(Theme.BUTTON_H, b[4].getMeasuredWidth());
        assertEquals("Manage", b[4].getContentDescription());
        assertFalse("the primary and label-only pills keep their label", b[0].isCompact() || b[1].isCompact());
    }

    @Test
    public void hero1280FitsTheNotImportedRow() {
        Widgets.PillButton[] b = new Widgets.PillButton[5];
        Widgets.PillRow row = fiveButtons(b);
        assertTrue(measure(row, 1280) <= 1280);
        assertFalse(measure(row, 4000) <= 0);
        for (Widgets.PillButton p : b)
            assertFalse("back to labels when there is room", p.isCompact());
    }
}
