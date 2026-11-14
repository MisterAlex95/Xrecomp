package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.graphics.Bitmap;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** CoverArt: the names tried, the URLs, the download and its misses, the captured frame. No network. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class CoverArtTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    /** Answers 200 with a PNG for the URLs in found, 404 for the rest, or throws for every URL when offline. */
    static final class Http implements VkDriverIndex.Http {
        final Map<String, byte[]> found = new HashMap<>();
        final List<String> asked = new ArrayList<>();
        boolean offline;

        @Override
        public VkDriverIndex.Response get(String url, Map<String, String> headers) throws IOException {
            asked.add(url);
            if (offline)
                throw new IOException("offline");
            byte[] b = found.get(url);
            return VkDriverIndexTest.resp(b == null ? 404 : 200, b == null ? new byte[0] : b);
        }
    }

    static byte[] png() {
        Bitmap b = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        b.eraseColor(0xFF3366CC);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        b.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }

    @Test
    public void candidatesFromADumpName() {
        List<String> c = CoverArt.candidates("/sdcard/PS1/Crash Bandicoot (Europe) (EDC).cue");
        assertEquals("Crash Bandicoot (Europe) (EDC)", c.get(0));
        assertTrue(c.contains("Crash Bandicoot (Europe)"));
        assertTrue(c.contains("Crash Bandicoot (USA)"));
        assertEquals("Crash Bandicoot", c.get(c.size() - 1));
        assertEquals(c.size(), new java.util.HashSet<>(c).size());

        c = CoverArt.candidates("Pokemon Pinball - Ruby & Sapphire (USA) [!].gba");
        assertEquals("Pokemon Pinball - Ruby & Sapphire (USA) [!]", c.get(0));
        assertEquals("Pokemon Pinball - Ruby & Sapphire (USA)", c.get(1));

        c = CoverArt.candidates("Final Fantasy VII (France) (Disc 2).chd");
        assertTrue(c.contains("Final Fantasy VII (USA) (Disc 2)"));

        assertTrue(CoverArt.candidates("").isEmpty());
        assertTrue(CoverArt.candidates(null).isEmpty());
    }

    @Test
    public void urlsFollowLibretroNaming() {
        assertEquals("Pokemon Pinball - Ruby _ Sapphire (USA)", CoverArt.sanitize("Pokemon Pinball - Ruby & Sapphire (USA)"));
        assertEquals("https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/"
                     + "Crash%20Bandicoot%20%28Europe%29.png",
                     CoverArt.url(Console.PS1, "Crash Bandicoot (Europe)"));
        assertTrue(CoverArt.url(Console.N64, "x").contains("/Nintendo_-_Nintendo_64/"));
        assertTrue(CoverArt.url(Console.GBA, "x").contains("/Nintendo_-_Game_Boy_Advance/"));
        assertNull(CoverArt.system(Console.XBOX));
    }

    @Test
    public void fetchStoresTheFirstFoundAndForgetsTheMiss() throws IOException {
        File files = tmp.newFolder("files");
        Http http = new Http();
        String key = "ps1-SCES-00344";
        http.found.put(CoverArt.url(Console.PS1, "Crash Bandicoot (Europe)"), png());
        assertTrue(CoverArt.wanted(files, key, System.currentTimeMillis()));
        assertTrue(CoverArt.fetch(http, files, key, "Crash Bandicoot (Europe) (EDC).cue"));
        assertEquals(2, http.asked.size());   // the full name (404), then without (EDC)
        assertTrue(CoverArt.artFile(files, key).isFile());
        assertFalse(CoverArt.missFile(files, key).exists());
        assertNotNull(CoverArt.load(files, key));
        assertFalse("found: not asked again", CoverArt.wanted(files, key, System.currentTimeMillis()));
    }

    @Test
    public void aMissIsRememberedForAWhile() throws IOException {
        File files = tmp.newFolder("files");
        Http http = new Http();
        String key = "gba-ABCD";
        assertFalse(CoverArt.fetch(http, files, key, "Homebrew Thing.gba"));
        assertTrue(CoverArt.missFile(files, key).isFile());
        long now = System.currentTimeMillis();
        assertFalse(CoverArt.wanted(files, key, now));
        assertTrue(CoverArt.wanted(files, key, now + CoverArt.MISS_RETRY_MS + 60_000));
    }

    @Test
    public void offlineWritesNothing() throws IOException {
        File files = tmp.newFolder("files");
        Http http = new Http();
        http.offline = true;
        String key = "n64-NZLP";
        assertFalse(CoverArt.fetch(http, files, key, "Zelda (Europe).z64"));
        assertEquals(1, http.asked.size());
        assertFalse(CoverArt.missFile(files, key).exists());
        assertTrue(CoverArt.wanted(files, key, System.currentTimeMillis()));
    }

    @Test
    public void notAnImageIsNotKept() throws IOException {
        File files = tmp.newFolder("files");
        Http http = new Http();
        String key = "gba-EFGH";
        for (String n : CoverArt.candidates("Game (USA).gba"))
            http.found.put(CoverArt.url(Console.GBA, n), "<html>".getBytes());
        assertFalse(CoverArt.fetch(http, files, key, "Game (USA).gba"));
        assertFalse(CoverArt.artFile(files, key).exists());
    }

    @Test
    public void aCapturedFrameUntilARealCover() throws IOException {
        File files = tmp.newFolder("files");
        String key = "ps1-SLUS-00001";
        Bitmap frame = Bitmap.createBitmap(8, 6, Bitmap.Config.ARGB_8888);
        assertNull(CoverArt.load(files, key));
        assertTrue(CoverArt.storeShot(frame, files, key));
        assertFalse("kept once", CoverArt.storeShot(frame, files, key));
        assertEquals(8, CoverArt.load(files, key).getWidth());
        assertTrue("a capture does not stop the download", CoverArt.wanted(files, key, System.currentTimeMillis()));
        assertTrue(CoverArt.store(png(), CoverArt.artFile(files, key)));
        assertEquals(4, CoverArt.load(files, key).getWidth());   // the downloaded cover wins
        assertFalse(CoverArt.wanted(files, "xbox-ish", 0));
    }
}
