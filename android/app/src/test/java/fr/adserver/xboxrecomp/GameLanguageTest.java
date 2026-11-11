package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.util.Locale;

/** XBA-77: codes, the order of precedence, the per-title choice. Codes match src/kernel/kernel_xbox.c. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GameLanguageTest {
    @Test
    public void codes() {
        assertEquals(3, GameLanguage.indexOf("fr"));
        assertEquals(3, GameLanguage.indexOf("FR"));
        assertEquals(3, GameLanguage.indexOf("fr-CA"));
        assertEquals(8, GameLanguage.indexOf("pt_BR"));
        assertEquals(6, GameLanguage.indexOf("7"));
        assertEquals(-1, GameLanguage.indexOf("xx"));
        assertEquals(-1, GameLanguage.indexOf("fra"));
        assertEquals(-1, GameLanguage.indexOf(""));
        assertEquals(-1, GameLanguage.indexOf(null));
        assertEquals("Français", GameLanguage.name("fr"));
        assertEquals(GameLanguage.CODES.length, GameLanguage.MENU_ORDER.length);
    }

    @Test
    public void precedence() {
        // The title's choice wins over everything.
        assertEquals("de", GameLanguage.resolve("de", "fr", Locale.ITALIAN));
        // A recomp.cfg line: leave the variable unset, the runtime reads the file.
        assertNull(GameLanguage.resolve(null, "fr", Locale.ITALIAN));
        // Nothing set: the Android language when the Xbox has it.
        assertEquals("it", GameLanguage.resolve(null, null, Locale.ITALIAN));
        assertEquals("pt", GameLanguage.resolve(null, null, new Locale("pt", "BR")));
        assertNull(GameLanguage.resolve(null, null, new Locale("nl", "NL")));
        assertNull(GameLanguage.resolve("garbage", null, null));
    }

    @Test
    public void titleChoiceIsPerTitle() {
        Context c = RuntimeEnvironment.getApplication();
        assertNull(GameLanguage.titleChoice(c, "4D5300D1"));
        assertTrue(GameLanguage.setTitleChoice(c, "4d5300d1", "fr"));
        assertEquals("fr", GameLanguage.titleChoice(c, "4D5300D1"));
        assertNull(GameLanguage.titleChoice(c, "4D530004"));
        assertTrue(GameLanguage.setTitleChoice(c, "4D5300D1", null));
        assertNull(GameLanguage.titleChoice(c, "4D5300D1"));
        assertNull(GameLanguage.titleChoice(c, null));
    }
}
