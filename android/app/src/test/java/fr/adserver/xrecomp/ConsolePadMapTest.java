package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.app.Application;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.util.Arrays;

/**
 * XBA-158 / XBA-168 / XBA-174: the Java side of the console mapping (ConsolePadMap, ConsoleControls) says what
 * src/input/console_pad.c reads: the same targets, defaults and text as tests/console_pad (the same cases), stepping
 * one source at a time, and where the console's and a game's own mapping are kept and handed to the runtime.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ConsolePadMapTest {
    private static int bit(PadMapping.Source s) {
        return ConsolePadMap.bit(s);
    }

    @Test
    public void sourcesInConsolePadOrder() {
        // console_pad.h's CPS_* order is PadMapping.Source's, NONE last (not a source).
        String[] c = { "south", "east", "west", "north", "left_shoulder", "right_shoulder", "left_trigger",
                       "right_trigger", "left_stick_click", "right_stick_click", "back", "start", "dpad_up",
                       "dpad_down", "dpad_left", "dpad_right", "left_stick_up", "left_stick_down", "left_stick_left",
                       "left_stick_right", "right_stick_up", "right_stick_down", "right_stick_left",
                       "right_stick_right" };
        for (int i = 0; i < c.length; i++)
            assertEquals(c[i], PadMapping.Source.values()[i].key);
        assertEquals(PadMapping.Source.NONE, PadMapping.Source.values()[c.length]);
        assertEquals(0, bit(PadMapping.Source.NONE));
    }

    @Test
    public void targetsAndDefaults() {
        assertEquals(10, ConsolePadMap.targets(Console.GBA).length);
        assertEquals(16, ConsolePadMap.targets(Console.PS1).length);
        assertEquals(14, ConsolePadMap.targets(Console.N64).length);
        assertEquals("triangle", ConsolePadMap.targets(Console.PS1)[3]);
        assertEquals("c_left", ConsolePadMap.targets(Console.N64)[12]);
        assertArrayEquals(new String[0], ConsolePadMap.sticks(Console.GBA));
        assertArrayEquals(new String[] { "left_stick", "right_stick" }, ConsolePadMap.sticks(Console.PS1));
        assertArrayEquals(new String[] { "stick" }, ConsolePadMap.sticks(Console.N64));

        ConsolePadMap g = new ConsolePadMap(Console.GBA);
        assertEquals(bit(PadMapping.Source.SOUTH) | bit(PadMapping.Source.NORTH), g.source[0]);
        assertEquals(bit(PadMapping.Source.DPAD_LEFT) | bit(PadMapping.Source.LSTICK_LEFT), g.source[8]);
        assertTrue(g.isDefault());
        assertEquals("", g.format());
        assertEquals(0, g.changedCount());
        ConsolePadMap n = new ConsolePadMap(Console.N64);
        assertEquals(bit(PadMapping.Source.LEFT_TRIGGER) | bit(PadMapping.Source.RIGHT_TRIGGER), n.source[2]);
        assertEquals(bit(PadMapping.Source.NORTH) | bit(PadMapping.Source.RSTICK_UP), n.source[10]);
        assertEquals(PadMapping.Stick.LEFT, n.stick[0]);
        assertEquals(-1, n.range);
        ConsolePadMap p = new ConsolePadMap(Console.PS1);
        assertEquals(bit(PadMapping.Source.LEFT_TRIGGER), p.source[6]);
        assertEquals(PadMapping.Stick.RIGHT, p.stick[1]);
        // XBA-259: the GameCube controller, console_pad.c's GC_T_* order and defaults
        assertArrayEquals(new String[] { "a", "b", "x", "y", "z", "l", "r", "start", "dpad_up", "dpad_down",
                                         "dpad_left", "dpad_right" }, ConsolePadMap.targets(Console.GC));
        assertArrayEquals(new String[] { "stick", "c_stick" }, ConsolePadMap.sticks(Console.GC));
        ConsolePadMap c = new ConsolePadMap(Console.GC);
        assertEquals(bit(PadMapping.Source.SOUTH), c.source[0]);
        assertEquals(bit(PadMapping.Source.RIGHT_SHOULDER), c.source[4]);
        assertEquals(bit(PadMapping.Source.LEFT_TRIGGER), c.source[5]);
        assertEquals(PadMapping.Stick.RIGHT, c.stick[1]);
        assertEquals("", c.format());
        assertEquals("RECOMP_GC_PAD_MAP", ConsoleControls.env(Console.GC));
        ConsolePadMap r = ConsolePadMap.parse(Console.GC, "a=east,b=south,c_stick=none");
        assertEquals("a=east,b=south,c_stick=none", r.format());
    }

    /** The same texts as tests/console_pad/test_main.c. */
    @Test
    public void parseAndFormatLikeTheRuntime() {
        ConsolePadMap m = ConsolePadMap.parse(Console.GBA, "a=east, B=South;l=left_trigger  select=none");
        assertEquals("a=east,b=south,l=left_trigger,select=none", m.format());
        assertEquals(4, m.changedCount());
        m = ConsolePadMap.parse(Console.GBA, "start=start+north,a=south");
        assertEquals("a=south,start=north+start", m.format());
        m = ConsolePadMap.parse(Console.GBA, "x=south,a=nope,cross=south,a,b=east+,r=north,stick=left,range=80");
        assertEquals("r=north", m.format());
        m = ConsolePadMap.parse(Console.PS1, "left_stick=right,right_stick=none,cross=east,circle=south");
        assertEquals("cross=east,circle=south,left_stick=right,right_stick=none", m.format());
        m = ConsolePadMap.parse(Console.N64, "stick=right,range=96,deadzone=12,z=left_shoulder,l=left_trigger");
        assertEquals("z=left_shoulder,l=left_trigger,stick=right,range=96,deadzone=12", m.format());
        ConsolePadMap same = ConsolePadMap.parse(Console.N64, m.format() + ",range=0,range=128,deadzone=91,range=");
        assertEquals(m.format(), same.format());
        assertTrue(ConsolePadMap.parse(Console.GBA, null).isDefault());
        assertTrue(ConsolePadMap.parse(Console.GBA, "").isDefault());
    }

    @Test
    public void stepOneSourceAtATime() {
        ConsolePadMap g = new ConsolePadMap(Console.GBA);
        assertArrayEquals(new PadMapping.Source[] { PadMapping.Source.SOUTH, PadMapping.Source.NORTH }, g.sourcesOf(0));
        assertEquals(PadMapping.Source.EAST, g.step(0, 1));   // from its first source
        assertEquals("a=east", g.format());
        assertEquals(Arrays.asList(1), g.sharing(0));         // B has east too
        assertEquals(PadMapping.Source.SOUTH, g.step(0, -1));
        assertEquals(PadMapping.Source.NONE, g.step(0, -1));  // before south: not used
        assertEquals("a=none", g.format());
        assertEquals(0, g.sourcesOf(0).length);
        assertEquals(PadMapping.Source.RSTICK_RIGHT, g.step(0, -1));
        assertEquals(PadMapping.Source.NONE, g.step(0, 1));
        assertEquals(PadMapping.Source.SOUTH, g.step(0, 1));
        g.reset();
        assertTrue(g.isDefault());

        ConsolePadMap n = new ConsolePadMap(Console.N64);
        n.stepStick(0, 1);
        assertEquals(PadMapping.Stick.RIGHT, n.stick[0]);
        n.range = ConsolePadMap.stepIn(ConsolePadMap.RANGES, n.range, 1);
        n.deadzone = ConsolePadMap.stepIn(ConsolePadMap.DEADZONES, n.deadzone, -1);
        assertEquals("stick=right,range=64,deadzone=30", n.format());
        assertEquals(3, n.changedCount());
        assertEquals(n.format(), n.copy().format());
        n.reset();
        assertEquals("", n.format());
    }

    @Test
    public void storageAndRunEnv() throws Exception {
        Application app = RuntimeEnvironment.getApplication();
        String key = "gba-AXVE-12345678";
        assertEquals("RECOMP_GBA_PAD_MAP", ConsoleControls.env(Console.GBA));
        assertEquals("RECOMP_PS1_PAD_MAP", ConsoleControls.env(Console.PS1));
        assertEquals("RECOMP_N64_PAD_MAP", ConsoleControls.env(Console.N64));
        assertNull(ConsoleControls.env(Console.XBOX));
        assertNull(ConsoleControls.gameText(app, key));
        assertFalse(ConsoleControls.hasGameMap(app, key));

        // The console's, in recomp.cfg.
        java.io.File dir = new java.io.File(app.getFilesDir(), "ext");
        RecompConfig cfg = RecompConfig.load(dir);
        ConsoleControls.storeConsole(cfg, ConsolePadMap.parse(Console.GBA, "a=east"));
        cfg.save();
        assertEquals("a=east", RecompConfig.load(dir).get("RECOMP_GBA_PAD_MAP"));
        assertEquals("a=east", ConsoleControls.effective(app, dir, key).format());
        ConsoleControls.storeConsole(cfg, new ConsolePadMap(Console.GBA));
        assertNull("the usual mapping: no line", cfg.get("RECOMP_GBA_PAD_MAP"));
        ConsoleControls.storeConsole(cfg, ConsolePadMap.parse(Console.GBA, "a=east"));
        cfg.save();

        // The game's own wins, also when it is the usual mapping ("").
        assertTrue(ConsoleControls.storeGame(app, key, ConsolePadMap.parse(Console.GBA, "b=north")));
        assertEquals("b=north", ConsoleControls.gameText(app, key));
        assertEquals("b=north", ConsoleControls.effective(app, dir, key).format());
        String[][] env = ConsoleControls.runEnv(app, key);
        assertArrayEquals(new String[] { "RECOMP_GBA_PAD_MAP", "b=north" }, env[0]);
        assertArrayEquals(new String[] { "RECOMP_PS1_PAD_MAP", null }, env[1]);
        assertArrayEquals(new String[] { "RECOMP_N64_PAD_MAP", null }, env[2]);
        assertArrayEquals(new String[] { "RECOMP_GC_PAD_MAP", null }, env[3]);   // XBA-274
        ConsoleControls.storeGame(app, key, new ConsolePadMap(Console.GBA));
        assertEquals("", ConsoleControls.gameText(app, key));
        assertEquals("", ConsoleControls.runEnv(app, key)[0][1]);
        assertEquals("", ConsoleControls.effective(app, dir, key).format());
        assertNull("another game follows the console", ConsoleControls.gameText(app, "gba-BPRE-00000001"));

        // Back to the console's: unset, recomp.cfg decides.
        ConsoleControls.storeGame(app, key, null);
        assertNull(ConsoleControls.gameText(app, key));
        assertNull(ConsoleControls.runEnv(app, key)[0][1]);
        assertEquals("a=east", ConsoleControls.effective(app, dir, key).format());
        // An Xbox game (or none): every console's variable unset.
        for (String[] kv : ConsoleControls.runEnv(app, null))
            assertNull(kv[1]);
        for (String[] kv : ConsoleControls.runEnv(app, "4D530004"))
            assertNull(kv[1]);
    }
}
