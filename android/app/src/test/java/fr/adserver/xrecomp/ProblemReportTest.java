package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.io.FileOutputStream;
import java.util.Random;

/** XBA-339: the line made one short line, the screenshot downscaled and size-capped, the report it builds. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ProblemReportTest {
    @Test
    public void theLineIsOneShortLine() {
        assertEquals("", ProblemReport.clean(null));
        assertEquals("", ProblemReport.clean("  \n\t "));
        assertEquals("No sound after the intro", ProblemReport.clean("  No sound\nafter   the intro \n"));
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 800; i++)
            b.append('x');
        assertEquals(ProblemReport.TEXT_MAX, ProblemReport.clean(b.toString()).length());
    }

    @Test
    public void shotSizeKeepsTheShapeUnder640() {
        assertArrayEquals(new int[] { 640, 360 }, ProblemReport.shotSize(1920, 1080));
        assertArrayEquals(new int[] { 640, 480 }, ProblemReport.shotSize(5120, 3840));   // an 8x N64 frame
        assertArrayEquals(new int[] { 480, 640 }, ProblemReport.shotSize(1080, 1440));
        assertArrayEquals(new int[] { 320, 240 }, ProblemReport.shotSize(320, 240));     // smaller: as it is
        assertArrayEquals(new int[] { 0, 0 }, ProblemReport.shotSize(0, 10));
        assertEquals(1, ProblemReport.sampleSize(640, 480));
        assertEquals(2, ProblemReport.sampleSize(1920, 1080));
        assertEquals(8, ProblemReport.sampleSize(5120, 3840));
        int[] big = { 5120, 3840 };
        int s = ProblemReport.sampleSize(big[0], big[1]);
        assertTrue("decoded no smaller than what is sent", Math.max(big[0], big[1]) / s >= ProblemReport.SHOT_SIDE);
    }

    private static Bitmap noise(int w, int h) {
        Random r = new Random(7);
        int[] px = new int[w * h];
        for (int i = 0; i < px.length; i++)
            px[i] = 0xFF000000 | r.nextInt(0xFFFFFF);
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }

    @Test
    public void jpegIsDownscaledAndUnderTheCap() {
        assertNull(ProblemReport.jpeg(null));
        // The worst case for JPEG: noise, at a big frame's size.
        byte[] j = ProblemReport.jpeg(noise(1920, 1080));
        assertNotNull(j);
        assertTrue(j.length + " bytes", j.length <= CrashReport.SHOT_MAX);
        assertEquals((byte)0xFF, j[0]);
        assertEquals((byte)0xD8, j[1]);   // a JPEG
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(j, 0, j.length, o);
        assertEquals(640, o.outWidth);
        assertEquals(360, o.outHeight);
        Bitmap pv = ProblemReport.preview(j);
        assertEquals(640, pv.getWidth());
    }

    @Test
    public void aFrameFileIsReadAndDeleted() throws Exception {
        Context c = RuntimeEnvironment.getApplication();
        File f = new File(c.getCacheDir(), "frame.png");
        try (FileOutputStream o = new FileOutputStream(f)) {
            noise(1280, 960).compress(Bitmap.CompressFormat.PNG, 100, o);
        }
        byte[] j = ProblemReport.jpegFromFile(f);
        assertNotNull(j);
        assertFalse("the frame file goes", f.exists());
        Bitmap b = BitmapFactory.decodeByteArray(j, 0, j.length);
        assertEquals(640, b.getWidth());
        assertEquals(480, b.getHeight());
        assertNull(ProblemReport.jpegFromFile(null));
        File bad = new File(c.getCacheDir(), "bad.bmp");
        java.nio.file.Files.write(bad.toPath(), new byte[] { 1, 2, 3 });
        assertNull(ProblemReport.jpegFromFile(bad));
        assertFalse(bad.exists());
    }

    @Test
    public void buildsAFeedbackWithAnOptionalScreenshot() {
        Context c = RuntimeEnvironment.getApplication();
        CrashReport r = ProblemReport.build(c, ProblemReport.FROM_GAME, "n64-abc", "Some Game", " Black\nscreen ",
                                            null, "game.log", null);
        assertEquals(CrashReport.KIND_FEEDBACK, r.kind);
        assertEquals("Black screen", r.feedback);
        assertEquals("game", r.tags.get("from"));
        assertEquals("n64", r.tags.get("console"));
        assertNull(r.image);
        byte[] small = { (byte)0xFF, (byte)0xD8, 1, 2 };
        ProblemReport.setShot(r, small);
        assertArrayEquals(small, r.image);
        assertTrue(r.summary().contains("screenshot.jpg"));
        ProblemReport.setShot(r, null);
        assertNull(r.image);
        assertFalse(r.summary().contains("screenshot.jpg"));
        ProblemReport.setShot(r, new byte[CrashReport.SHOT_MAX + 1]);   // never listed when it would be dropped
        assertNull(r.image);
    }
}
