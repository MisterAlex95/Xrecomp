package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.ComponentCallbacks2;
import android.content.Context;
import android.view.KeyEvent;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * XBA-340: the game process's breadcrumb helpers build only pairs the allow-list takes (one bad pair would drop the
 * whole event), never a name; settings changed in the menu leave one crumb each; the controller count, memory
 * pressure and the rewind / fast-forward chords.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GameCrumbsTest {
    private Context c;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        Breadcrumbs.file(c).delete();
    }

    @After
    public void tearDown() {
        Breadcrumbs.file(c).delete();
    }

    private List<String> names() {
        List<String> out = new ArrayList<>();
        for (Breadcrumbs.Crumb cr : Breadcrumbs.read(Breadcrumbs.file(c), Breadcrumbs.KEEP))
            out.add(cr.name + (cr.data.isEmpty() ? "" : " " + String.join(",", cr.data)));
        return out;
    }

    @Test
    public void valuesAreKnownWordsOrOther() {
        assertEquals("default", GameCrumbs.value(null));
        assertEquals("2", GameCrumbs.value(" 2 "));
        assertEquals("gl", GameCrumbs.value("GL"));
        assertEquals("other", GameCrumbs.value("16:9"));               // would fail the token check
        assertEquals("other", GameCrumbs.value("my_save_name"));       // a token, but not a known word
        assertEquals("other", GameCrumbs.value("/sdcard/x.iso"));
        assertEquals("auto", GameCrumbs.slot(GameMenu.AUTO_SLOT));
        assertEquals("3", GameCrumbs.slot(3));
        assertEquals("ok", GameCrumbs.code(GameMenu.STATE_OK));
        assertEquals("-2", GameCrumbs.code(GameMenu.STATE_EMPTY));
        assertEquals("cpu", GameCrumbs.renderer("0"));
        assertEquals("gl", GameCrumbs.renderer("1"));
        assertEquals("auto", GameCrumbs.renderer(null));
        assertEquals("other", GameCrumbs.renderer("vulkan;rm -rf"));
        for (String why : new String[] { "Back", "Guide", "Select+Start", "swipe", "panel", "anything else", null })
            assertNotNull(why, Breadcrumbs.line(1, "menu_open", "via", GameCrumbs.via(why)));
        assertEquals("combo", GameCrumbs.via("Select+Start"));
    }

    @Test
    public void everyHelperWritesItsEvent() {
        GameSettings.Env env = new GameSettings.Env();
        env.vars.add(new String[] { "RECOMP_GPU", "1" });
        env.vars.add(new String[] { ConsoleVideo.ENV_UPSCALE, "4" });
        GameCrumbs.start(c, Console.N64, env, 2, 3);
        GameCrumbs.state(c, true, 1, GameMenu.STATE_OK, "menu");
        GameCrumbs.state(c, false, GameMenu.AUTO_SLOT, GameMenu.STATE_DAMAGED, "resume");
        GameCrumbs.fastForward(c, "on", "menu");
        GameCrumbs.cheat(c, false);
        GameCrumbs.disc(c, 2);
        GameCrumbs.menu(c, true, "Select+Start");
        GameCrumbs.menu(c, false, null);
        GameCrumbs.pause(c, true);
        GameCrumbs.pause(c, false);
        GameCrumbs.Chords.crumb(c, Hotkeys.Action.REWIND);
        GameCrumbs.end(c, "quit");
        assertEquals(List.of("game_start console,n64,renderer,gl,upscale,4,pads,2", "mod state,on,count,3",
                             "savestate_save slot,1,result,ok,via,menu",
                             "savestate_load slot,auto,result,-7,via,resume", "fast_forward state,on,via,menu",
                             "cheat state,off", "disc_change disc,2", "menu_open via,combo", "menu_close", "pause",
                             "resume", "rewind via,hotkey", "game_end how,quit"), names());
        // A new run starts the ring again.
        GameCrumbs.start(c, Console.XBOX, null, 0, 0);
        assertEquals(List.of("game_start console,xbox,renderer,auto,upscale,default,pads,0"), names());
    }

    @Test
    public void settingsLeaveTheirLastValueOnce() {
        GameCrumbs.Pending p = new GameCrumbs.Pending();
        p.option(ConsoleVideo.ENV_UPSCALE, "2");
        p.option("RECOMP_ASPECT", "16:9");
        p.option(ConsoleVideo.ENV_UPSCALE, "4");      // the same setting again: its last value only, last in order
        p.option("RECOMP_GPU", "0");
        p.option("RECOMP_GPU_FPS_CAP", null);
        p.named("renderer", "vk_driver", "other");
        List<String[]> l = p.list();
        assertEquals(5, l.size());
        assertArrayEquals(new String[] { "setting", "name", "aspect", "value", "other" }, l.get(0));
        assertArrayEquals(new String[] { "resolution", "name", "upscale", "value", "4" }, l.get(1));
        assertArrayEquals(new String[] { "renderer", "name", "gpu", "value", "0" }, l.get(2));
        assertArrayEquals(new String[] { "speed", "name", "gpu_fps_cap", "value", "default" }, l.get(3));
        p.flush(c);
        assertTrue(p.isEmpty());
        assertEquals(List.of("setting name,aspect,value,other", "resolution name,upscale,value,4",
                             "renderer name,gpu,value,0", "speed name,gpu_fps_cap,value,default",
                             "renderer name,vk_driver,value,other"), names());
        assertEquals("other", GameCrumbs.Pending.nameOf("RECOMP_A_KEY_THAT_IS_FAR_TOO_LONG_FOR_IT"));
    }

    @Test
    public void padsCountChanges() {
        GameCrumbs.Pads p = new GameCrumbs.Pads(1);
        assertNull(p.changed(1));                     // a device that is not a new pad (or the same pad again)
        assertArrayEquals(new String[] { "pad_connect", "pads", "2" }, p.changed(2));
        assertNull(p.changed(2));
        assertArrayEquals(new String[] { "pad_disconnect", "pads", "0" }, p.changed(0));
        p.changed(c, 1);
        assertEquals(List.of("pad_connect pads,1"), names());
    }

    @Test
    public void lowMemoryNotForTheBackgroundNorRepeated() {
        assertFalse(GameCrumbs.trimWorthIt(ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN, -1));
        assertTrue(GameCrumbs.trimWorthIt(ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW, -1));
        assertFalse(GameCrumbs.trimWorthIt(ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW,
                                           ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW));
        GameCrumbs.trim(c, ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL);
        GameCrumbs.trim(c, ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL);
        GameCrumbs.trim(c, ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN);
        assertEquals(List.of("low_memory level,15"), names());
    }

    @Test
    public void rewindAndFastForwardChords() {
        Hotkeys h = new Hotkeys();   // the defaults: rewind Select + L2 held
        GameCrumbs.Chords w = new GameCrumbs.Chords(h, Console.GBA, true);
        Hotkeys.Chord rw = h.get(Hotkeys.Action.REWIND);
        int mod = Hotkeys.keyOf(rw.mod), button = Hotkeys.keyOf(rw.button);
        assertNull(w.key(mod, true));
        assertEquals(Hotkeys.Action.REWIND, w.key(button, true));
        assertNull("held: once", w.key(button, true));
        assertNull(w.key(button, false));
        assertEquals("pressed again", Hotkeys.Action.REWIND, w.key(button, true));
        w.reset();
        // L2 as an axis.
        if (button == KeyEvent.KEYCODE_BUTTON_L2) {
            assertNull(w.key(mod, true));
            assertEquals(Hotkeys.Action.REWIND, w.triggers(0.9f, 0));
            assertNull(w.triggers(1f, 0));
            assertNull(w.triggers(0, 0));
        }
        // Rewind off: not watched.
        GameCrumbs.Chords off = new GameCrumbs.Chords(h, Console.GBA, false);
        off.key(mod, true);
        assertNull(off.key(button, true));
        // The fast-forward chords, when set.
        Hotkeys.Chord ff = h.get(Hotkeys.Action.FF_TOGGLE);
        if (ff != null && !ff.isNone() && Hotkeys.keyOf(ff.button) >= 0) {
            GameCrumbs.Chords f = new GameCrumbs.Chords(h, Console.GBA, false);
            if (ff.mod != null)
                f.key(Hotkeys.keyOf(ff.mod), true);
            assertEquals(Hotkeys.Action.FF_TOGGLE, f.key(Hotkeys.keyOf(ff.button), true));
        }
    }

    @Test
    public void enabledModsAreCounted() throws Exception {
        File root = new File(c.getCacheDir(), "mods-test");
        File d = new File(root, "xbox/58420001");
        assertTrue(d.mkdirs() || d.isDirectory());
        assertEquals(0, GameCrumbs.enabledMods(root, Console.XBOX, "58420001"));
        Files.write(new File(d, "enabled.json").toPath(), "[\"widescreen\", \"fps\"]".getBytes(StandardCharsets.UTF_8));
        assertEquals(2, GameCrumbs.enabledMods(root, Console.XBOX, "58420001"));
        Files.write(new File(d, "enabled.json").toPath(), "{\"enabled\": [\"a\"]}".getBytes(StandardCharsets.UTF_8));
        assertEquals(1, GameCrumbs.enabledMods(root, Console.XBOX, "58420001"));
        Files.write(new File(d, "enabled.json").toPath(), "not json".getBytes(StandardCharsets.UTF_8));
        assertEquals(0, GameCrumbs.enabledMods(root, Console.XBOX, "58420001"));
        assertEquals(0, GameCrumbs.enabledMods(null, Console.XBOX, "58420001"));
        new File(d, "enabled.json").delete();
    }
}
