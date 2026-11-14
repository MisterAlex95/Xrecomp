package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Arrays;
import java.util.Collections;

/** XBA-131: the controller mapping's model and its RECOMP_PAD_MAP text (the grammar of src/input/input_map.c). */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PadMappingTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void defaultsAreTheFixedMapping() {
        PadMapping m = new PadMapping();
        assertTrue(m.isDefault());
        assertEquals("", m.format());
        assertEquals(0, m.changedCount());
        assertEquals(PadMapping.Source.SOUTH, m.get(PadMapping.Control.A));
        assertEquals(PadMapping.Source.LEFT_SHOULDER, m.get(PadMapping.Control.BLACK));
        assertEquals(PadMapping.Source.RIGHT_SHOULDER, m.get(PadMapping.Control.WHITE));
        assertEquals(PadMapping.Source.LEFT_TRIGGER, m.get(PadMapping.Control.LEFT_TRIGGER));
        assertEquals(PadMapping.Source.BACK, m.get(PadMapping.Control.BACK));
        // The controls are input_map.h's sixteen, in its order (InputControl).
        String[] keys = { "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "left_thumb",
                          "right_thumb", "a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger" };
        for (int i = 0; i < keys.length; i++)
            assertEquals(keys[i], PadMapping.Control.values()[i].key);
    }

    /** The same text tests/input_map/test_main.c expects from xbox_InputMap_PadMapFormat for the same mapping. */
    @Test
    public void formatIsInputMapsText() {
        PadMapping m = new PadMapping();
        m.set(PadMapping.Control.A, PadMapping.Source.EAST);
        m.set(PadMapping.Control.B, PadMapping.Source.SOUTH);
        m.set(PadMapping.Control.BLACK, PadMapping.Source.LEFT_TRIGGER);
        m.rightStick = PadMapping.Stick.NONE;
        assertEquals("a=east,b=south,black=left_trigger,right_stick=none", m.format());
        assertEquals(4, m.changedCount());
        PadMapping back = PadMapping.parse(m.format());
        assertEquals(m.format(), back.format());
    }

    @Test
    public void parseLikeTheNativeSide() {
        PadMapping m = PadMapping.parse("a=east, B=South;black=left_trigger right_stick=none");
        assertEquals("a=east,b=south,black=left_trigger,right_stick=none", m.format());
        // Unknown entries are skipped, the rest kept.
        m = PadMapping.parse("a=banana,left_stick_up=south,z=east,x,=,y=west,left_stick=sideways,start=guide");
        assertEquals("y=west", m.format());
        assertTrue(PadMapping.parse(null).isDefault());
        assertTrue(PadMapping.parse("").isDefault());
        m = PadMapping.parse("start=none,left_stick=right,right_stick=left,white=right_stick_up");
        assertEquals(PadMapping.Source.NONE, m.get(PadMapping.Control.START));
        assertEquals(PadMapping.Stick.RIGHT, m.leftStick);
        assertEquals(PadMapping.Source.RSTICK_UP, m.get(PadMapping.Control.WHITE));
    }

    @Test
    public void stepWrapsThroughEverySource() {
        PadMapping m = new PadMapping();
        int n = PadMapping.Source.values().length;
        for (int i = 0; i < n; i++)
            m.step(PadMapping.Control.A, 1);
        assertEquals(PadMapping.Source.SOUTH, m.get(PadMapping.Control.A));
        assertEquals(PadMapping.Source.NONE, m.step(PadMapping.Control.A, -1));
        assertEquals(PadMapping.Stick.NONE, PadMapping.stepStick(PadMapping.Stick.LEFT, -1));
        assertEquals(PadMapping.Stick.RIGHT, PadMapping.stepStick(PadMapping.Stick.LEFT, 1));
    }

    @Test
    public void reverseViewAndSharing() {
        PadMapping m = new PadMapping();
        m.set(PadMapping.Control.WHITE, PadMapping.Source.SOUTH);
        assertEquals(Arrays.asList(PadMapping.Control.A, PadMapping.Control.WHITE),
                     m.controlsOf(PadMapping.Source.SOUTH));
        assertEquals(Collections.emptyList(), m.controlsOf(PadMapping.Source.RIGHT_SHOULDER));
        assertEquals(Collections.singletonList(PadMapping.Control.A), m.sharing(PadMapping.Control.WHITE));
        m.set(PadMapping.Control.WHITE, PadMapping.Source.NONE);
        m.set(PadMapping.Control.BLACK, PadMapping.Source.NONE);
        assertTrue("two unbound controls do not share a button", m.sharing(PadMapping.Control.WHITE).isEmpty());
    }

    @Test
    public void storedInRecompCfg() throws Exception {
        File dir = tmp.getRoot();
        TestSupport.write(new File(dir, RecompConfig.FILE_NAME), "# mine\nRECOMP_KEYBOARD=1\n");
        RecompConfig c = RecompConfig.load(dir);
        PadMapping m = PadMapping.load(c);
        assertTrue(m.isDefault());
        m.set(PadMapping.Control.X, PadMapping.Source.NORTH);
        m.set(PadMapping.Control.Y, PadMapping.Source.WEST);
        m.store(c);
        c.save();
        String text = PrepareProtocol.readText(new File(dir, RecompConfig.FILE_NAME), 1 << 16);
        assertTrue(text, text.contains("RECOMP_PAD_MAP=x=north,y=west\n"));
        assertTrue("the player's lines stay", text.startsWith("# mine\nRECOMP_KEYBOARD=1\n"));
        assertEquals("x=north,y=west", PadMapping.load(RecompConfig.load(dir)).format());
        // Back to the default: no line.
        m.reset();
        RecompConfig c2 = RecompConfig.load(dir);
        m.store(c2);
        c2.save();
        assertNull(RecompConfig.load(dir).get(PadMapping.KEY));
        assertFalse(PrepareProtocol.readText(new File(dir, RecompConfig.FILE_NAME), 1 << 16).contains("PAD_MAP"));
    }
}
