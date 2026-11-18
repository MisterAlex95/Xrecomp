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
import java.util.Locale;

/**
 * XBA-188: a game's own settings (GameSettings): game &gt; Settings (recomp.cfg) &gt; the runtime's default for every
 * key, the environment ShellActivity exports, the migration of the earlier per-game files, the rows per console.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GameSettingsTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String XBOX = "4D5300D1", GBA = "gba-AXVE-1A2B3C4D", PS1 = "ps1-SLUS-00001",
                                N64 = "n64-NZLP-B2055FBD";

    private static RecompConfig.Option option(int title) {
        for (RecompConfig.Option o : RecompConfig.OPTIONS)
            if (o.title == title)
                return o;
        throw new AssertionError("no option " + title);
    }

    private RecompConfig cfg(String... kv) throws Exception {
        File d = tmp.newFolder();
        RecompConfig c = RecompConfig.load(d);
        for (int i = 0; i < kv.length; i += 2)
            c.set(kv[i], kv[i + 1]);
        return c;
    }

    private RecompConfig game(File files, String key, String... kv) {
        RecompConfig g = GameSettings.load(files, key);
        for (int i = 0; i < kv.length; i += 2)
            g.set(kv[i], kv[i + 1]);
        assertTrue(GameSettings.save(g));
        return GameSettings.load(files, key);
    }

    private static List<String> titles(List<RecompConfig.Option> os) {
        List<String> out = new ArrayList<>();
        for (RecompConfig.Option o : os)
            out.add(RuntimeEnvironment.getApplication().getResources().getResourceEntryName(o.title));
        return out;
    }

    // ---- Resolution order ------------------------------------------------------------------------------------------

    @Test
    public void gameThenSettingsThenDefault() throws Exception {
        File files = tmp.newFolder();
        RecompConfig global = cfg("RECOMP_PRESENT_VSYNC", "0", "RECOMP_GPU_FPS_CAP", "30", "RECOMP_APU_HRTF", "0");
        RecompConfig g = game(files, XBOX, "RECOMP_GPU_FPS_CAP", "60", "RECOMP_APU_HRTF", "", "RECOMP_ASPECT", "4:3");
        assertEquals("game", "60", GameSettings.effective(global, g, "RECOMP_GPU_FPS_CAP"));
        assertEquals("Settings", "0", GameSettings.effective(global, g, "RECOMP_PRESENT_VSYNC"));
        assertNull("the game's default over Settings' line", GameSettings.effective(global, g, "RECOMP_APU_HRTF"));
        assertEquals("4:3", GameSettings.effective(global, g, "RECOMP_ASPECT"));
        assertNull("neither: the runtime's default", GameSettings.effective(global, g, "RECOMP_GPU_SCALE"));

        GameSettings.Env env = GameSettings.runEnv(Console.XBOX, global, g, Locale.US, false);
        assertEquals("60", env.value("RECOMP_GPU_FPS_CAP"));
        assertEquals("4:3", env.value("RECOMP_ASPECT"));
        assertNull("followed: unset, recomp.cfg decides natively", env.value("RECOMP_PRESENT_VSYNC"));
        assertNull("the game's default: unset", env.value("RECOMP_APU_HRTF"));
        assertEquals("and recomp.cfg's line skipped", "RECOMP_APU_HRTF", env.value(GameSettings.SKIP_ENV));
        assertEquals(List.of("RECOMP_GPU_FPS_CAP", "RECOMP_ASPECT", "RECOMP_APU_HRTF").size(), env.overridden.size());
        assertTrue(env.logLine(XBOX).contains("RECOMP_GPU_FPS_CAP=60"));
        assertTrue(env.logLine(XBOX).contains("RECOMP_APU_HRTF=(default)"));
    }

    /** What Game settings' tabs and Home's pill count: options that differ from Settings, then the other keys. */
    @Test
    public void ownCountsWhatDiffersAndTheOtherKeys() throws Exception {
        File files = tmp.newFolder();
        RecompConfig global = cfg("RECOMP_GPU_FPS_CAP", "60");
        // Frame cap as Settings' (not counted), vsync off and widescreen default (counted), its own mapping and theme.
        RecompConfig g = game(files, XBOX, "RECOMP_GPU_FPS_CAP", "60", "RECOMP_PRESENT_VSYNC", "0",
                              "RECOMP_WIDESCREEN", "1", "RECOMP_PAD_MAP", "a=b", GameSettings.THEME_KEY, "off");
        List<RecompConfig.Option> opts = GameSettings.options(Console.XBOX, false);
        assertEquals(2, GameSettings.ownCount(global, g, opts));
        assertEquals(2, GameSettings.ownExtras(g, Console.XBOX));
        assertEquals(4, GameSettings.ownTotal(global, g, Console.XBOX, false));
        assertEquals(0, GameSettings.ownTotal(global, GameSettings.load(files, GBA), Console.GBA, false));
    }

    @Test
    public void everyKeyIsSetOrUnsetSoNoPreviousGameStays() throws Exception {
        GameSettings.Env env = GameSettings.runEnv(Console.GBA, cfg(), GameSettings.load(tmp.newFolder(), GBA),
                                                   Locale.US, true);
        List<String> keys = new ArrayList<>();
        for (String[] kv : env.vars)
            keys.add(kv[0]);
        for (String k : GameSettings.allExportedKeys())
            assertTrue(k, keys.contains(k));
        assertTrue(keys.contains("RECOMP_PAD_MAP") && keys.contains("RECOMP_N64_PAK"));
        assertTrue(keys.contains(GameSettings.SKIP_ENV));
        assertNull(env.value(GameSettings.SKIP_ENV));
        assertTrue(env.overridden.isEmpty());
        assertTrue(env.logLine(GBA).endsWith("none (as in Settings)"));
    }

    @Test
    public void keysThatDoNotApplyAreNotExported() throws Exception {
        File files = tmp.newFolder();
        // Hand-written lines for another console: an Xbox title gets neither the console picture nor a GBA mapping.
        RecompConfig g = game(files, XBOX, "RECOMP_SCALE", "integer", "RECOMP_GBA_PAD_MAP", "a=b", "RECOMP_WIDESCREEN", "1");
        GameSettings.Env env = GameSettings.runEnv(Console.XBOX, cfg(), g, Locale.US, false);
        assertNull(env.value("RECOMP_SCALE"));
        assertNull(env.value("RECOMP_GBA_PAD_MAP"));
        assertEquals("1", env.value("RECOMP_WIDESCREEN"));
        RecompConfig p = game(files, PS1, "RECOMP_WIDESCREEN", "1", "RECOMP_UPSCALE", "4", "RECOMP_RUMBLE", "0");
        env = GameSettings.runEnv(Console.PS1, cfg(), p, Locale.US, false);
        assertNull("the Xbox's own", env.value("RECOMP_WIDESCREEN"));
        assertEquals("4", env.value("RECOMP_UPSCALE"));
        assertEquals("shared", "0", env.value("RECOMP_RUMBLE"));
    }

    /** XBA-237: an Xbox game's own internal resolution is exported (its GPU renderer reads RECOMP_UPSCALE when
     *  RECOMP_GPU_SCALE is unset); its « Native » unsets it and skips Settings' line. */
    @Test
    public void xboxInternalResolution() throws Exception {
        File files = tmp.newFolder();
        RecompConfig g = game(files, XBOX, "RECOMP_UPSCALE", "2");
        GameSettings.Env env = GameSettings.runEnv(Console.XBOX, cfg(), g, Locale.US, false);
        assertEquals("2", env.value("RECOMP_UPSCALE"));
        assertTrue(env.overridden.contains("RECOMP_UPSCALE"));
        RecompConfig global = cfg();
        global.set("RECOMP_UPSCALE", "4");
        RecompConfig n = game(tmp.newFolder(), XBOX, "RECOMP_UPSCALE", "");
        env = GameSettings.runEnv(Console.XBOX, global, n, Locale.US, false);
        assertNull(env.value("RECOMP_UPSCALE"));
        assertTrue(env.value(GameSettings.SKIP_ENV).contains("RECOMP_UPSCALE"));
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.UPSCALE, Console.XBOX));
    }

    @Test
    public void pictureShaderPerGame() throws Exception {
        // XBA-195: a console game's own shader over Settings', its « None » over Settings' line; XBA-203: an Xbox
        // game's too, but not its LCD ghosting (no LCD grid in the Xbox presenter).
        File files = tmp.newFolder();
        RecompConfig g = game(files, GBA, "RECOMP_SHADER", "lcd", "RECOMP_SHADER_GHOSTING", "30");
        GameSettings.Env env = GameSettings.runEnv(Console.GBA, cfg("RECOMP_SHADER", "crt", "RECOMP_SHADER_STRENGTH",
                                                                    "75"), g, Locale.US, false);
        assertEquals("lcd", env.value("RECOMP_SHADER"));
        assertEquals("30", env.value("RECOMP_SHADER_GHOSTING"));
        assertNull("Settings' strength: recomp.cfg decides", env.value("RECOMP_SHADER_STRENGTH"));
        RecompConfig none = game(files, PS1, "RECOMP_SHADER", "");
        env = GameSettings.runEnv(Console.PS1, cfg("RECOMP_SHADER", "crt"), none, Locale.US, false);
        assertNull(env.value("RECOMP_SHADER"));
        assertEquals("RECOMP_SHADER", env.value(GameSettings.SKIP_ENV));
        RecompConfig x = game(files, XBOX, "RECOMP_SHADER", "crt", "RECOMP_SHADER_CURVATURE", "30",
                              "RECOMP_SHADER_GHOSTING", "60");
        env = GameSettings.runEnv(Console.XBOX, cfg("RECOMP_SHADER", "scanlines"), x, Locale.US, false);
        assertEquals("crt", env.value("RECOMP_SHADER"));
        assertEquals("30", env.value("RECOMP_SHADER_CURVATURE"));
        assertNull("no LCD grid on the Xbox", env.value("RECOMP_SHADER_GHOSTING"));
        RecompConfig xs = game(files, XBOX, "RECOMP_SHADER", "");
        env = GameSettings.runEnv(Console.XBOX, cfg("RECOMP_SHADER", "crt"), xs, Locale.US, false);
        assertNull(env.value("RECOMP_SHADER"));
        assertEquals("the game's None skips Settings' line", "RECOMP_SHADER", env.value(GameSettings.SKIP_ENV));
        java.util.List<RecompConfig.Option> xo = GameSettings.options(Console.XBOX, false);
        java.util.List<Integer> titles = new java.util.ArrayList<>();
        for (RecompConfig.Option o : xo)
            titles.add(o.title);
        assertTrue(titles.containsAll(List.of(R.string.OPT_CSHADER, R.string.OPT_CSHADER_STRENGTH,
                                              R.string.OPT_CSHADER_CURVE)));
        assertFalse(titles.contains(R.string.OPT_CSHADER_GHOST));
        assertTrue(GameSettings.options(Console.N64, false).stream().anyMatch(o -> o.title == R.string.OPT_CSHADER_GHOST));
        assertTrue(GameSettings.allExportedKeys().containsAll(List.of("RECOMP_SHADER", "RECOMP_SHADER_STRENGTH",
                                                                      "RECOMP_SHADER_CURVATURE",
                                                                      "RECOMP_SHADER_GHOSTING")));
    }

    @Test
    public void language() throws Exception {
        File files = tmp.newFolder();
        RecompConfig none = GameSettings.load(files, XBOX);
        assertEquals("the Android language", "fr",
                     GameSettings.runEnv(Console.XBOX, cfg(), none, Locale.FRANCE, false).value(GameLanguage.ENV));
        assertNull("recomp.cfg's line: unset",
                   GameSettings.runEnv(Console.XBOX, cfg(GameLanguage.ENV, "de"), none, Locale.FRANCE, false)
                               .value(GameLanguage.ENV));
        RecompConfig own = game(files, XBOX, GameLanguage.ENV, "ja");
        assertEquals("ja", GameSettings.runEnv(Console.XBOX, cfg(GameLanguage.ENV, "de"), own, Locale.FRANCE, false)
                                       .value(GameLanguage.ENV));
        RecompConfig system = game(files, XBOX, GameLanguage.ENV, "");
        GameSettings.Env env = GameSettings.runEnv(Console.XBOX, cfg(GameLanguage.ENV, "de"), system, Locale.FRANCE,
                                                   false);
        assertEquals("the game's « System »: the Android language over recomp.cfg's", "fr",
                     env.value(GameLanguage.ENV));
        assertEquals(GameLanguage.ENV, env.value(GameSettings.SKIP_ENV));
        assertNull("a console game has no language",
                   GameSettings.runEnv(Console.GBA, cfg(), none, Locale.FRANCE, false).value(GameLanguage.ENV));
    }

    @Test
    public void controlsAndPakPerConsole() throws Exception {
        File files = tmp.newFolder();
        RecompConfig x = game(files, XBOX, "RECOMP_PAD_MAP", "a=x");
        assertEquals("a=x", GameSettings.runEnv(Console.XBOX, cfg(), x, Locale.US, false).value("RECOMP_PAD_MAP"));
        RecompConfig n = game(files, N64, "RECOMP_N64_PAD_MAP", "", "RECOMP_N64_PAK", "rumble");
        GameSettings.Env env = GameSettings.runEnv(Console.N64, cfg("RECOMP_N64_PAD_MAP", "a=b"), n, Locale.US, false);
        assertNull("its usual mapping", env.value("RECOMP_N64_PAD_MAP"));
        assertEquals("over recomp.cfg's", "RECOMP_N64_PAD_MAP", env.value(GameSettings.SKIP_ENV));
        assertEquals("rumble", env.value("RECOMP_N64_PAK"));
        assertNull(env.value("RECOMP_PAD_MAP"));
        assertNull(env.value("RECOMP_GBA_PAD_MAP"));
    }

    // ---- Rows ---------------------------------------------------------------------------------------------------

    @Test
    public void rowsPerConsole() {
        List<String> xbox = titles(GameSettings.options(Console.XBOX, false));
        assertTrue(xbox.contains("OPT_LANGUAGE") && xbox.contains("OPT_WIDESCREEN") && xbox.contains("OPT_GPU_FPS_CAP")
                   && xbox.contains("OPT_VSYNC") && xbox.contains("OPT_RUMBLE") && xbox.contains("OPT_BIG_CORES")
                   && xbox.contains("OPT_HRTF") && xbox.contains("OPT_AUDIO_BUF") && xbox.contains("OPT_GPU"));
        assertFalse(xbox.contains("OPT_CSCALE") || xbox.contains("OPT_LSFG"));
        assertTrue("XBA-237: the Xbox GPU renderer's internal resolution", xbox.contains("OPT_UPSCALE"));
        assertFalse("a developer switch", xbox.contains("OPT_SELFTEST"));
        assertTrue(titles(GameSettings.options(Console.XBOX, true)).contains("OPT_LSFG"));

        List<String> gba = titles(GameSettings.options(Console.GBA, true));
        assertTrue(gba.contains("OPT_CSCALE") && gba.contains("OPT_CFILTER") && gba.contains("OPT_VSYNC")
                   && gba.contains("OPT_RUMBLE") && gba.contains("OPT_AUDIO_BUF") && gba.contains("OPT_LSFG"));
        assertFalse(gba.contains("OPT_UPSCALE") || gba.contains("OPT_LANGUAGE") || gba.contains("OPT_WIDESCREEN")
                    || gba.contains("OPT_GPU_FPS_CAP") || gba.contains("OPT_HRTF"));
        assertTrue(titles(GameSettings.options(Console.PS1, false)).contains("OPT_UPSCALE"));
        assertTrue(titles(GameSettings.options(Console.N64, false)).contains("OPT_UPSCALE"));
        assertTrue("XBA-274", titles(GameSettings.options(Console.GC, false)).contains("OPT_UPSCALE"));
        // XBA-194 / XBA-274: rewind and its memory for every game, an Xbox title's too (its state service rewinds).
        for (Console c : Console.values()) {
            List<String> t = titles(GameSettings.options(c, false));
            assertTrue(c + " rewind", t.contains("OPT_REWIND") && t.contains("OPT_REWIND_MB"));
        }
        // XBA-195: the picture shader's rows for every game since XBA-203 (the Xbox presenter's), but the LCD
        // ghosting only for the console window's (no LCD grid on the Xbox).
        for (String t : new String[] { "OPT_CSHADER", "OPT_CSHADER_STRENGTH", "OPT_CSHADER_CURVE", "OPT_CSHADER_GHOST" }) {
            assertEquals(t, !t.equals("OPT_CSHADER_GHOST"), xbox.contains(t));
            assertTrue(t, gba.contains(t));
            assertTrue(t, titles(GameSettings.options(Console.PS1, false)).contains(t));
            assertTrue(t, titles(GameSettings.options(Console.N64, false)).contains(t));
        }
        assertEquals(List.of("RECOMP_PAD_MAP", "RECOMP_HOTKEYS"), GameSettings.extraKeys(Console.XBOX));
        assertEquals(List.of("RECOMP_N64_PAD_MAP", "RECOMP_N64_PAK", "RECOMP_HOTKEYS"), GameSettings.extraKeys(Console.N64));
    }

    @Test
    public void positionStepAndMarker() throws Exception {
        File files = tmp.newFolder();
        RecompConfig.Option cap = option(R.string.OPT_GPU_FPS_CAP);   // Auto (no line), 30, 60, Off
        RecompConfig global = cfg("RECOMP_GPU_FPS_CAP", "30");
        RecompConfig g = GameSettings.load(files, XBOX);
        assertEquals("as in Settings", 0, GameSettings.position(global, g, cap));
        assertFalse(GameSettings.differs(global, g, cap));
        assertEquals(1, GameSettings.globalIndex(global, cap));
        int pos = GameSettings.step(0, cap, 1);
        assertEquals(1, pos);
        GameSettings.setPosition(g, cap, pos);   // Auto for this game: « » over Settings' 30
        assertEquals("", g.get("RECOMP_GPU_FPS_CAP"));
        assertEquals(1, GameSettings.position(global, g, cap));
        assertTrue(GameSettings.differs(global, g, cap));
        GameSettings.setPosition(g, cap, 2);   // 30, as Settings: its own, no marker
        assertEquals("30", g.get("RECOMP_GPU_FPS_CAP"));
        assertTrue(GameSettings.owns(g, cap));
        assertFalse(GameSettings.differs(global, g, cap));
        assertEquals("around: back to as in Settings", 0, GameSettings.step(4, cap, 1));
        assertEquals(4, GameSettings.step(0, cap, -1));
        GameSettings.setPosition(g, cap, 0);
        assertFalse(g.has("RECOMP_GPU_FPS_CAP"));
        g.set("RECOMP_GPU_FPS_CAP", "45");   // by hand: no choice has it
        assertEquals(-1, GameSettings.position(global, g, cap));
        assertEquals("a step from a value of its own starts over", 1, GameSettings.step(-1, cap, 1));

        // Frame generation: three keys; the ones the game does not set keep Settings'.
        RecompConfig.Option lsfg = option(R.string.OPT_LSFG);
        RecompConfig gl = cfg("RECOMP_LSFG", "1", "RECOMP_LSFG_MULT", "2", "RECOMP_LSFG_PERF", "1");
        RecompConfig h = GameSettings.load(files, PS1);
        h.set("RECOMP_LSFG_PERF", "");
        assertEquals("2x, the game's quality set", 2, GameSettings.position(gl, h, lsfg));
        GameSettings.setPosition(h, lsfg, 1);
        assertEquals("", h.get("RECOMP_LSFG"));
        assertEquals("", h.get("RECOMP_LSFG_MULT"));
    }

    @Test
    public void resetDropsTheFile() throws Exception {
        File files = tmp.newFolder();
        game(files, XBOX, "RECOMP_ASPECT", "16:9");
        assertTrue(GameSettings.file(files, XBOX).isFile());
        assertTrue(GameSettings.reset(files, XBOX));
        assertFalse(GameSettings.file(files, XBOX).exists());
        assertFalse(GameSettings.load(files, XBOX).has("RECOMP_ASPECT"));
        assertTrue("nothing to drop", GameSettings.reset(files, XBOX));
    }

    // ---- Migration ----------------------------------------------------------------------------------------------

    private static void old(File files, String name, String value) throws Exception {
        TestSupport.write(new File(new File(files, "settings"), name),
                          (value + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    @Test
    public void migratesTheEarlierFilesWithoutLosingValues() throws Exception {
        File files = tmp.newFolder();
        old(files, "language_" + XBOX, "fr");
        old(files, "widescreen_" + XBOX, "1");
        old(files, "480p_" + XBOX, "0");
        old(files, "theme_" + XBOX, "colors");
        old(files, "scale_" + GBA.toUpperCase(Locale.ROOT), "integer");
        old(files, "filter_" + GBA.toUpperCase(Locale.ROOT), "linear");
        old(files, "pad_map_" + GBA.toUpperCase(Locale.ROOT), "map:a=b");
        old(files, "upscale_" + N64.toUpperCase(Locale.ROOT), "4");
        old(files, "framegen_" + N64.toUpperCase(Locale.ROOT), "0");
        old(files, "n64_pak_" + N64.toUpperCase(Locale.ROOT), "rumble");
        old(files, "upscale_" + PS1.toUpperCase(Locale.ROOT), "1");
        old(files, "scale_" + PS1.toUpperCase(Locale.ROOT), "aspect");
        old(files, "framegen_" + PS1.toUpperCase(Locale.ROOT), "1");
        old(files, "enhancements_" + XBOX, "off");   // not a setting: stays where it is

        RecompConfig x = GameSettings.load(files, XBOX);
        assertEquals("fr", x.get("RECOMP_LANGUAGE"));
        assertEquals("1", x.get("RECOMP_WIDESCREEN"));
        assertEquals("off: the runtime's default", "", x.get("RECOMP_480P"));
        assertEquals("colors", x.get(GameSettings.THEME_KEY));
        RecompConfig g = GameSettings.load(files, GBA);
        assertEquals("integer", g.get("RECOMP_SCALE"));
        assertEquals("linear", g.get("RECOMP_FILTER"));
        assertEquals("a=b", g.get("RECOMP_GBA_PAD_MAP"));
        RecompConfig n = GameSettings.load(files, N64);
        assertEquals("4", n.get("RECOMP_UPSCALE"));
        assertEquals("", n.get("RECOMP_LSFG"));
        assertEquals("rumble", n.get("RECOMP_N64_PAK"));
        RecompConfig p = GameSettings.load(files, PS1);
        assertEquals("native: the runtime's default", "", p.get("RECOMP_UPSCALE"));
        assertEquals("", p.get("RECOMP_SCALE"));
        assertEquals("1", p.get("RECOMP_LSFG"));
        assertEquals("2", p.get("RECOMP_LSFG_MULT"));

        String[] left = new File(files, "settings").list();
        List<String> names = left == null ? List.of() : List.of(left);
        assertTrue("the earlier files are gone: " + names, names.contains("enhancements_" + XBOX));
        assertFalse(names.contains("language_" + XBOX) || names.contains("theme_" + XBOX)
                    || names.contains("pad_map_" + GBA.toUpperCase(Locale.ROOT)));
        assertEquals("nothing more to move", 0, GameSettings.migrate(files));
    }

    @Test
    public void theGameFileWinsOverAnEarlierFile() throws Exception {
        File files = tmp.newFolder();
        game(files, XBOX, "RECOMP_LANGUAGE", "de");
        old(files, "language_" + XBOX, "fr");
        assertEquals("de", GameSettings.load(files, XBOX).get("RECOMP_LANGUAGE"));
        assertFalse(new File(new File(files, "settings"), "language_" + XBOX).exists());
    }

    // ---- The per-game classes share the file -----------------------------------------------------------------------

    @Test
    public void perGameClassesWriteTheGameFile() {
        Context c = RuntimeEnvironment.getApplication();
        assertTrue(GameLanguage.setTitleChoice(c, XBOX, "fr"));
        assertTrue(GameVideo.setTitleChoice(c, XBOX, GameVideo.Setting.WIDESCREEN, Boolean.TRUE));
        assertTrue(GameVideo.setTitleChoice(c, XBOX, GameVideo.Setting.P480, Boolean.FALSE));
        assertTrue(GameTheme.setMode(c, XBOX, GameTheme.Mode.OFF));
        assertTrue(N64Pak.setChoice(c, N64, N64Pak.RUMBLE));
        assertTrue(ConsoleControls.storeGame(c, GBA, ConsolePadMap.parse(Console.GBA, null)));
        RecompConfig x = GameSettings.load(c, XBOX);
        assertEquals("fr", x.get("RECOMP_LANGUAGE"));
        assertEquals("1", x.get("RECOMP_WIDESCREEN"));
        assertEquals("", x.get("RECOMP_480P"));
        assertEquals(Boolean.FALSE, GameVideo.titleChoice(c, XBOX, GameVideo.Setting.P480));
        assertEquals("off", x.get(GameSettings.THEME_KEY));
        assertEquals(GameTheme.Mode.OFF, GameTheme.mode(c, XBOX));
        assertTrue(GameTheme.setMode(c, XBOX, GameTheme.Mode.FULL));
        assertFalse("the default theme writes nothing", GameSettings.load(c, XBOX).has(GameSettings.THEME_KEY));
        assertEquals("rumble", GameSettings.load(c, N64).get("RECOMP_N64_PAK"));
        assertEquals("its usual mapping, its own", "", GameSettings.load(c, GBA).get("RECOMP_GBA_PAD_MAP"));
        assertEquals("", ConsoleControls.gameText(c, GBA));
        assertTrue(ConsoleControls.storeGame(c, GBA, null));
        assertNull(ConsoleControls.gameText(c, GBA));
    }
}
