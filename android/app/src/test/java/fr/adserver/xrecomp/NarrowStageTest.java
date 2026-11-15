package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.view.View;
import android.widget.LinearLayout;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

/**
 * Single-screen handhelds that are not 16:9 (4:3, square): the stage narrows instead of shrinking, Home's art and
 * title stack, the library has fewer columns, a list keeps room for its description.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class NarrowStageTest {
    @Test
    public void designWidthKeepsTheHeightScale() {
        assertEquals(1920, Widgets.Stage.designWidth(1920, 1080));   // 16:9 (AYN Thor's top screen)
        assertEquals(1920, Widgets.Stage.designWidth(1280, 720));
        assertEquals(2400, Widgets.Stage.designWidth(2400, 1080));   // 20:9 phone: wider
        assertEquals(1440, Widgets.Stage.designWidth(1440, 1080));   // 4:3: narrower, same text size
        assertEquals(1440, Widgets.Stage.designWidth(960, 720));
        assertEquals(1280, Widgets.Stage.designWidth(1080, 1080));   // square: down to DESIGN_MIN_W, then shrinks
        assertEquals(1280, Widgets.Stage.designWidth(720, 720));
    }

    @Test
    public void libraryColumnsFollowTheWidth() {
        assertEquals(5, LibraryScreen.columns(1920 - 2 * Theme.MARGIN_X));
        assertEquals(5, LibraryScreen.columns(2400 - 2 * Theme.MARGIN_X));   // never more than on 16:9
        assertEquals(4, LibraryScreen.columns(1440 - 2 * Theme.MARGIN_X));
        assertEquals(3, LibraryScreen.columns(1280 - 2 * Theme.MARGIN_X));
        assertEquals(3, LibraryScreen.columns(100));
    }

    private static Widgets.HeroRow hero(int width) {
        Widgets.HeroRow row = new Widgets.HeroRow(RuntimeEnvironment.getApplication());
        row.addView(new View(row.getContext()), new LinearLayout.LayoutParams(Theme.ART, Theme.ART));
        row.addView(new View(row.getContext()), new LinearLayout.LayoutParams(0, 100, 1));
        row.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(900, View.MeasureSpec.EXACTLY));
        return row;
    }

    @Test
    public void heroStacksOnANarrowStage() {
        Widgets.HeroRow wide = hero(1920 - 2 * Theme.MARGIN_X);
        assertFalse(wide.stacked());
        assertEquals(Theme.ART, wide.getChildAt(0).getMeasuredWidth());

        Widgets.HeroRow narrow = hero(1440 - 2 * Theme.MARGIN_X);
        assertTrue(narrow.stacked());
        assertEquals(Theme.ART_COMPACT, narrow.getChildAt(0).getMeasuredWidth());
        assertEquals(1440 - 2 * Theme.MARGIN_X, narrow.getChildAt(1).getMeasuredWidth());   // the buttons' full row

        // Back to wide (a resize): side by side again.
        narrow.measure(View.MeasureSpec.makeMeasureSpec(1728, View.MeasureSpec.EXACTLY),
                       View.MeasureSpec.makeMeasureSpec(900, View.MeasureSpec.EXACTLY));
        assertFalse(narrow.stacked());
        assertEquals(Theme.ART, narrow.getChildAt(0).getMeasuredWidth());
    }

    @Test
    public void listKeepsRoomForTheDescription() {
        for (int w : new int[] { 1728, 1248, 1088 }) {
            Widgets.ListSplit split = new Widgets.ListSplit(RuntimeEnvironment.getApplication(),
                                                            LauncherActivity.LIST_W, LauncherActivity.DESC_MIN_W);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LauncherActivity.LIST_W, 100);
            lp.leftMargin = -Theme.ROW_INSET;
            split.addView(new View(split.getContext()), lp);
            LinearLayout.LayoutParams dp = new LinearLayout.LayoutParams(0, 100, 1);
            dp.leftMargin = LauncherActivity.DESC_GAP;
            split.addView(new View(split.getContext()), dp);
            split.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY),
                          View.MeasureSpec.makeMeasureSpec(900, View.MeasureSpec.EXACTLY));
            int desc = split.getChildAt(1).getMeasuredWidth();
            assertTrue(w + ": description " + desc, desc >= LauncherActivity.DESC_MIN_W);
            if (w == 1728)
                assertEquals(LauncherActivity.LIST_W, split.getChildAt(0).getMeasuredWidth());
        }
    }
}
