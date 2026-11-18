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
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** XBA-282: the Xbox 360 in the app: the key file (X360Key), the shaders the self-healing loop counts, the video
 * options, the cheats shape, the pack's cover. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class X360AppTest {
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
    public void thePlayersKeyComesBeforeTheBuiltInOne() throws Exception {
        File dir = java.nio.file.Files.createTempDirectory("x360keys").toFile();
        File player = new File(dir, "x360.key"), builtin = new File(dir, "builtin.key");
        assertNull(X360Key.keyFiles(player, null));
        assertNull(X360Key.keyFiles(player, builtin));
        java.nio.file.Files.write(builtin.toPath(), KEY);
        org.junit.Assert.assertEquals(builtin.getAbsolutePath(), X360Key.keyFiles(player, builtin));
        java.nio.file.Files.write(player.toPath(), KEY);
        org.junit.Assert.assertEquals(player.getAbsolutePath() + File.pathSeparator + builtin.getAbsolutePath(),
                                      X360Key.keyFiles(player, builtin));
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

    @Test
    public void videoOptionsApply() {
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.UPSCALE, Console.X360));
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.SCALE, Console.X360));
        assertTrue(ConsoleVideo.applies(ConsoleVideo.Setting.FILTER, Console.X360));
        boolean up = false, tex = false;
        for (RecompConfig.Option o : GameSettings.options(Console.X360, false)) {
            up |= o.title == R.string.OPT_UPSCALE;
            tex |= o.title == R.string.OPT_TEX;
        }
        assertTrue(up && tex);
        for (RecompConfig.Option o : GameSettings.options(Console.GC, false))
            assertFalse(o.title == R.string.OPT_TEX);   // the GameCube's renderer has no texture filter override
    }

    @Test
    public void cheatsAreRawWrites() {
        assertNull(Cheats.check(Console.X360, Cheats.normalize("82001000 3F800000\n82001004:1\n")));
        assertEquals(Cheats.Problem.SHAPE, Cheats.check(Console.X360, Cheats.normalize("82001000 3F800000 00")));
        assertEquals(Cheats.Problem.NOT_HEX, Cheats.check(Console.X360, Cheats.normalize("8200100G 1")));
    }

    @Test
    public void coverFromThePack() throws Exception {
        File files = tmp.getRoot();
        String key = "x360-X360-58425A30-00000001";
        assertNull(CoverArt.load(files, key));
        File png = CoverArt.packArtFile(files, key);
        assertTrue(png.getParentFile().mkdirs());
        android.graphics.Bitmap b = android.graphics.Bitmap.createBitmap(4, 4, android.graphics.Bitmap.Config.ARGB_8888);
        try (java.io.FileOutputStream o = new java.io.FileOutputStream(png)) {
            b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, o);
        }
        assertTrue(CoverArt.load(files, key) != null);
    }
}
