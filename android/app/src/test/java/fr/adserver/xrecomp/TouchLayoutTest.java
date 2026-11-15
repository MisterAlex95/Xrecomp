package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * XBA-207: the player's on-screen pad layout (TouchLayout, TouchEdit, TouchPad's custom placement), pure Java: the
 * file format both ways, normalised places across screen sizes, a game's file over its console's, hidden controls
 * never hit, the editor's drag (on the grid), pinch, − / +, reset, and the defaults untouched until something is saved.
 */
public class TouchLayoutTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static TouchPad pad(TouchPad.Layout l, float w, float h) {
        TouchPad p = new TouchPad(l);
        p.layout(w, h, 1f);
        return p;
    }

    @Test
    public void formatAndParseRoundTrip() {
        TouchLayout t = new TouchLayout();
        TouchLayout.Entry a = new TouchLayout.Entry();
        a.x = 0.875f;
        a.y = 0.4630f;
        a.size = 1.2f;
        t.entries.put(TouchPad.A, a);
        TouchLayout.Entry sel = new TouchLayout.Entry();
        sel.hidden = true;
        t.entries.put(TouchPad.SELECT, sel);
        String text = t.format();
        assertTrue(text, text.contains("A x=0.8750 y=0.4630 size=1.20\n"));
        assertTrue(text, text.contains("SELECT hidden\n"));
        TouchLayout back = TouchLayout.parse(text);
        assertEquals(t, back);
        assertTrue(Float.isNaN(back.entries.get(TouchPad.SELECT).x));
    }

    @Test
    public void parseSkipsWhatItDoesNotKnow() {
        TouchLayout t = TouchLayout.parse("# comment\nNOPE x=0.1\nB x=abc y=2 size=9 shiny\n\nC_UP size=0.1\n");
        assertEquals(2, t.entries.size());
        TouchLayout.Entry b = t.entries.get(TouchPad.B);
        assertTrue(Float.isNaN(b.x));             // a bad number keeps the default place
        assertEquals(1f, b.y, 0f);                // clamped to the screen
        assertEquals(TouchLayout.MAX_SIZE, b.size, 0f);
        assertEquals(TouchLayout.MIN_SIZE, t.entries.get(TouchPad.C_UP).size, 0f);
        assertTrue(TouchLayout.parse(null).isEmpty());
        assertTrue(TouchLayout.parse("").isEmpty());
    }

    @Test
    public void everyControlHasAFileName() {
        for (TouchPad.Layout l : TouchPad.Layout.values())
            for (TouchPad.Control c : new TouchPad(l).controls) {
                String n = TouchLayout.name(c.id);
                assertNotNull(l + " " + c.id, n);
                assertEquals(c.id, TouchLayout.id(n));
            }
    }

    @Test
    public void untouchedPadCapturesNothing() {
        for (TouchPad.Layout l : TouchPad.Layout.values())
            assertTrue(l.name(), TouchLayout.capture(pad(l, 2400, 1080)).isEmpty());
    }

    @Test
    public void placesAreNormalisedAcrossScreenSizes() {
        TouchPad p = pad(TouchPad.Layout.XBOX, 2400, 1080);
        float defaultR = p.control(TouchPad.A).r;
        p.moveTo(p.control(TouchPad.A), 1200, 540);
        p.resize(p.control(TouchPad.A), 1.5f);
        TouchLayout t = TouchLayout.capture(p);
        assertEquals(1, t.entries.size());
        assertEquals(0.5f, t.entries.get(TouchPad.A).x, 1e-4f);
        assertEquals(0.5f, t.entries.get(TouchPad.A).y, 1e-4f);
        assertEquals(defaultR * 1.5f, p.control(TouchPad.A).r, 1e-3f);

        // Through the file, on a 4:3 screen of another size: the same share of the screen, the same size factor.
        TouchPad q = new TouchPad(TouchPad.Layout.XBOX);
        q.setCustom(TouchLayout.parse(t.format()));
        q.layout(1440, 1080, 1f);
        TouchPad.Control a = q.control(TouchPad.A);
        assertEquals(720, a.cx, 0.5f);
        assertEquals(540, a.cy, 0.5f);
        assertEquals(pad(TouchPad.Layout.XBOX, 1440, 1080).control(TouchPad.A).r * 1.5f, a.r, 1e-3f);
        // An untouched control keeps its default anchoring to the edges.
        assertEquals(pad(TouchPad.Layout.XBOX, 1440, 1080).control(TouchPad.B).cx, q.control(TouchPad.B).cx, 0f);
        // The global size setting still applies on top.
        q.layout(1440, 1080, 1.25f);
        TouchPad big = new TouchPad(TouchPad.Layout.XBOX);
        big.layout(1440, 1080, 1.25f);
        assertEquals(big.control(TouchPad.A).r * 1.5f, q.control(TouchPad.A).r, 1e-3f);
    }

    @Test
    public void placesStayOnScreenAndClearOfTheMenuSwipe() {
        TouchPad p = pad(TouchPad.Layout.GBA, 2400, 1080);
        TouchPad.Control d = p.control(TouchPad.DPAD);
        p.moveTo(d, -500, -500);
        assertTrue(d.cx - d.r >= 2400 * TouchPad.EDGE_FRAC - 0.01f);
        assertTrue(d.cy - d.r >= -0.01f);
        p.moveTo(d, 99999, 99999);
        assertTrue(d.cx + d.r <= 2400.01f);
        assertTrue(d.cy + d.r <= 1080.01f);
    }

    @Test
    public void hiddenControlsAreNotHitTested() {
        TouchPad p = pad(TouchPad.Layout.PS1, 2400, 1080);
        TouchPad.Control cross = p.control(TouchPad.A);
        TouchLayout.Entry e = new TouchLayout.Entry();
        e.hidden = true;
        TouchLayout t = new TouchLayout();
        t.entries.put(TouchPad.A, e);
        p.setCustom(t);
        assertTrue(cross.hidden);
        assertNull(p.at(cross.cx, cross.cy, true));
        p.down(0, cross.cx, cross.cy);
        assertEquals(new TouchPad.State(), p.state());
        p.up(0);
        // The editor still picks it (to show it again); the others still work.
        assertSame(cross, p.pick(cross.cx, cross.cy));
        TouchPad.Control circle = p.control(TouchPad.B);
        p.down(1, circle.cx, circle.cy);
        assertEquals(1 << TouchPad.AN_B, p.state().analogMask);
    }

    @Test
    public void gameLayoutWinsOverTheConsoles() throws Exception {
        File files = tmp.newFolder();
        TouchPad.Layout l = TouchPad.Layout.N64;
        assertTrue(TouchLayout.resolve(files, "N64-ABCD", l).layout.isEmpty());   // nothing saved: the defaults

        TouchLayout console = TouchLayout.parse("Z x=0.2 y=0.8\n");
        assertTrue(TouchLayout.save(files, null, l, console, false));
        TouchLayout.Resolved r = TouchLayout.resolve(files, "N64-ABCD", l);
        assertFalse(r.perGame);
        assertEquals(console, r.layout);
        assertTrue(TouchLayout.resolve(files, "N64-ABCD", TouchPad.Layout.GBA).layout.isEmpty());   // per console

        TouchLayout game = TouchLayout.parse("START hidden\n");
        assertTrue(TouchLayout.save(files, "n64-abcd", l, game, true));
        r = TouchLayout.resolve(files, "N64-ABCD", l);
        assertTrue(r.perGame);
        assertEquals(game, r.layout);
        assertEquals(console, TouchLayout.resolve(files, "N64-OTHER", l).layout);   // the others keep the console's
        assertEquals(console, TouchLayout.resolve(files, null, l).layout);

        // An empty game file: the defaults for this game even though the console has a layout.
        assertTrue(TouchLayout.save(files, "N64-ABCD", l, new TouchLayout(), true));
        r = TouchLayout.resolve(files, "N64-ABCD", l);
        assertTrue(r.perGame);
        assertTrue(r.layout.isEmpty());

        // Back to « all games »: the game's file goes, the console's is written.
        assertTrue(TouchLayout.save(files, "N64-ABCD", l, game, false));
        assertFalse(TouchLayout.gameFile(files, "N64-ABCD", l).exists());
        r = TouchLayout.resolve(files, "N64-ABCD", l);
        assertFalse(r.perGame);
        assertEquals(game, r.layout);
        String text = new String(Files.readAllBytes(TouchLayout.consoleFile(files, l).toPath()),
                                 StandardCharsets.UTF_8);
        assertTrue(text, text.contains("START hidden"));

        // Reset and saved for all games: no file left, the defaults.
        assertTrue(TouchLayout.save(files, null, l, new TouchLayout(), false));
        assertFalse(TouchLayout.consoleFile(files, l).exists());
        assertTrue(TouchLayout.resolve(files, "N64-ABCD", l).layout.isEmpty());
        assertFalse(TouchLayout.save(files, null, l, game, true));   // « this game only » needs a game
    }

    @Test
    public void editorDragsOnTheGrid() {
        TouchPad p = pad(TouchPad.Layout.XBOX, 2400, 1080);
        TouchEdit e = new TouchEdit(p);
        TouchPad.Control y = p.control(TouchPad.Y);
        float step = e.gridStep();
        assertEquals(27f, step, 0f);
        e.down(0, y.cx, y.cy);
        assertSame(y, e.selected);
        e.move(0, 1210, 600);
        assertEquals(0f, y.cx % step, 0.01f);
        assertEquals(0f, y.cy % step, 0.01f);
        assertEquals(1210, y.cx, step / 2 + 0.01f);
        e.up(0);
        e.grid = false;
        e.down(0, y.cx, y.cy);
        e.move(0, 1201, 599);
        assertEquals(1201, y.cx, 0.01f);
        assertEquals(599, y.cy, 0.01f);
        e.up(0);
        // A tap on an empty spot drops the selection.
        e.down(0, 1200, 900);
        assertNull(e.selected);
        e.up(0);
        TouchLayout t = e.layout();
        assertEquals(1201f / 2400, t.entries.get(TouchPad.Y).x, 1e-4f);
    }

    @Test
    public void editorPinchesAndSteps() {
        TouchPad p = pad(TouchPad.Layout.N64, 2400, 1080);
        TouchEdit e = new TouchEdit(p);
        TouchPad.Control a = p.control(TouchPad.A);
        float r = a.r, cx = a.cx, cy = a.cy;
        e.down(0, cx, cy);
        e.down(1, cx + 100, cy);
        e.move(1, cx + 200, cy);   // twice the distance: twice the size
        assertEquals(2f, a.size, 1e-3f);
        assertEquals(r * 2, a.r, 1e-3f);
        e.up(1);
        e.up(0);
        e.grow(-1);
        assertEquals(1.9f, a.size, 1e-3f);
        for (int i = 0; i < 40; i++)
            e.grow(-1);
        assertEquals(TouchLayout.MIN_SIZE, a.size, 1e-3f);
        e.toggleHidden();
        assertTrue(a.hidden);
        TouchLayout t = e.layout();
        assertTrue(t.entries.get(TouchPad.A).hidden);
        assertEquals(TouchLayout.MIN_SIZE, t.entries.get(TouchPad.A).size, 1e-3f);
        e.reset();
        assertSame(a, e.selected);
        assertFalse(a.hidden);
        assertEquals(r, a.r, 1e-3f);
        assertTrue(e.layout().isEmpty());
    }
}
