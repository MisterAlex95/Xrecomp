package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.content.Intent;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;

/**
 * The N64 shader warm-up's triggers (ShaderWarmup): the decisions (run, kept for later while a game runs, already
 * warm for this app version, a driver whose last start failed, not an N64 game), the factor and driver it warms for a
 * game, its record, the pending request, the app update. The service itself (native, Vulkan) is not run here: its
 * start is seen as an intent.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ShaderWarmupTest {
    private static final String GAME = "n64-NZLP-B2055FBD", GBA = "gba-AXVE-01";
    private Application c;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
    }

    // ---- Pure ------------------------------------------------------------------------------------------------------

    @Test
    public void factorIsTheRuntimesReading() {
        assertEquals(1, ShaderWarmup.factor(null));
        assertEquals(1, ShaderWarmup.factor(""));
        assertEquals(1, ShaderWarmup.factor("native"));
        assertEquals(1, ShaderWarmup.factor("1"));
        assertEquals(2, ShaderWarmup.factor("2"));
        assertEquals(2, ShaderWarmup.factor(" 3 "));
        assertEquals(4, ShaderWarmup.factor("4"));
        assertEquals(8, ShaderWarmup.factor("8"));
        assertEquals(8, ShaderWarmup.factor("16"));
    }

    @Test
    public void decisions() {
        ShaderWarmup.Reason s = ShaderWarmup.Reason.SETTING;
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.decide(s, true, false, false, false));
        assertEquals(ShaderWarmup.Decision.PENDING, ShaderWarmup.decide(s, true, true, false, false));
        assertEquals(ShaderWarmup.Decision.SKIP_WARM, ShaderWarmup.decide(s, true, true, false, true));
        assertEquals(ShaderWarmup.Decision.SKIP_WARM, ShaderWarmup.decide(ShaderWarmup.Reason.PREPARE, true, false,
                                                                          false, true));
        assertEquals(ShaderWarmup.Decision.SKIP_DRIVER_FAILED, ShaderWarmup.decide(s, true, false, true, false));
        assertEquals(ShaderWarmup.Decision.SKIP_NOT_N64, ShaderWarmup.decide(s, false, false, false, false));
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.decide(ShaderWarmup.Reason.UPDATE, true, false, false,
                                                                    false));
    }

    @Test
    public void theRecordCountsForThisVersionOnly() {
        String done = ShaderWarmup.recordLine(42, new int[] { 0, 4, 13, 2, 9, 0, 1500 });
        assertEquals("42 0 13 9\n", done);
        assertTrue(ShaderWarmup.isWarm(done, 42));
        assertFalse(ShaderWarmup.isWarm(done, 43));              // an update: the shaders may have changed
        assertTrue(ShaderWarmup.isWarm("42 1 0 0\n", 42));       // the driver cannot run paraLLEl-RDP: not again
        assertFalse(ShaderWarmup.isWarm("42 2 0 0\n", 42));      // no cache directory or memory: again
        assertFalse(ShaderWarmup.isWarm(null, 42));
        assertFalse(ShaderWarmup.isWarm("", 42));
        assertFalse(ShaderWarmup.isWarm("x 0", 42));
    }

    @Test
    public void updated() {
        assertTrue(ShaderWarmup.updated(null, 5));
        assertTrue(ShaderWarmup.updated("4\n", 5));
        assertFalse(ShaderWarmup.updated("5\n", 5));
        assertTrue(ShaderWarmup.updated("junk", 5));
    }

    // ---- With a context --------------------------------------------------------------------------------------------

    @Test
    public void theTargetIsWhatTheGameWouldStartWith() {
        ShaderWarmup.Target t = ShaderWarmup.target(c, GAME);
        assertNull(t.driver);
        assertEquals(1, t.factor);
        assertTrue(ConsoleVideo.setGameChoice(c, GAME, ConsoleVideo.Setting.UPSCALE, ConsoleVideo.UPSCALE_8X));
        t = ShaderWarmup.target(c, GAME);
        assertEquals(8, t.factor);
        assertEquals(new File(c.getCacheDir(), "vk_cache/system/warmup-8x"), ShaderWarmup.recordFile(c.getCacheDir(),
                                                                                                     t));
        assertEquals(1, ShaderWarmup.target(c, "n64-NSME-12345678").factor);   // another game: Settings'
    }

    @Test
    public void aRequestStartsTheServiceOnceThenItIsWarm() {
        ConsoleVideo.setGameChoice(c, GAME, ConsoleVideo.Setting.UPSCALE, ConsoleVideo.UPSCALE_4X);
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.PREPARE));
        Intent i = shadowOf(c).getNextStartedService();
        assertEquals(ShaderWarmupService.class.getName(), i.getComponent().getClassName());
        assertEquals(GAME, i.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
        // the service wrote its record: the next request for the same driver and factor skips
        ShaderWarmup.Target t = ShaderWarmup.target(c, GAME);
        ShaderWarmup.write(ShaderWarmup.recordFile(c.getCacheDir(), t),
                           ShaderWarmup.recordLine(ShaderWarmup.versionCode(c), new int[] { 0, 4, 13, 9, 13, 0, 900 }));
        assertEquals(ShaderWarmup.Decision.SKIP_WARM, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.SETTING));
        assertNull(shadowOf(c).getNextStartedService());
        // another factor is not warm yet
        ConsoleVideo.setGameChoice(c, GAME, ConsoleVideo.Setting.UPSCALE, ConsoleVideo.UPSCALE_2X);
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.SETTING));
    }

    @Test
    public void notForAnotherConsole() {
        assertEquals(ShaderWarmup.Decision.SKIP_NOT_N64, ShaderWarmup.request(c, GBA, ShaderWarmup.Reason.PREPARE));
        assertNull(shadowOf(c).getNextStartedService());
    }

    @Test
    public void aDriverThatFailedIsSkipped() throws IOException {
        VkDrivers vk = VkDrivers.of(c);
        Map<String, byte[]> m = VkDriversTest.good();
        m.put("meta.json", VkDriversTest.meta("vulkan.test.so", 30).getBytes(StandardCharsets.UTF_8));
        VkDrivers.Driver d = vk.install(new ByteArrayInputStream(VkDriversTest.zip(m)), VkDrivers.Origin.manual());
        assertTrue(vk.setActive(d.id));
        ShaderWarmup.Target t = ShaderWarmup.target(c, GAME);
        assertEquals(d.id, t.driverLabel());
        assertEquals(new File(c.getCacheDir(), "vk_cache/" + d.id + "/warmup-1x"),
                     ShaderWarmup.recordFile(c.getCacheDir(), t));
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.SETTING));
        Files.write(new File(vk.root(), d.id + ".status").toPath(), "crashed\n".getBytes(StandardCharsets.UTF_8));
        assertEquals(ShaderWarmup.Decision.SKIP_DRIVER_FAILED,
                     ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.SETTING));
        Files.write(new File(vk.root(), d.id + ".status").toPath(), "ok\n".getBytes(StandardCharsets.UTF_8));
        assertEquals(ShaderWarmup.Decision.RUN, ShaderWarmup.request(c, GAME, ShaderWarmup.Reason.SETTING));
    }

    @Test
    public void aPendingRequestIsMadeOnceAtTheNextResume() {
        ShaderWarmup.keepPending(c, GAME);
        File pending = new File(c.getCacheDir(), "vk_cache/" + ShaderWarmup.PENDING);
        assertTrue(pending.isFile());
        ShaderWarmup.runPending(c);
        assertFalse(pending.exists());
        Intent i = shadowOf(c).getNextStartedService();
        assertEquals(GAME, i.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
        ShaderWarmup.runPending(c);                                      // consumed
        assertNull(shadowOf(c).getNextStartedService());
        ShaderWarmup.keepPending(c, null);                                // Settings' (no N64 game known)
        ShaderWarmup.runPending(c);
        i = shadowOf(c).getNextStartedService();
        assertNull(i.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
    }

    @Test
    public void afterAnUpdateTheLastN64GameIsWarmedOnce() {
        GameStore.rememberLaunch(c, null, "Zelda", GAME);
        ShaderWarmup.afterUpdate(c);                                     // no version stored: an update
        Intent i = shadowOf(c).getNextStartedService();
        assertEquals(GAME, i.getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
        ShaderWarmup.afterUpdate(c);                                     // the same version: nothing
        assertNull(shadowOf(c).getNextStartedService());
        File v = new File(c.getCacheDir(), "vk_cache/" + ShaderWarmup.VERSION);
        ShaderWarmup.write(v, "-1\n");                                   // another version
        ShaderWarmup.afterUpdate(c);
        assertEquals(GAME, shadowOf(c).getNextStartedService().getStringExtra(ShaderWarmup.EXTRA_TITLE_ID));
    }
}
