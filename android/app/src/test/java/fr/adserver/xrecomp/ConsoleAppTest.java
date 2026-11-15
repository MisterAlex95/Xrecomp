package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import com.android.apksig.ApkVerifier;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * A console game's own app (XBA-215, docs/android/app-shell.md « A console game's own app »): its package name and back,
 * the game's files and pack put into the APK as stored entries (signed: apksig's ApkVerifier), then taken out by the
 * game's app (a .cue pointed at its tracks, the pack published, installed.json naming the APK).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ConsoleAppTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String KEY = "ps1-SLUS_000-01";

    @Test
    public void packageNamesGoBothWays() {
        assertEquals("fr.adserver.xrecomp.t4d5300d1", TitleApk.packageName("4D5300D1"));
        assertEquals("4D5300D1", TitleApk.keyOfPackage("fr.adserver.xrecomp.t4d5300d1"));
        for (String k : Arrays.asList("gba-AXVE01", "ps1-SLUS_000-01", "n64-NSME_E", "gba-a_b-c__d--e")) {
            String pkg = TitleApk.packageName(k);
            assertTrue(pkg, pkg.matches("[a-z]+(\\.[A-Za-z][A-Za-z0-9_]*)+"));
            assertEquals(k, TitleApk.keyOfPackage(pkg));
            assertEquals(k, TitleApk.normKey(k));
        }
        assertEquals("fr.adserver.xrecomp.tps1_SLUS_u000_d01", TitleApk.packageName(KEY));
        assertNull(TitleApk.keyOfPackage("fr.adserver.xrecomp.tgba_A_x"));
        assertNull(TitleApk.keyOfPackage("fr.adserver.xrecomp.shell"));
        assertNull(TitleApk.keyOfPackage("com.example.t4d5300d1"));
        assertNull(TitleApk.normKey("gba-a/b"));
        assertEquals("0000ABCD", TitleApk.normKey("abcd"));
    }

    @Test
    public void unsafeGameFileNamesAreRefused() {
        for (String n : Arrays.asList("../x.bin", "a/b.bin", ".hidden", "", ConsoleApp.INSTALLED)) {
            try {
                ConsoleApp.entries("gba-AXVE01", "x", files(new String[] { n }, new byte[][] { { 1 } }), null);
                fail("accepted " + n);
            } catch (IOException expected) {
                // refused
            }
        }
    }

    private static List<ConsoleApp.GameFile> files(String[] names, byte[][] data) {
        List<ConsoleApp.GameFile> out = new ArrayList<>();
        for (int i = 0; i < names.length; i++) {
            byte[] b = data[i];
            out.add(new ConsoleApp.GameFile(names[i], b.length, () -> new java.io.ByteArrayInputStream(b)));
        }
        return out;
    }

    @Test
    public void theGameGoesIntoTheApkAndComesOut() throws Exception {
        File template = TitleApkTest.resourceApk();
        TitleApk.Spec s = TitleApkTest.spec(template);
        s.titleId = KEY;
        s.xbeSha256 = "";
        // A disc as a .cue and two tracks (the .cue names them in another case), and a prepared pack.
        byte[] cue = ("FILE \"GAME (TRACK 1).BIN\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                      + "FILE \"Game (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n")
                .getBytes(StandardCharsets.ISO_8859_1);
        byte[] t1 = new byte[300_001], t2 = new byte[7];
        new java.util.Random(1).nextBytes(t1);
        File pack = tmp.newFolder("parent-pack");
        byte[] lib = new byte[20_000];
        new java.util.Random(2).nextBytes(lib);
        Files.write(new File(pack, "libtitle.so").toPath(), lib);
        String json = "{\"format\":1,\"console\":\"ps1\",\"title_key\":\"SLUS_000-01\",\"lib\":\"libtitle.so\"}";
        Files.write(new File(pack, Pack.JSON).toPath(), json.getBytes(StandardCharsets.UTF_8));
        s.extra = ConsoleApp.entries(KEY, "Sample Racer", files(
            new String[] { "Game.cue", "Game (Track 1).bin", "Game (Track 2).bin" }, new byte[][] { cue, t1, t2 }),
            pack);

        File signed = tmp.newFile("game.apk");
        TitleApp.Key k = TitleApkTest.makeKey("xba-console");
        TitleApk.build(template, tmp.newFile("u.apk"), signed, s, k.key, k.cert, null, null, null);
        ApkVerifier.Result r = new ApkVerifier.Builder(signed).setMinCheckedPlatformVersion(30).build().verify();
        assertTrue("verified: " + r.getAllErrors(), r.isVerified());
        assertEquals(TitleApk.packageName(KEY), TitleApk.manifestPackage(TitleApkTest.entry(signed,
                                                                                            "AndroidManifest.xml")));
        assertArrayEquals(t1, TitleApkTest.entry(signed, ConsoleApp.GAME_ASSETS + "Game (Track 1).bin"));
        assertArrayEquals(lib, TitleApkTest.entry(signed, ConsoleApp.PACK_ASSETS + "libtitle.so"));

        File files = tmp.newFolder("app-files");
        assertNull(ConsoleApp.installed(files, KEY, "1"));
        long[] last = new long[2];
        ConsoleApp.Installed in = ConsoleApp.install(signed, files, KEY, "1", (d, n) -> {
            last[0] = d;
            last[1] = n;
        }, null);
        File dir = ConsoleApp.gameDir(files);
        assertEquals(new File(dir, "Game.cue").getAbsolutePath(), in.rom);
        assertEquals("Sample Racer", in.title);
        assertEquals(last[1], last[0]);
        assertEquals(cue.length + t1.length + t2.length + lib.length + json.length(), last[1]);
        assertArrayEquals(t1, Files.readAllBytes(new File(dir, "Game (Track 1).bin").toPath()));
        String sheet = new String(Files.readAllBytes(new File(dir, "Game.cue").toPath()), StandardCharsets.ISO_8859_1);
        assertTrue(sheet, sheet.contains("FILE \"" + new File(dir, "Game (Track 1).bin").getAbsolutePath() + "\""));
        assertTrue(sheet, sheet.contains("FILE \"" + new File(dir, "Game (Track 2).bin").getAbsolutePath() + "\""));
        File packDir = Pack.packDir(files, Console.PS1, "SLUS_000-01");
        assertArrayEquals(lib, Files.readAllBytes(new File(packDir, "libtitle.so").toPath()));
        assertEquals(json, new String(Files.readAllBytes(new File(packDir, Pack.JSON).toPath()),
                                      StandardCharsets.UTF_8));
        assertFalse(new File(files, ConsoleApp.DIR + ".partial").exists());

        assertNotNull(ConsoleApp.installed(files, KEY, "1"));
        assertNull("another APK: taken out again", ConsoleApp.installed(files, KEY, "2"));
        assertNull("another game", ConsoleApp.installed(files, "gba-AXVE01", "1"));
        assertNotNull(ConsoleApp.install(signed, files, KEY, "2", null, null));
        assertNotNull(ConsoleApp.installed(files, KEY, "2"));

        // Another game's app does not take this one's files.
        try {
            ConsoleApp.install(signed, tmp.newFolder("other"), "gba-AXVE01", "1", null, null);
            fail("took another game's files");
        } catch (IOException expected) {
            // refused
        }
        // XRecomp's own APK (or an Xbox game's app) has no game in it.
        try {
            ConsoleApp.install(template, tmp.newFolder("none"), KEY, "1", null, null);
            fail("no game in it");
        } catch (IOException expected) {
            // refused
        }
    }
}
