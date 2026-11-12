package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;

import android.app.Activity;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

/** XBA-99: the game window hides the system bars, swipe shows them for a moment, and it draws under the cutout. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
public class SystemBarsTest {
    @Test
    public void hidesBarsWithSwipeBehaviourAndCutout() {
        Activity a = Robolectric.buildActivity(Activity.class).setup().get();
        SystemBars.hide(a);
        WindowInsetsController c = a.getWindow().getDecorView().getWindowInsetsController();
        assertNotNull(c);
        assertEquals(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE, c.getSystemBarsBehavior());
        assertEquals(WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES,
                     a.getWindow().getAttributes().layoutInDisplayCutoutMode);
        WindowInsets in = a.getWindow().getDecorView().getRootWindowInsets();
        if (in != null)
            assertFalse("status and navigation bars hidden", in.isVisible(WindowInsets.Type.systemBars()));
    }

    @Test
    public void appliedAgainIsHarmless() {
        Activity a = Robolectric.buildActivity(Activity.class).setup().get();
        SystemBars.hide(a);
        SystemBars.hide(a.getWindow());
        SystemBars.hide((android.view.Window) null);
        assertEquals(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE,
                     a.getWindow().getDecorView().getWindowInsetsController().getSystemBarsBehavior());
    }
}
