package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

/** XBA-134: the per-title widescreen and 480p; variables as src/kernel/kernel_xbox.c reads them. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GameVideoTest {
    @Test
    public void variables() {
        assertEquals("RECOMP_WIDESCREEN", GameVideo.Setting.WIDESCREEN.env);
        assertEquals("RECOMP_480P", GameVideo.Setting.P480.env);
    }

    @Test
    public void resolve() {
        assertNull(GameVideo.resolve(null));                 // recomp.cfg, then off
        assertEquals("1", GameVideo.resolve(Boolean.TRUE));
        assertEquals("0", GameVideo.resolve(Boolean.FALSE)); // off even when recomp.cfg says on
    }

    @Test
    public void cycle() {
        assertEquals(Boolean.TRUE, GameVideo.next(null, 1));
        assertEquals(Boolean.FALSE, GameVideo.next(Boolean.TRUE, 1));
        assertNull(GameVideo.next(Boolean.FALSE, 1));
        assertEquals(Boolean.FALSE, GameVideo.next(null, -1));
    }

    @Test
    public void titleChoiceIsPerTitleAndPerSetting() {
        Context c = RuntimeEnvironment.getApplication();
        GameVideo.Setting w = GameVideo.Setting.WIDESCREEN, p = GameVideo.Setting.P480;
        assertNull(GameVideo.titleChoice(c, "4D5300D1", w));
        assertTrue(GameVideo.setTitleChoice(c, "4d5300d1", w, Boolean.FALSE));
        assertEquals(Boolean.FALSE, GameVideo.titleChoice(c, "4D5300D1", w));
        assertNull(GameVideo.titleChoice(c, "4D5300D1", p));
        assertNull(GameVideo.titleChoice(c, "4D530004", w));
        assertTrue(GameVideo.setTitleChoice(c, "4D5300D1", p, Boolean.TRUE));
        assertEquals(Boolean.TRUE, GameVideo.titleChoice(c, "4D5300D1", p));
        assertTrue(GameVideo.setTitleChoice(c, "4D5300D1", w, null));
        assertNull(GameVideo.titleChoice(c, "4D5300D1", w));
        assertNull(GameVideo.titleChoice(c, null, w));
        assertFalse(GameVideo.setTitleChoice(c, "", w, Boolean.TRUE));
    }
}
