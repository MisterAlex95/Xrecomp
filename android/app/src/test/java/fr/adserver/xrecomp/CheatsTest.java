package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/**
 * XBA-197: a game's cheat codes (Cheats): the file beside its settings, its format (the native engine's,
 * src/platform/recomp_cheats.h: the runtime reads what the app writes), codes cleaned up and checked per console,
 * add / toggle / delete, writes through a temporary file.
 */
public class CheatsTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String GBA = "gba-AXVE-1A2B3C4D", N64 = "n64-NZLP-B2055FBD";

    @Test
    public void fileBesideTheGameSettings() throws Exception {
        File files = tmp.getRoot();
        assertEquals(new File(files, "settings/game/GBA-AXVE-1A2B3C4D.cheats"), Cheats.file(files, GBA));
        assertEquals(GameSettings.dir(files), Cheats.file(files, GBA).getParentFile());
        Cheats none = Cheats.load(files, GBA);
        assertTrue(none.list.isEmpty());
    }

    @Test
    public void parsesTheNativeFormat() {
        String text = "# comment\n"
                      + "[Infinite lives]\nenabled=1\n8003A2C4 0063\n8003A2C6 0001\n\n"
                      + "[Off]\nenabled=0\nformat=gs\n8103A2C8 0001\n"
                      + "[Name [with] brackets]\nenabled=on\nformat=auto\n0203FFB0:C0DE0001\n"
                      + "lines before a section are ignored\n";
        List<Cheats.Cheat> l = Cheats.parse(text);
        assertEquals(3, l.size());
        assertEquals("Infinite lives", l.get(0).name);
        assertTrue(l.get(0).enabled);
        assertEquals(Arrays.asList("8003A2C4 0063", "8003A2C6 0001"), l.get(0).lines);
        assertNull(l.get(0).format);
        assertFalse(l.get(1).enabled);
        assertEquals("gs", l.get(1).format);
        assertEquals("Name [with] brackets", l.get(2).name);
        assertTrue(l.get(2).enabled);
        assertNull("auto is no format line", l.get(2).format);
        // What format() writes parses back the same.
        List<Cheats.Cheat> again = Cheats.parse(Cheats.format(l));
        assertEquals(3, again.size());
        for (int i = 0; i < 3; i++) {
            assertEquals(l.get(i).name, again.get(i).name);
            assertEquals(l.get(i).enabled, again.get(i).enabled);
            assertEquals(l.get(i).format, again.get(i).format);
            assertEquals(l.get(i).lines, again.get(i).lines);
        }
        assertTrue(Cheats.format(l).startsWith(Cheats.HEADER + "\n"));
        assertTrue(Cheats.format(l).contains("\n[Off]\nenabled=0\nformat=gs\n8103A2C8 0001\n"));
    }

    @Test
    public void namesStayOnOneLine() {
        assertEquals("Two lines", Cheats.cleanName("Two\nlines"));
        assertEquals("Bracket", Cheats.cleanName("[[Bracket"));
        assertEquals(Cheats.MAX_NAME, Cheats.cleanName("x".repeat(200)).length());
        assertEquals("", Cheats.cleanName(null));
    }

    @Test
    public void codesAreCleanedUp() {
        assertEquals(Arrays.asList("8003A2C4 0063", "D0123456 0001", "0203FFB0:C0DE0001", "8003A2C4 0063"),
                     Cheats.normalize("  8003a2c4   0063  # lives\r\n\n d0123456\t0001\n0203ffb0:c0de0001\n8003A2C40063"));
        assertEquals(Arrays.asList("A1B2C3D4 E5F60718"), Cheats.normalize("a1b2c3d4e5f60718"));
        assertTrue(Cheats.normalize("  \n# only a comment\n").isEmpty());
    }

    @Test
    public void codesAreCheckedPerConsole() {
        assertEquals(Cheats.Problem.EMPTY, Cheats.check(Console.GBA, Cheats.normalize("")));
        assertNull(Cheats.check(Console.GBA, Cheats.normalize("A1B2C3D4 E5F60718\n32000010 0042\n02000010:12")));
        assertNull(Cheats.check(Console.PS1, Cheats.normalize("80012344 03E7\n30012347 0063")));
        assertNull(Cheats.check(Console.N64, Cheats.normalize("8033B21D 0064")));
        assertEquals("8+8 is a GBA shape", Cheats.Problem.SHAPE,
                     Cheats.check(Console.N64, Cheats.normalize("A1B2C3D4 E5F60718")));
        assertEquals(Cheats.Problem.NOT_HEX, Cheats.check(Console.PS1, Cheats.normalize("8003A2C4 00G3")));
        assertEquals(Cheats.Problem.SHAPE, Cheats.check(Console.PS1, Cheats.normalize("8003A2C4 063")));
        assertEquals(Cheats.Problem.SHAPE, Cheats.check(Console.GBA, Cheats.normalize("123456789:1")));
        StringBuilder big = new StringBuilder();
        for (int i = 0; i <= Cheats.MAX_LINES; i++)
            big.append("8003A2C4 0063\n");
        assertEquals(Cheats.Problem.TOO_LONG, Cheats.check(Console.N64, Cheats.normalize(big.toString())));
    }

    @Test
    public void addToggleDeleteSave() throws Exception {
        File files = tmp.getRoot();
        Cheats c = Cheats.load(files, N64);
        Cheats.Cheat a = c.add("  Moon jump ", Cheats.normalize("D01AB2C3 0020\n8133B1BC 4400"), "Cheat 1");
        assertEquals("Moon jump", a.name);
        assertTrue("a new cheat is on", a.enabled);
        c.add("", Cheats.normalize("8033B21D 0064"), "Cheat 2");
        assertEquals("Cheat 2", c.list.get(1).name);
        c.toggle(1);
        assertEquals(1, c.enabledCount());
        assertTrue(c.save());
        assertFalse("no temporary file left", new File(c.file.getPath() + ".tmp").exists());
        String text = new String(Files.readAllBytes(c.file.toPath()), StandardCharsets.UTF_8);
        assertTrue(text.contains("[Moon jump]\nenabled=1\nD01AB2C3 0020\n8133B1BC 4400\n"));
        assertTrue(text.contains("[Cheat 2]\nenabled=0\n8033B21D 0064\n"));

        Cheats back = Cheats.load(files, N64);
        assertEquals(2, back.list.size());
        back.remove(0);
        back.remove(5);   // out of range: nothing
        assertTrue(back.save());
        Cheats last = Cheats.load(files, N64);
        assertEquals(1, last.list.size());
        assertEquals("Cheat 2", last.list.get(0).name);
        assertEquals(1, last.view().size());
    }
}
