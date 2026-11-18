package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** XBA-282: the player's Xbox 360 key file (X360Key) and the shaders the launcher counts for the self-healing loop. */
public class X360KeyTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final byte[] KEY = "xba-270-made-up!".getBytes(StandardCharsets.US_ASCII);

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b)
            s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }

    @Test
    public void parsesTheTwoForms() {
        assertArrayEquals(KEY, X360Key.parse(KEY, 16));
        byte[] text = (hex(KEY) + "\n# a comment\n").getBytes(StandardCharsets.US_ASCII);
        assertArrayEquals(KEY, X360Key.parse(text, text.length));
        byte[] upper = hex(KEY).toUpperCase().getBytes(StandardCharsets.US_ASCII);
        assertArrayEquals(KEY, X360Key.parse(upper, upper.length));
        byte[] shortHex = "0011".getBytes(StandardCharsets.US_ASCII);
        assertNull(X360Key.parse(shortHex, shortHex.length));
        byte[] notHex = "zz112233445566778899aabbccddeeff".getBytes(StandardCharsets.US_ASCII);
        assertNull(X360Key.parse(notHex, notHex.length));
    }

    @Test
    public void installsTheKeyBytesOnly() throws Exception {
        File dir = tmp.getRoot();
        X360Key.install(dir, new ByteArrayInputStream((hex(KEY) + "\n").getBytes(StandardCharsets.US_ASCII)));
        File f = X360Key.file(dir);
        assertTrue(f.isFile());
        assertArrayEquals(KEY, Files.readAllBytes(f.toPath()));   // 16 raw bytes: what load_keys reads
        try {
            X360Key.install(dir, new ByteArrayInputStream(new byte[X360Key.MAX + 10]));
            fail("too large");
        } catch (IOException expected) {
        }
        try {
            X360Key.install(dir, new ByteArrayInputStream("not a key".getBytes(StandardCharsets.US_ASCII)));
            fail("not a key");
        } catch (IOException expected) {
        }
        assertArrayEquals(KEY, Files.readAllBytes(f.toPath()));   // a bad pick keeps the key there was
    }

    @Test
    public void newShadersAreThoseThePackLacks() throws Exception {
        File files = tmp.getRoot();
        String key = "x360-X360-58425A30-00000001";
        File met = new File(new File(new File(files, "work"), key), "shaders");
        File have = new File(Pack.packDir(files, key), "shaders");
        assertEquals(0, Pack.newShaders(files, key));
        assertTrue(met.mkdirs() && have.mkdirs());
        for (String n : new String[] { "vs_00000001.bin", "ps_00000002.bin", "vs_00000003.bin", "notes.txt" })
            TestSupport.write(new File(met, n), "x");
        TestSupport.write(new File(have, "vs_00000001.bin"), "x");
        TestSupport.write(new File(have, "vs_00000001.vert"), "x");
        assertEquals(2, Pack.newShaders(files, key));
        assertFalse(new File(have, "ps_00000002.bin").exists());
    }
}
