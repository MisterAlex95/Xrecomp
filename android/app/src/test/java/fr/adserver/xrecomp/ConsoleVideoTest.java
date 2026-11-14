package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * XBA-181: the shared console options. A GBA, PS1 or N64 game's own scale and filter (scale_&lt;KEY&gt;,
 * filter_&lt;KEY&gt;), the environment ShellActivity hands to the runtime (the game's choice, else recomp.cfg's line,
 * else unset; the shared vertical sync, audio buffer and rumble), Settings' « All consoles » / « Xbox only » grouping,
 * and the running game's title ID (a console game after an Xbox game is not the Xbox game).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ConsoleVideoTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String GBA = "gba-AXVE-1A2B3C4D";

    @Test
    public void contractNames() {
        assertEquals("RECOMP_SCALE", ConsoleVideo.ENV_SCALE);
        assertEquals("RECOMP_FILTER", ConsoleVideo.ENV_FILTER);
        assertEquals(List.of("aspect", "integer", "stretch"), List.of(ConsoleVideo.Setting.SCALE.values));
        assertEquals(List.of("nearest", "linear"), List.of(ConsoleVideo.Setting.FILTER.values));
        assertEquals(List.of("RECOMP_PRESENT_VSYNC", "RECOMP_AUDIO_BUF_SAMPLES", "RECOMP_RUMBLE"),
                     List.of(ConsoleVideo.SHARED_ENV));
        assertEquals("RECOMP_RUMBLE", RecompConfig.ENV_RUMBLE);
    }

    @Test
    public void choiceIsPerGame() {
        Context c = RuntimeEnvironment.getApplication();
        ConsoleVideo.Setting s = ConsoleVideo.Setting.SCALE, f = ConsoleVideo.Setting.FILTER;
        assertNull(ConsoleVideo.gameChoice(c, GBA, s));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA.toLowerCase(), s, "Integer"));
        assertEquals("integer", ConsoleVideo.gameChoice(c, GBA, s));
        assertTrue(new File(new File(c.getFilesDir(), "settings"), "scale_" + GBA.toUpperCase()).isFile());
        assertNull("the filter untouched", ConsoleVideo.gameChoice(c, GBA, f));
        assertNull("another game", ConsoleVideo.gameChoice(c, "gba-BPEE-00000000", s));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, f, "linear"));
        assertEquals("linear", ConsoleVideo.gameChoice(c, GBA, f));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, f, "bilinear"));   // not a value: follows Settings
        assertNull(ConsoleVideo.gameChoice(c, GBA, f));
        assertTrue(ConsoleVideo.setGameChoice(c, GBA, s, null));
        assertNull(ConsoleVideo.gameChoice(c, GBA, s));
        assertNull(ConsoleVideo.gameChoice(c, null, s));
        assertFalse(ConsoleVideo.setGameChoice(c, "", s, "aspect"));
    }

    @Test
    public void cycle() {
        ConsoleVideo.Setting s = ConsoleVideo.Setting.SCALE;
        assertEquals("aspect", ConsoleVideo.next(s, null, 1));
        assertEquals("integer", ConsoleVideo.next(s, "aspect", 1));
        assertEquals("stretch", ConsoleVideo.next(s, "integer", 1));
        assertNull(ConsoleVideo.next(s, "stretch", 1));
        assertEquals("stretch", ConsoleVideo.next(s, null, -1));
        assertEquals("linear", ConsoleVideo.next(ConsoleVideo.Setting.FILTER, null, -1));
        assertEquals(R.string.MENU_VIDEO_FOLLOW, ConsoleVideo.label(s, null));
        assertEquals(R.string.VAL_CSCALE_STRETCH, ConsoleVideo.label(s, "stretch"));
        assertEquals(R.string.VAL_CFILTER_NEAREST, ConsoleVideo.label(ConsoleVideo.Setting.FILTER, "nearest"));
    }

    private static String value(List<String[]> env, String key) {
        for (String[] kv : env)
            if (kv[0].equals(key))
                return kv[1];
        throw new AssertionError(key + " not in the environment");
    }

    private static List<String> keys(List<String[]> env) {
        List<String> out = new ArrayList<>();
        for (String[] kv : env)
            out.add(kv[0]);
        return out;
    }

    @Test
    public void runEnvGameThenSettingsThenUnset() throws Exception {
        File dir = tmp.newFolder("ext");
        RecompConfig cfg = RecompConfig.load(dir);
        // Nothing anywhere: everything unset (the runtime's defaults), and an unset value clears a previous game's.
        List<String[]> env = ConsoleVideo.runEnv(Console.PS1, null, null, cfg);
        assertEquals(List.of("RECOMP_SCALE", "RECOMP_FILTER", "RECOMP_PRESENT_VSYNC", "RECOMP_AUDIO_BUF_SAMPLES",
                             "RECOMP_RUMBLE"), keys(env));
        for (String[] kv : env)
            assertNull(kv[0], kv[1]);
        // Settings' lines.
        cfg.set("RECOMP_SCALE", "stretch");
        cfg.set("RECOMP_FILTER", " nearest ");
        cfg.set("RECOMP_PRESENT_VSYNC", "0");
        cfg.set("RECOMP_AUDIO_BUF_SAMPLES", "2048");
        cfg.set("RECOMP_RUMBLE", "0");
        env = ConsoleVideo.runEnv(Console.N64, null, null, cfg);
        assertEquals("stretch", value(env, "RECOMP_SCALE"));
        assertEquals("nearest", value(env, "RECOMP_FILTER"));
        assertEquals("0", value(env, "RECOMP_PRESENT_VSYNC"));
        assertEquals("2048", value(env, "RECOMP_AUDIO_BUF_SAMPLES"));
        assertEquals("0", value(env, "RECOMP_RUMBLE"));
        // The game's own choices win.
        env = ConsoleVideo.runEnv(Console.GBA, "integer", "linear", cfg);
        assertEquals("integer", value(env, "RECOMP_SCALE"));
        assertEquals("linear", value(env, "RECOMP_FILTER"));
        assertEquals("0", value(env, "RECOMP_PRESENT_VSYNC"));
        // An Xbox game: scale and filter unset, the shared ones left to recomp.cfg.
        env = ConsoleVideo.runEnv(Console.XBOX, "integer", "linear", cfg);
        assertEquals(List.of("RECOMP_SCALE", "RECOMP_FILTER"), keys(env));
        assertNull(value(env, "RECOMP_SCALE"));
        assertNull(value(env, "RECOMP_FILTER"));
        // No recomp.cfg at all (no external storage).
        env = ConsoleVideo.runEnv(Console.GBA, null, "nearest", null);
        assertNull(value(env, "RECOMP_SCALE"));
        assertEquals("nearest", value(env, "RECOMP_FILTER"));
    }

    private static List<Integer> titles(List<RecompConfig.Option> l) {
        List<Integer> out = new ArrayList<>();
        for (RecompConfig.Option o : l)
            out.add(o.title);
        return out;
    }

    /** Settings › Graphics and Audio: the options every console reads first, then the Xbox's own. */
    @Test
    public void settingsGroups() {
        List<RecompConfig.Option> shown = RecompConfig.shownOptions(false);
        List<RecompConfig.Option> g = RecompConfig.tabOptions(shown, RecompConfig.Section.GRAPHICS);
        assertTrue(RecompConfig.grouped(g, RecompConfig.Section.GRAPHICS));
        assertEquals(List.of(R.string.OPT_VSYNC, R.string.OPT_CSCALE, R.string.OPT_CFILTER),
                     titles(g).subList(0, 3));
        for (int i = 3; i < g.size(); i++)
            assertFalse(g.get(i).title + " is the Xbox's", g.get(i).shared);
        List<Integer> xbox = titles(g).subList(3, g.size());
        for (int t : new int[] { R.string.OPT_GPU, R.string.OPT_GPU_FPS_CAP, R.string.OPT_GPU_SCALE,
                                 R.string.OPT_ASPECT, R.string.OPT_WIDESCREEN, R.string.OPT_480P, R.string.OPT_TEX })
            assertTrue(xbox.contains(t));
        List<RecompConfig.Option> a = RecompConfig.tabOptions(shown, RecompConfig.Section.AUDIO);
        assertEquals(List.of(R.string.OPT_AUDIO_BUF, R.string.OPT_HRTF), titles(a));
        assertTrue(RecompConfig.grouped(a, RecompConfig.Section.AUDIO));
        // General and Advanced: as listed, no labels.
        List<RecompConfig.Option> gen = RecompConfig.tabOptions(shown, RecompConfig.Section.GAME);
        assertFalse(RecompConfig.grouped(gen, RecompConfig.Section.GAME));
        assertTrue(titles(gen).contains(R.string.OPT_RUMBLE));
        assertFalse(RecompConfig.grouped(RecompConfig.tabOptions(shown, RecompConfig.Section.ADVANCED),
                                         RecompConfig.Section.ADVANCED));
    }

    /** The Settings rows write the contract's values; the default writes no line. */
    @Test
    public void settingsValues() throws Exception {
        RecompConfig cfg = RecompConfig.load(tmp.newFolder("cfg"));
        for (RecompConfig.Option o : RecompConfig.OPTIONS) {
            if (o.title == R.string.OPT_CSCALE) {
                assertEquals(0, o.defaultIndex());
                cfg.set(o, o.choices[2]);
                assertEquals("stretch", cfg.get("RECOMP_SCALE"));
                cfg.set(o, o.choices[1]);
                assertEquals("integer", cfg.get("RECOMP_SCALE"));
            } else if (o.title == R.string.OPT_CFILTER) {
                cfg.set(o, o.choices[1]);
                assertEquals("nearest", cfg.get("RECOMP_FILTER"));
                cfg.set(o, o.choices[2]);
                assertEquals("linear", cfg.get("RECOMP_FILTER"));
                cfg.set(o, o.choices[0]);
                assertNull("Console default", cfg.get("RECOMP_FILTER"));
            } else if (o.title == R.string.OPT_RUMBLE) {
                cfg.set(o, o.choices[1]);
                assertEquals("0", cfg.get("RECOMP_RUMBLE"));
            }
        }
    }

    /** The bug of XBA-181: during a GBA game launched after an Xbox game, the title ID is the GBA game's key. */
    @Test
    public void runningTitleIdAfterAnXboxGame() throws Exception {
        Context c = RuntimeEnvironment.getApplication();
        File dir = new File(c.getFilesDir(), "xbox-game");
        TestSupport.write(new File(dir, "default.xbe"), TestSupport.xbe(0x4D5300D1, "Game"));
        GameStore.remember(c, dir.getPath(), "Game");
        assertEquals("4D5300D1", GameLanguage.runningTitleId(c));
        assertTrue(GameStore.rememberRomLaunch(c, "/sdcard/roms/quest.gba", "Pocket Quest", GBA));
        assertEquals("the chosen Xbox folder stays", dir.getPath(), GameStore.chosenDir(c));
        assertEquals(GBA, GameLanguage.runningTitleId(c));
        assertEquals("4D5300D1", GameLanguage.currentTitleId(c));
        GameStore.rememberLaunch(c, dir.getPath(), "Game", "4D5300D1");   // an Xbox launch clears the ROM
        assertEquals("4D5300D1", GameLanguage.runningTitleId(c));
    }
}
