package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Test;

/**
 * XBA-42: what the fingers on the on-screen pad press (TouchPad, pure Java), on a 2400 x 1080 phone: both sticks
 * analog at once with buttons, the D-pad's eight directions, sliding between buttons, a stick kept when the finger
 * slides off, the triggers full, release, the layout clear of the menu's edge swipe and on screen at every size;
 * the visibility rule and the mix with the companion panel.
 */
public class TouchPadTest {
    private static final float W = 2400, H = 1080;
    private TouchPad pad;

    @Before
    public void setUp() {
        pad = new TouchPad();
        pad.layout(W, H, 1f);
    }

    private TouchPad.Control c(int id) {
        return pad.control(id);
    }

    @Test
    public void twoSticksAndAButtonAtOnce() {
        TouchPad.Control ls = c(TouchPad.LSTICK), rs = c(TouchPad.RSTICK), a = c(TouchPad.A);
        pad.down(0, ls.cx, ls.cy);
        pad.down(1, rs.cx, rs.cy);
        pad.down(2, a.cx, a.cy);
        TouchPad.State s = pad.state();
        assertEquals(0, s.lx);   // landing on the centre: the dead zone
        assertEquals(1, s.analogMask);
        pad.move(0, ls.cx + ls.r, ls.cy);               // left stick full right
        pad.move(1, rs.cx, rs.cy - rs.r * 0.5f);        // right stick half up (Y up)
        s = pad.state();
        assertEquals(32767, s.lx);
        assertEquals(0, s.ly);
        assertEquals(0, s.rx);
        assertTrue(s.ry > 14000 && s.ry < 17500);
        assertEquals(1 << TouchPad.AN_A, s.analogMask);
        pad.up(2);
        s = pad.state();
        assertEquals(0, s.analogMask);
        assertEquals(32767, s.lx);
        pad.up(0);
        pad.up(1);
        assertEquals(new TouchPad.State(), pad.state());
    }

    @Test
    public void aStickKeepsItsFingerOffTheCircle() {
        TouchPad.Control ls = c(TouchPad.LSTICK);
        pad.down(5, ls.cx, ls.cy);
        pad.move(5, ls.cx, ls.cy + ls.r * 3);           // far below, over the D-pad
        TouchPad.State s = pad.state();
        assertEquals(-32767, s.ly);                     // full down, clamped
        assertEquals(0, s.buttons);                     // not the D-pad
        pad.move(5, ls.cx - ls.r * 0.7f, ls.cy - ls.r * 0.7f);   // diagonal at the edge
        s = pad.state();
        assertTrue(s.lx < -22000 && s.ly > 22000);
    }

    @Test
    public void dpadEightWays() {
        TouchPad.Control d = c(TouchPad.DPAD);
        float r = d.r * 0.8f;
        int[][] cases = {
            { 0, -1, TouchPad.XB_UP }, { 0, 1, TouchPad.XB_DOWN }, { -1, 0, TouchPad.XB_LEFT }, { 1, 0, TouchPad.XB_RIGHT },
            { 1, -1, TouchPad.XB_UP | TouchPad.XB_RIGHT }, { -1, 1, TouchPad.XB_DOWN | TouchPad.XB_LEFT },
        };
        for (int[] k : cases) {
            float n = (float)Math.sqrt(k[0] * k[0] + k[1] * k[1]);
            pad.down(1, d.cx + k[0] / n * r, d.cy + k[1] / n * r);
            assertEquals(k[2], pad.state().buttons);
            pad.up(1);
        }
        pad.down(1, d.cx, d.cy);
        assertEquals(0, pad.state().buttons);   // the centre presses nothing
        pad.move(1, d.cx + d.r * 3, d.cy);      // a finger that landed on it keeps it
        assertEquals(TouchPad.XB_RIGHT, pad.state().buttons);
    }

    @Test
    public void slideBetweenButtonsTriggersAndSystem() {
        TouchPad.Control x = c(TouchPad.X), y = c(TouchPad.Y);
        pad.down(3, x.cx, x.cy);
        assertEquals(1 << TouchPad.AN_X, pad.state().analogMask);
        pad.move(3, y.cx, y.cy);
        assertEquals(1 << TouchPad.AN_Y, pad.state().analogMask);
        pad.move(3, W / 2, H / 2);              // off every button: nothing
        assertEquals(new TouchPad.State(), pad.state());
        pad.up(3);

        pad.down(0, c(TouchPad.LT).cx, c(TouchPad.LT).cy);
        pad.down(1, c(TouchPad.RT).cx, c(TouchPad.RT).cy);
        pad.down(2, c(TouchPad.START).cx, c(TouchPad.START).cy);
        pad.down(3, c(TouchPad.L3).cx, c(TouchPad.L3).cy);
        pad.down(4, c(TouchPad.WHITE).cx, c(TouchPad.WHITE).cy);
        pad.down(6, c(TouchPad.BLACK).cx, c(TouchPad.BLACK).cy);
        TouchPad.State s = pad.state();
        assertEquals(255, s.lt);
        assertEquals(255, s.rt);
        assertEquals(TouchPad.XB_START | TouchPad.XB_LTHUMB, s.buttons);
        assertEquals((1 << TouchPad.AN_WHITE) | (1 << TouchPad.AN_BLACK), s.analogMask);
        pad.cancel();
        assertEquals(new TouchPad.State(), pad.state());
        assertEquals(0, pad.fingers());
    }

    @Test
    public void layoutStaysOnScreenAndOffTheMenuEdge() {
        for (float scale : new float[] { 0.8f, 1f, 1.25f }) {
            for (float[] wh : new float[][] { { 2400, 1080 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 960 } }) {
                pad.layout(wh[0], wh[1], scale);
                for (TouchPad.Control k : pad.controls) {
                    float hw = k.pill() ? k.hw : k.r, hh = k.pill() ? k.hh : k.r;
                    String what = "control " + k.id + " at " + scale + " on " + wh[0] + "x" + wh[1];
                    assertTrue(what, k.cx - hw >= wh[0] * TouchPad.EDGE_FRAC);
                    assertTrue(what, k.cx + hw <= wh[0] && k.cy - hh >= 0 && k.cy + hh <= wh[1]);
                }
                // No two controls overlap (as drawn).
                for (TouchPad.Control p : pad.controls)
                    for (TouchPad.Control q : pad.controls)
                        if (p != q && !p.pill() && !q.pill()) {
                            float dx = p.cx - q.cx, dy = p.cy - q.cy;
                            assertTrue(p.id + "/" + q.id, Math.sqrt(dx * dx + dy * dy) >= p.r + q.r);
                        }
            }
        }
    }

    @Test
    public void visibility() {
        assertTrue(TouchSettings.visible(TouchSettings.Mode.AUTO, false, false, false));
        assertFalse(TouchSettings.visible(TouchSettings.Mode.AUTO, true, false, false));
        assertFalse(TouchSettings.visible(TouchSettings.Mode.AUTO, false, true, false));
        assertTrue(TouchSettings.visible(TouchSettings.Mode.ON, true, true, false));
        assertFalse(TouchSettings.visible(TouchSettings.Mode.ON, false, false, true));   // the Thor's panel
        assertFalse(TouchSettings.visible(TouchSettings.Mode.OFF, false, false, false));
    }

    @Test
    public void settingsText() {
        TouchSettings s = TouchSettings.parse("on 70 125");
        assertEquals(TouchSettings.Mode.ON, s.mode);
        assertEquals(70, s.opacity);
        assertEquals(125, s.size);
        assertEquals("ON 70 125", s.format());
        assertTrue(TouchSettings.parse(null).isDefault());
        assertEquals(30, TouchSettings.parse("AUTO 10").opacity);   // nearest choice
        assertEquals(TouchSettings.Mode.AUTO, TouchSettings.parse("bogus 70").mode);
        assertEquals(100, TouchSettings.step(TouchSettings.OPACITIES, 70, 1));
        assertEquals(30, TouchSettings.step(TouchSettings.OPACITIES, 100, 1));
    }

    /** What the overlay gets when the companion panel and the on-screen pad both hold something. */
    @Test
    public void mixer() {
        final int[] got = new int[8];
        final boolean[] off = { false };
        OverlayMixer m = new OverlayMixer(new OverlayMixer.Sink() {
            @Override
            public void pad(int buttons, int analogMask, int lt, int rt, int lx, int ly, int rx, int ry) {
                int[] v = { buttons, analogMask, lt, rt, lx, ly, rx, ry };
                System.arraycopy(v, 0, got, 0, 8);
                off[0] = false;
            }

            @Override
            public void off() {
                off[0] = true;
            }
        });
        m.panel(TouchPad.XB_BACK, 1 << TouchPad.AN_WHITE);
        TouchPad.State t = new TouchPad.State();
        t.buttons = TouchPad.XB_UP;
        t.analogMask = 1 << TouchPad.AN_A;
        t.lt = 255;
        t.lx = -100;
        m.touch(t);
        assertEquals(TouchPad.XB_BACK | TouchPad.XB_UP, got[0]);
        assertEquals((1 << TouchPad.AN_WHITE) | (1 << TouchPad.AN_A), got[1]);
        assertEquals(255, got[2]);
        assertEquals(-100, got[4]);
        m.panelOff();
        assertEquals(TouchPad.XB_UP, got[0]);
        assertFalse(off[0]);
        m.touchOff();
        assertTrue(off[0]);
        assertFalse(m.on());
    }
}
