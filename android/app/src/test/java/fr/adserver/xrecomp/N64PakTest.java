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

/** XBA-174: the N64 pak per game, and the variable consoles/n64/runtime/n64_runtime.c reads. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class N64PakTest {
    @Test
    public void variable() {
        assertEquals("RECOMP_N64_PAK", N64Pak.ENV);
        assertEquals("controller", N64Pak.CONTROLLER);   // n64_runtime_pak_parse's names
        assertEquals("rumble", N64Pak.RUMBLE);
    }

    @Test
    public void choiceIsPerGame() {
        Context c = RuntimeEnvironment.getApplication();
        assertNull(N64Pak.choice(c, "n64-NSME-635A2BFF"));
        assertTrue(N64Pak.setChoice(c, "n64-nsme-635a2bff", N64Pak.RUMBLE));
        assertEquals(N64Pak.RUMBLE, N64Pak.choice(c, "n64-NSME-635A2BFF"));
        assertNull(N64Pak.choice(c, "n64-NSME-00000001"));
        assertTrue(N64Pak.setChoice(c, "n64-NSME-635A2BFF", N64Pak.CONTROLLER));
        assertEquals(N64Pak.CONTROLLER, N64Pak.choice(c, "n64-NSME-635A2BFF"));
        assertTrue(N64Pak.setChoice(c, "n64-NSME-635A2BFF", null));
        assertNull(N64Pak.choice(c, "n64-NSME-635A2BFF"));
        assertTrue(N64Pak.setChoice(c, "n64-NSME-635A2BFF", "transfer"));   // not offered: none
        assertNull(N64Pak.choice(c, "n64-NSME-635A2BFF"));
        assertNull(N64Pak.choice(c, null));
        assertFalse(N64Pak.setChoice(c, "", N64Pak.RUMBLE));
    }

    @Test
    public void rumbleFromTheChoiceThenRecompCfg() {
        assertFalse(N64Pak.rumble(null, null));                       // the controller pak by default
        assertTrue(N64Pak.rumble(null, " Rumble "));                  // recomp.cfg's line
        assertFalse(N64Pak.rumble(N64Pak.CONTROLLER, "rumble"));      // the game's choice wins
        assertTrue(N64Pak.rumble(N64Pak.RUMBLE, null));
        assertEquals(N64Pak.RUMBLE, N64Pak.toggled(false));
        assertEquals(N64Pak.CONTROLLER, N64Pak.toggled(true));
    }
}
