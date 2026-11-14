package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.view.Display;

import org.junit.Test;

import java.util.Arrays;
import java.util.List;

/** D2: the display chooser and the show / move / dismiss decision, pure functions over fake display lists. */
public class DualScreenTest {
    private static final GameStore.BottomScreen PANEL = GameStore.BottomScreen.PANEL,
            BLACK = GameStore.BottomScreen.BLACK, OFF = GameStore.BottomScreen.OFF;

    private static DualScreen.Disp top() {
        return new DualScreen.Disp(0, "Built-in Screen", Display.FLAG_SECURE | Display.FLAG_SUPPORTS_PROTECTED_BUFFERS,
                                   1920, 1080, 120, Display.STATE_ON, false);
    }

    private static DualScreen.Disp bottom(int flags, int state, boolean presentation) {
        return new DualScreen.Disp(2, "Second Screen", flags, 1240, 1080, 60, state, presentation);
    }

    private static List<DualScreen.Disp> list(DualScreen.Disp... d) {
        return Arrays.asList(d);
    }

    @Test
    public void oneDisplay() {
        assertNull(DualScreen.choose(list(top()), 0));
        assertEquals(DualScreen.Action.NONE, DualScreen.decide(PANEL, null, -1));
    }

    @Test
    public void twoDisplays() {
        DualScreen.Disp b = bottom(Display.FLAG_PRESENTATION, Display.STATE_ON, true);
        assertEquals(b, DualScreen.choose(list(top(), b), 0));
        // Not in the presentation category but public and on: still chosen (the fallback).
        DualScreen.Disp plain = bottom(0, Display.STATE_ON, false);
        assertEquals(plain, DualScreen.choose(list(top(), plain), 0));
        // An activity started on the bottom screen puts the panel on the top one.
        assertEquals(0, DualScreen.choose(list(top(), b), 2).id);
    }

    @Test
    public void presentationPreferred() {
        DualScreen.Disp other = new DualScreen.Disp(3, "Overlay", 0, 720, 480, 60, Display.STATE_ON, false);
        DualScreen.Disp pres = bottom(Display.FLAG_PRESENTATION, Display.STATE_ON, true);
        assertEquals(pres, DualScreen.choose(list(top(), other, pres), 0));
    }

    @Test
    public void privateDisplayIsNeverUsed() {
        DualScreen.Disp b = bottom(Display.FLAG_PRIVATE, Display.STATE_ON, false);
        assertNull(DualScreen.choose(list(top(), b), 0));
        DualScreen.Disp bp = bottom(Display.FLAG_PRIVATE | Display.FLAG_PRESENTATION, Display.STATE_ON, true);
        assertNull(DualScreen.choose(list(top(), bp), 0));
    }

    @Test
    public void displayThatIsOff() {
        assertNull(DualScreen.choose(list(top(), bottom(0, Display.STATE_OFF, false)), 0));
        assertNull(DualScreen.choose(list(top(), bottom(Display.FLAG_PRESENTATION, Display.STATE_OFF, true)), 0));
        // Doze: a plain display must be on to be used; a presentation display only must not be off.
        assertNull(DualScreen.choose(list(top(), bottom(0, Display.STATE_DOZE, false)), 0));
        assertEquals(2, DualScreen.choose(list(top(), bottom(Display.FLAG_PRESENTATION, Display.STATE_DOZE, true)),
                                          0).id);
    }

    @Test
    public void decide() {
        DualScreen.Disp b = bottom(Display.FLAG_PRESENTATION, Display.STATE_ON, true);
        assertEquals(DualScreen.Action.SHOW, DualScreen.decide(PANEL, b, -1));
        assertEquals(DualScreen.Action.SHOW, DualScreen.decide(BLACK, b, -1));
        assertEquals(DualScreen.Action.NONE, DualScreen.decide(PANEL, b, 2));
        assertEquals(DualScreen.Action.MOVE, DualScreen.decide(PANEL, b, 5));
        assertEquals(DualScreen.Action.DISMISS, DualScreen.decide(OFF, b, 2));
        assertEquals(DualScreen.Action.NONE, DualScreen.decide(OFF, b, -1));
        assertEquals(DualScreen.Action.DISMISS, DualScreen.decide(PANEL, null, 2));   // display removed
    }

    @Test
    public void focusMode() {
        assertFalse(DualScreen.forwardFocus(null, null));                 // Plan A by default
        assertTrue(DualScreen.forwardFocus("forward", null));
        assertTrue(DualScreen.forwardFocus(null, "forward"));
        assertFalse(DualScreen.forwardFocus("none", "forward"));          // recomp.cfg wins
        assertTrue(DualScreen.forwardFocus(" FORWARD ", "none"));
        assertFalse(DualScreen.forwardFocus("", "none"));
    }
}
