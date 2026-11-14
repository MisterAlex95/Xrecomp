package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * A game's own Vulkan driver choice (VkDrivers.gameChoice / setGameChoice / resolve: per game, then Settings, then the
 * system driver), the reset when a driver is deleted, the per-driver status files of the native runtime, the row
 * helpers of VkDriverStore (the choices a game cycles through, the download names). Synthetic packages only.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class VkDriverPerGameTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    private static final String GAME = "n64-NZLP-B2055FBD", OTHER = "n64-NSME-12345678";
    private Context c;
    private File files;
    private VkDrivers vk;

    @Before
    public void setUp() throws IOException {
        c = RuntimeEnvironment.getApplication();
        files = tmp.newFolder("files");
        vk = new VkDrivers(files, 33);
    }

    private VkDrivers.Driver install(String name) throws IOException {
        Map<String, byte[]> m = VkDriversTest.good();
        m.put("meta.json", VkDriversTest.meta("vulkan.test.so", 30).replace("Turnip Test T1", name)
                                         .getBytes(StandardCharsets.UTF_8));
        return vk.install(new ByteArrayInputStream(VkDriversTest.zip(m)), VkDrivers.Origin.manual());
    }

    private void write(String rel, String text) throws IOException {
        File f = new File(files, rel);
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private String read(String rel) throws IOException {
        return new String(Files.readAllBytes(new File(files, rel).toPath()), StandardCharsets.UTF_8).trim();
    }

    // ---- Storage and resolution ------------------------------------------------------------------------------------

    @Test
    public void gameChoiceIsStoredNextToTheGlobalOne() throws IOException {
        VkDrivers.Driver d = install("T30");
        assertNull(vk.gameChoice(GAME));
        assertTrue(vk.setGameChoice(GAME, d.id));
        assertEquals(d.id, read("settings/vk_driver_N64-NZLP-B2055FBD"));
        assertEquals(d.id, vk.gameChoice(GAME));
        assertEquals(d.id, vk.gameChoice(GAME.toLowerCase()));   // the key's case does not matter
        assertNull(vk.gameChoice(OTHER));
        assertNull(vk.activeId());                               // the global choice is untouched
        assertTrue(vk.setGameChoice(GAME, VkDrivers.SYSTEM));
        assertEquals("system", read("settings/vk_driver_N64-NZLP-B2055FBD"));
        assertEquals(VkDrivers.SYSTEM, vk.gameChoice(GAME));
        assertTrue(vk.setGameChoice(GAME, null));
        assertNull(vk.gameChoice(GAME));
        // Refused: a driver not installed, a key that cannot name a file.
        assertFalse(vk.setGameChoice(GAME, "0123456789abcdef"));
        assertFalse(vk.setGameChoice("../x", d.id));
        assertFalse(vk.setGameChoice(null, d.id));
        assertFalse(vk.setGameChoice("", d.id));
        // A line that is neither an id nor « system » follows Settings.
        write("settings/vk_driver_N64-NZLP-B2055FBD", "nonsense\n");
        assertNull(vk.gameChoice(GAME));
    }

    @Test
    public void resolveGoesGameThenSettingsThenSystem() throws IOException {
        VkDrivers.Driver a = install("A"), b = install("B");
        assertNull(vk.resolve(GAME));                 // nothing chosen: the system driver
        vk.setActive(a.id);
        assertEquals(a.id, vk.resolve(GAME).id);      // Settings' choice
        assertEquals(a.id, vk.resolve(null).id);
        vk.setGameChoice(GAME, b.id);
        assertEquals(b.id, vk.resolve(GAME).id);      // the game's own
        assertEquals(a.id, vk.resolve(OTHER).id);     // another game: Settings'
        vk.setGameChoice(GAME, VkDrivers.SYSTEM);
        assertNull(vk.resolve(GAME));                 // the system driver whatever Settings says
        vk.clearActive();
        vk.setGameChoice(GAME, b.id);
        assertEquals(b.id, vk.resolve(GAME).id);      // a game's driver with Settings on the system one
        // A game's id no longer installed (removed by hand): Settings' choice.
        vk.setActive(a.id);
        write("settings/vk_driver_N64-NZLP-B2055FBD", "0123456789abcdef\n");
        assertEquals(a.id, vk.resolve(GAME).id);
    }

    @Test
    public void choosingForAGameForgetsAFailedTrial() throws IOException {
        VkDrivers.Driver d = install("T30");
        write("vk_drivers/trying", d.id + "\n");
        assertTrue(vk.failedLastStart(d.id));
        vk.setGameChoice(GAME, VkDrivers.SYSTEM);
        assertTrue(vk.failedLastStart(d.id));         // another choice: the marker stays
        vk.setGameChoice(GAME, d.id);
        assertFalse(vk.failedLastStart(d.id));        // chosen again: tried again
    }

    @Test
    public void deletingADriverResetsTheGamesThatChoseIt() throws IOException {
        VkDrivers.Driver a = install("A"), b = install("B");
        vk.setActive(a.id);
        vk.setGameChoice(GAME, a.id);
        vk.setGameChoice(OTHER, b.id);
        vk.setGameChoice("n64-NTHR-00000000", VkDrivers.SYSTEM);
        write("vk_drivers/" + a.id + ".status", "unsupported\n");
        assertTrue(vk.delete(a.id));
        assertNull(vk.activeId());
        assertNull(vk.gameChoice(GAME));                                    // back to Settings
        assertFalse(new File(files, "settings/vk_driver_N64-NZLP-B2055FBD").exists());
        assertEquals(b.id, vk.gameChoice(OTHER));                           // another driver's game: kept
        assertEquals(VkDrivers.SYSTEM, vk.gameChoice("n64-NTHR-00000000"));
        assertFalse(new File(files, "vk_drivers/" + a.id + ".status").exists());
        assertEquals(1, vk.list().size());   // a status file is never listed as a driver
    }

    // ---- The runtime's statuses ------------------------------------------------------------------------------------

    @Test
    public void statusParsing() {
        assertEquals("ok", VkDrivers.parseStatus("ok\n", null));
        assertEquals("unsupported", VkDrivers.parseStatus("0123456789abcdef unsupported\n", "0123456789abcdef"));
        assertNull(VkDrivers.parseStatus("0123456789abcdef ok", "fedcba9876543210"));
        assertNull(VkDrivers.parseStatus("0123456789abcdef", "0123456789abcdef"));
        assertNull(VkDrivers.parseStatus("ok ok", null));
        assertNull(VkDrivers.parseStatus("", null));
        assertNull(VkDrivers.parseStatus(null, null));
        assertNull(VkDrivers.parseStatus("../evil", null));
        String[][] words = { { "ok", "OK" }, { "crashed", "CRASHED" }, { "unsupported", "UNSUPPORTED" },
                             { "open_failed", "OPEN_FAILED" }, { "failed", "FAILED" }, { "not_loaded", "NOT_LOADED" },
                             { "something_new", "UNKNOWN" } };
        for (String[] w : words)
            assertEquals(w[0], VkDriverStore.LastStart.valueOf(w[1]), VkDriverStore.parseLastStart(w[0]));
        assertEquals(VkDriverStore.LastStart.UNKNOWN, VkDriverStore.parseLastStart(null));
    }

    @Test
    public void perDriverStatusFirstThenLastStart() throws IOException {
        VkDrivers.Driver a = install("A"), b = install("B");
        // An old runtime: last_start only, « failed » still read.
        write("vk_drivers/last_start", a.id + " failed\n");
        assertEquals(VkDriverStore.LastStart.FAILED, VkDriverStore.lastStart(vk, a.id));
        assertEquals(VkDriverStore.LastStart.UNKNOWN, VkDriverStore.lastStart(vk, b.id));
        // Today's: each driver's own file; last_start names only the last one, the other keeps its outcome.
        write("vk_drivers/" + a.id + ".status", "unsupported\n");
        write("vk_drivers/" + b.id + ".status", "ok\n");
        write("vk_drivers/last_start", b.id + " ok\n");
        assertEquals(VkDriverStore.LastStart.UNSUPPORTED, VkDriverStore.lastStart(vk, a.id));
        assertEquals(VkDriverStore.LastStart.OK, VkDriverStore.lastStart(vk, b.id));
        write("vk_drivers/" + b.id + ".status", "open_failed\n");
        assertEquals(VkDriverStore.LastStart.OPEN_FAILED, VkDriverStore.lastStart(vk, b.id));
        // The trial marker still wins.
        write("vk_drivers/trying", a.id + "\n");
        assertEquals(VkDriverStore.LastStart.CRASHED, VkDriverStore.lastStart(vk, a.id));
    }

    @Test
    public void warningsAndBadges() throws IOException {
        assertEquals(R.string.VKD_LAST_UNSUPPORTED,
                     VkDriverStore.lastStartWarning(VkDriverStore.LastStart.UNSUPPORTED));
        assertEquals(R.string.VKD_LAST_OPEN_FAILED,
                     VkDriverStore.lastStartWarning(VkDriverStore.LastStart.OPEN_FAILED));
        assertEquals(R.string.VKD_LAST_FAILED, VkDriverStore.lastStartWarning(VkDriverStore.LastStart.FAILED));
        assertTrue(c.getString(R.string.VKD_LAST_UNSUPPORTED).startsWith("Not compatible with the N64 renderer"));
        assertEquals(R.string.VKD_UNSUPPORTED_BADGE,
                     VkDriverStore.lastStartBadge(VkDriverStore.LastStart.UNSUPPORTED, false));
        assertEquals(R.string.VKD_ACTIVE_UNSUPPORTED_BADGE,
                     VkDriverStore.lastStartBadge(VkDriverStore.LastStart.UNSUPPORTED, true));
        assertEquals(R.string.VKD_WARN_BADGE, VkDriverStore.lastStartBadge(VkDriverStore.LastStart.CRASHED, false));
        assertEquals(0, VkDriverStore.lastStartBadge(VkDriverStore.LastStart.OK, true));
        assertEquals(0, VkDriverStore.lastStartBadge(VkDriverStore.LastStart.UNKNOWN, false));

        // The warning follows the driver a game starts with, not last_start.
        VkDrivers.Driver bad = install("R8"), good = install("T30");
        write("vk_drivers/" + bad.id + ".status", "unsupported\n");
        write("vk_drivers/" + good.id + ".status", "ok\n");
        write("vk_drivers/last_start", good.id + " ok\n");
        vk.setActive(good.id);
        assertEquals(0, VkDriverStore.effectiveWarning(vk, GAME));
        vk.setGameChoice(GAME, bad.id);
        assertEquals(R.string.VKD_LAST_UNSUPPORTED, VkDriverStore.effectiveWarning(vk, GAME));
        assertEquals(0, VkDriverStore.effectiveWarning(vk, null));
        vk.setGameChoice(GAME, VkDrivers.SYSTEM);
        assertEquals(0, VkDriverStore.effectiveWarning(vk, GAME));
    }

    // ---- The rows --------------------------------------------------------------------------------------------------

    @Test
    public void gameRowCyclesFollowSystemThenEachDriver() throws IOException {
        VkDrivers.Driver a = install("A"), b = install("B");
        List<String> ch = VkDriverStore.gameChoices(vk.list());
        assertEquals(Arrays.asList(null, VkDrivers.SYSTEM, a.id, b.id), ch);
        assertEquals(VkDrivers.SYSTEM, VkDriverStore.nextGameChoice(ch, null, 1));
        assertEquals(b.id, VkDriverStore.nextGameChoice(ch, null, -1));
        assertNull(VkDriverStore.nextGameChoice(ch, b.id, 1));
        assertEquals(VkDrivers.SYSTEM, VkDriverStore.nextGameChoice(ch, "0123456789abcdef", 1));   // gone: as null

        assertEquals(VkDrivers.SYSTEM, VkDriverStore.stepGameChoice(vk, GAME, 1));
        assertEquals(a.id, VkDriverStore.stepGameChoice(vk, GAME, 1));
        assertEquals(b.id, VkDriverStore.stepGameChoice(vk, GAME, 1));
        assertNull(VkDriverStore.stepGameChoice(vk, GAME, 1));
        assertEquals(b.id, VkDriverStore.stepGameChoice(vk, GAME, -1));
        assertNull(vk.activeId());

        // Labels: « As in Settings (…) » names the global driver.
        assertEquals("As in Settings (System)", VkDriverStore.gameChoiceLabel(c, vk, null));
        vk.setActive(a.id);
        assertEquals("As in Settings (" + VkDriverStore.label(a) + ")", VkDriverStore.gameChoiceLabel(c, vk, null));
        assertEquals("System", VkDriverStore.gameChoiceLabel(c, vk, VkDrivers.SYSTEM));
        assertEquals(VkDriverStore.label(b), VkDriverStore.gameChoiceLabel(c, vk, b.id));
    }

    @Test
    public void downloadNamesWithoutTheirSuffixes() {
        assertEquals("turnip_mrpurple_T29-toasted",
                     VkDriverStore.displayName("turnip_mrpurple_T29-toasted.adpkg.zip"));
        assertEquals("Turnip_v26.0.0_R8", VkDriverStore.displayName("Turnip_v26.0.0_R8.zip"));
        assertEquals("t", VkDriverStore.displayName("t.ADPKG.ZIP"));
        assertEquals("plain", VkDriverStore.displayName("plain"));
        assertEquals(".zip", VkDriverStore.displayName(".zip"));   // never an empty name
        VkDriverIndex.Entry e1 = entry("T29", "turnip_T.adpkg.zip"), e2 = entry("T30", "turnip_T.zip"),
                            e3 = entry("T30", "other.adpkg.zip");
        // « turnip_T » twice (once with .adpkg): both keep their tag; the third does not need one.
        assertEquals(Arrays.asList("turnip_T · T29", "turnip_T · T30", "other"),
                     VkDriverStore.rowNames(Arrays.asList(e1, e2, e3)));
        assertNotEquals(VkDriverStore.rowNames(Arrays.asList(e1)), VkDriverStore.rowNames(Arrays.asList(e1, e2)));
    }

    private static VkDriverIndex.Entry entry(String tag, String asset) {
        return new VkDriverIndex.Entry("MrPurple666/purple-turnip", tag, "", false, 1, asset, 10, null,
                                       "https://github.com/x");
    }
}
