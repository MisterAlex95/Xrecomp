package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * XBA-123: the Controls screen's ports (RECOMP_PAD_PORTS) and its prediction of the game's start, the same cases as
 * tests/input_pad_ports (src/input/input_ports.c xbox_InputPortsStart).
 */
public class PadPortsTest {
    private static final PadPorts.Device XBOX = new PadPorts.Device("045e:0b13", "Xbox Wireless Controller", false);
    private static final PadPorts.Device THOR = new PadPorts.Device("2dc8:3106", "Thor Controller", true);
    private static final PadPorts.Device PS4 = new PadPorts.Device("054c:09cc", "Wireless Controller", false);

    @Test
    public void key() {
        assertEquals("045e:0b13", PadPorts.key(0x45E, 0xB13));
        assertEquals("ffff:0000", PadPorts.key(-1, 0x10000));
    }

    @Test
    public void startWithoutPins() {
        PadPorts.Device[] at = new PadPorts().start(Arrays.asList(XBOX, PS4));
        assertSame(XBOX, at[0]);
        assertSame(PS4, at[1]);
        assertNull(at[2]);
    }

    @Test
    public void theBuiltInPadIsPlayerOne() {
        PadPorts.Device[] at = new PadPorts().start(Arrays.asList(XBOX, THOR));
        assertSame(THOR, at[0]);
        assertSame(XBOX, at[1]);
        assertEquals("2dc8:3106", PadPorts.builtinKey(Arrays.asList(XBOX, THOR)));
    }

    @Test
    public void aPinWinsAndAnAbsentPinLeavesNoHole() {
        PadPorts p = PadPorts.parse("1=045e:0b13:Xbox Wireless Controller");
        PadPorts.Device[] at = p.start(Arrays.asList(XBOX, THOR));
        assertSame(XBOX, at[0]);
        assertSame(THOR, at[1]);
        p = PadPorts.parse("1=9999:9999,2=2dc8:3106");
        at = p.start(Arrays.asList(XBOX, THOR));
        assertSame(THOR, at[0]);
        assertSame(XBOX, at[1]);
        at = p.start(Collections.<PadPorts.Device>emptyList());
        assertNull(at[0]);
    }

    @Test
    public void parseAndFormat() {
        PadPorts p = PadPorts.parse("5=045e:0b13, 1=zz, 2=045E:0B13 ,x,3=2dc8:3106:Thor, Controller");
        assertNull(p.pin(0));
        assertEquals("045e:0b13", p.pin(1));
        assertEquals("2dc8:3106", p.pin(2));
        assertEquals("Thor", p.pinName(2));   // a comma ends the entry
        assertEquals("2=045e:0b13,3=2dc8:3106:Thor", p.format());
        assertTrue(PadPorts.parse(null).isDefault());
        assertTrue(PadPorts.parse("").format().isEmpty());
    }

    @Test
    public void setMovesAPinAndStepCycles() {
        PadPorts p = new PadPorts();
        p.set(0, XBOX);
        p.set(1, XBOX);   // the same controller: it leaves port 1
        assertNull(p.pin(0));
        assertEquals("045e:0b13", p.pin(1));
        p.set(1, new PadPorts.Device("045e:0b13", "A, b", false));
        assertEquals("2=045e:0b13:A  b", p.format());

        p.reset();
        List<PadPorts.Device> present = Arrays.asList(THOR, XBOX);
        assertSame(THOR, p.step(0, 1, present));
        assertSame(XBOX, p.step(0, 1, present));
        assertNull(p.step(0, 1, present));            // back to First come
        assertSame(XBOX, p.step(0, -1, present));
        // Pinned to a controller that is not connected: still offered, so stepping does not lose it.
        PadPorts q = PadPorts.parse("1=054c:09cc:Wireless Controller");
        List<PadPorts.Device> c = q.choices(0, present);
        assertEquals(4, c.size());
        assertEquals("Wireless Controller", c.get(3).name);
        // Two of one model: offered once.
        assertEquals(2, q.choices(1, Arrays.asList(XBOX, XBOX)).size());
    }
}
