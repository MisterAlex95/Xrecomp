package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/**
 * The N64 start-up line's accounting (ShaderProgress, polled from n64_rdp_shader_progress): nothing for a start served
 * from the cache, a spinner while the total is unknown, « 7/13 » with a bar once it is, never more than the total,
 * and the end of the polling.
 */
public class ShaderProgressTest {
    private static final String PLAIN = "Compilation des shaders…", N = "Compilation des shaders… %1$d",
                                COUNT = "Compilation des shaders… %1$d/%2$d";

    private static int[] p(int active, int done, int total) {
        return new int[] { active, done, total };
    }

    @Test
    public void aStartFromTheCacheShowsNothing() {
        ShaderProgress sp = new ShaderProgress();
        assertSame(ShaderProgress.HIDDEN, sp.update(0, p(0, 0, 0)));      // the RDP not there yet
        assertEquals(ShaderProgress.Kind.HIDDEN, sp.update(100, p(1, 3, 13)).kind);
        assertEquals(ShaderProgress.Kind.HIDDEN, sp.update(200, p(1, 13, 13)).kind);
        assertFalse(sp.finished());
        assertEquals(ShaderProgress.Kind.HIDDEN, sp.update(300, p(0, 13, 0)).kind);   // over within 300 ms
        assertTrue(sp.finished());
        assertTrue(sp.seen());
        assertSame(ShaderProgress.HIDDEN, sp.update(400, p(1, 1, 13)));   // finished: no more frames
    }

    @Test
    public void aKnownTotalShowsABar() {
        ShaderProgress sp = new ShaderProgress();
        sp.update(1000, p(1, 0, 13));
        ShaderProgress.Frame f = sp.update(1000 + ShaderProgress.SHOW_AFTER_MS, p(1, 7, 13));
        assertEquals(ShaderProgress.Kind.BAR, f.kind);
        assertEquals(7, f.done);
        assertEquals(13, f.total);
        assertEquals("Compilation des shaders… 7/13", ShaderProgress.text(f, PLAIN, N, COUNT));
        assertEquals("Compiling shaders… 13/13",
                     ShaderProgress.text(sp.update(2000, p(1, 13, 13)), "Compiling shaders…", "Compiling shaders… %1$d",
                                         "Compiling shaders… %1$d/%2$d"));
        assertEquals(ShaderProgress.Kind.HIDDEN, sp.update(2100, p(0, 13, 0)).kind);
        assertTrue(sp.finished());
        assertEquals(13, sp.lastDone());
        assertEquals(13, sp.lastTotal());
    }

    @Test
    public void anUnknownTotalShowsASpinnerAndTheCount() {
        ShaderProgress sp = new ShaderProgress();
        sp.update(0, p(1, 0, 0));
        ShaderProgress.Frame f = sp.update(500, p(1, 0, 0));
        assertEquals(ShaderProgress.Kind.SPINNER, f.kind);
        assertEquals(PLAIN, ShaderProgress.text(f, PLAIN, N, COUNT));      // nothing built yet: no number
        f = sp.update(600, p(1, 4, 0));
        assertEquals(ShaderProgress.Kind.SPINNER, f.kind);
        assertEquals("Compilation des shaders… 4", ShaderProgress.text(f, PLAIN, N, COUNT));
    }

    @Test
    public void moreThanExpectedIsASpinnerNeverFourteenOfThirteen() {
        ShaderProgress sp = new ShaderProgress();
        sp.update(0, p(1, 0, 13));
        ShaderProgress.Frame f = sp.update(1000, p(1, 14, 13));   // an older native side, or a retry
        assertEquals(ShaderProgress.Kind.SPINNER, f.kind);
        assertEquals("Compilation des shaders… 14", ShaderProgress.text(f, PLAIN, N, COUNT));
        f = sp.update(1100, p(1, 15, 0));                          // the native rule (n64_vk_progress_total)
        assertEquals(ShaderProgress.Kind.SPINNER, f.kind);
    }

    @Test
    public void pollingStopsWhenNoStartComes() {
        ShaderProgress sp = new ShaderProgress();
        sp.update(0, null);                                          // no native side (not loaded yet, or none)
        assertFalse(sp.finished());
        sp.update(ShaderProgress.GIVE_UP_MS - 1, p(0, 0, 0));
        assertFalse(sp.finished());
        sp.update(ShaderProgress.GIVE_UP_MS, p(0, 0, 0));            // the software RDP: it never starts
        assertTrue(sp.finished());
        assertFalse(sp.seen());
    }

    @Test
    public void aShortArrayIsNoProgress() {
        ShaderProgress sp = new ShaderProgress();
        assertSame(ShaderProgress.HIDDEN, sp.update(0, new int[] { 1 }));
        assertEquals("", ShaderProgress.text(ShaderProgress.HIDDEN, PLAIN, N, COUNT));
    }
}
