package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.View;

import org.json.JSONObject;
import org.junit.rules.ExternalResource;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Fixtures for the Prepare tests: an XBE header, a pack, zips as assets, a fake nativeLibraryDir, screenshots. */
final class TestSupport {
    private TestSupport() {}

    static void write(File f, String text) throws IOException {
        write(f, text.getBytes(StandardCharsets.UTF_8));
    }

    static void write(File f, byte[] b) throws IOException {
        File d = f.getParentFile();
        if (d != null)
            d.mkdirs();
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(b);
        }
    }

    /** A minimal XBE: "XBEH", base 0x10000, certificate at 0x10200 with the title ID and a UTF-16 name. */
    static byte[] xbe(int titleId, String name) {
        byte[] b = new byte[0x400];
        b[0] = 'X';
        b[1] = 'B';
        b[2] = 'E';
        b[3] = 'H';
        le32(b, 0x104, 0x10000);
        le32(b, 0x118, 0x10200);
        le32(b, 0x200 + 0x08, titleId);
        byte[] n = name.getBytes(StandardCharsets.UTF_16LE);
        System.arraycopy(n, 0, b, 0x200 + 0x0c, Math.min(n.length, 80));
        return b;
    }

    /** xbe() with the certificate's game region (+0xA0), disc number (+0xA8) and version (+0xAC) set. */
    static byte[] xbe(int titleId, String name, int region, int disc, int version) {
        byte[] b = xbe(titleId, name);
        le32(b, 0x200 + 0xa0, region);
        le32(b, 0x200 + 0xa8, disc);
        le32(b, 0x200 + 0xac, version);
        return b;
    }

    /**
     * A minimal XBE as xbe() with one section, $$XTIMAGE, holding image (an XPR0 built by xpr0()): headers up to
     * 0x400 (section header at 0x300, its name at 0x340), the section's bytes from 0x400. Ours, no game content.
     */
    static byte[] xbeWithImage(int titleId, String name, byte[] image) {
        return xbeWithSection(titleId, name, TitleArt.SECTION, image);
    }

    /** xbeWithImage() with the one section called section ($$XSIMAGE, ...). */
    static byte[] xbeWithSection(int titleId, String name, String section, byte[] image) {
        byte[] head = xbe(titleId, name);
        byte[] b = new byte[0x400 + image.length];
        System.arraycopy(head, 0, b, 0, head.length);
        le32(b, 0x108, 0x400);              // size of headers
        le32(b, 0x11c, 1);                  // one section
        le32(b, 0x120, 0x10300);            // section headers at base + 0x300
        le32(b, 0x300 + 0x0c, 0x400);       // raw address
        le32(b, 0x300 + 0x10, image.length);
        le32(b, 0x300 + 0x14, 0x10340);     // name address
        byte[] n = section.getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(n, 0, b, 0x340, n.length);
        System.arraycopy(image, 0, b, 0x400, image.length);
        return b;
    }

    /** An XPR0 file with one texture: format code fmt, w x h (powers of two), data placed after a 2048-byte header. */
    static byte[] xpr0(int fmt, int w, int h, byte[] data) {
        int hdr = 0x800;
        byte[] b = new byte[hdr + data.length];
        java.util.Arrays.fill(b, 12, hdr, (byte)0xAD);
        b[0] = 'X';
        b[1] = 'P';
        b[2] = 'R';
        b[3] = '0';
        le32(b, 4, b.length);
        le32(b, 8, hdr);
        le32(b, 12, 0x00040001);            // a texture, one reference
        le32(b, 16, 0);                     // its data: right after the header
        le32(b, 20, 0);
        le32(b, 24, 0x29 | fmt << 8 | 1 << 16 | Integer.numberOfTrailingZeros(w) << 20
                    | Integer.numberOfTrailingZeros(h) << 24);
        le32(b, 28, 0);
        le32(b, 32, 0xFFFFFFFF);
        System.arraycopy(data, 0, b, hdr, data.length);
        return b;
    }

    /** An XPR0 with one linear A8R8G8B8 texture (format 0x12) of w x h pixels (plain Java, no android.*). */
    static byte[] xprArgb(int w, int h, int[] argb) {
        byte[] d = new byte[w * h * 4];
        for (int i = 0; i < w * h; i++)
            le32(d, 4 * i, argb[i]);
        return xpr0(0x12, w, h, d);
    }

    /** xprArgb() of one colour. */
    static byte[] xprSolid(int w, int h, int argb) {
        int[] px = new int[w * h];
        java.util.Arrays.fill(px, argb);
        return xprArgb(w, h, px);
    }

    static int rgb565(int r, int g, int b) {
        return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
    }

    /**
     * A 128 x 128 DXT1 picture made here: concentric rings over a hue sweep, so the art and its tint are visible in
     * the screenshots.
     */
    static byte[] dxt1Picture() {
        int n = 32;
        byte[] d = new byte[n * n * 8];
        for (int by = 0; by < n; by++) {
            for (int bx = 0; bx < n; bx++) {
                float hue = 190 + (bx + by) * 1.6f;
                int c0 = android.graphics.Color.HSVToColor(new float[] { hue % 360, 0.75f, 0.95f });
                int c1 = android.graphics.Color.HSVToColor(new float[] { (hue + 50) % 360, 0.85f, 0.22f });
                int a = rgb565(c0 >> 16 & 0xff, c0 >> 8 & 0xff, c0 & 0xff);
                int z = rgb565(c1 >> 16 & 0xff, c1 >> 8 & 0xff, c1 & 0xff);
                if (a <= z) {   // keep the four-colour mode
                    int t = a;
                    a = z;
                    z = t;
                }
                int o = (by * n + bx) * 8;
                d[o] = (byte)a;
                d[o + 1] = (byte)(a >> 8);
                d[o + 2] = (byte)z;
                d[o + 3] = (byte)(z >> 8);
                int idx = 0;
                for (int i = 0; i < 16; i++) {
                    float x = bx * 4 + (i & 3) - 64, y = by * 4 + (i >> 2) - 64;
                    double r = Math.sqrt(x * x + y * y);
                    int band = (int)(r / 6) % 4;
                    int code = band == 0 ? 0 : band == 1 ? 2 : band == 2 ? 3 : 1;
                    idx |= code << (2 * i);
                }
                le32(d, o + 4, idx);
            }
        }
        return d;
    }

    static void le32(byte[] b, int o, int v) {
        b[o] = (byte)v;
        b[o + 1] = (byte)(v >> 8);
        b[o + 2] = (byte)(v >> 16);
        b[o + 3] = (byte)(v >> 24);
    }

    /** A valid pack for titleId under filesDir/packs, with a lib of libSize bytes. */
    static File writePack(File filesDir, String titleId, int libSize) throws Exception {
        File dir = Pack.packDir(filesDir, titleId);
        write(new File(dir, "libtitle.so"), new byte[libSize]);
        JSONObject o = new JSONObject();
        o.put("format", 1);
        o.put("title_id", titleId);
        o.put("title", "Test title");
        o.put("xbe_sha256", "00");
        o.put("abi", "arm64-v8a");
        o.put("shell_abi", 1);
        o.put("toolkit", "test");
        o.put("lib", "libtitle.so");
        o.put("lib_sha256", "00");
        o.put("lib_size", libSize);
        o.put("entry", "recomp_title_main");
        o.put("cc", "clang");
        o.put("built_at", "2026-11-10T00:00:00Z");
        write(new File(dir, "pack.json"), o.toString(1));
        return dir;
    }

    static byte[] zip(String... namesAndContents) throws IOException {
        java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(bo)) {
            for (int i = 0; i < namesAndContents.length; i += 2) {
                z.putNextEntry(new ZipEntry(namesAndContents[i]));
                if (!namesAndContents[i].endsWith("/"))
                    z.write(namesAndContents[i + 1].getBytes(StandardCharsets.UTF_8));
                z.closeEntry();
            }
        }
        return bo.toByteArray();
    }

    /** Assets from a directory on disk (prepare/... under root). */
    static final class DirAssets implements Toolchain.AssetSource {
        final File root;
        int opens;

        DirAssets(File root) {
            this.root = root;
        }

        @Override
        public String[] list(String dir) {
            String[] l = new File(root, dir).list();
            return l == null ? new String[0] : l;
        }

        @Override
        public InputStream open(String path) throws IOException {
            File f = new File(root, path);
            if (!f.isFile())
                throw new java.io.FileNotFoundException(path);
            opens++;
            return new FileInputStream(f);
        }
    }

    /** The three tool zips as assets in root/prepare (the toolkit with a VERSION file). */
    static DirAssets toolAssets(File root, String toolkitVersion) throws IOException {
        write(new File(root, "prepare/python-stdlib.zip"), zip("lib/python3.13/os.py", "# os\n"));
        write(new File(root, "prepare/sysroot.zip"), zip("usr/include/", "", "usr/include/stdio.h", "int x;\n"));
        write(new File(root, "prepare/toolkit.zip"), zip("VERSION", toolkitVersion + "\n", "tools/prepare.py", "#\n"));
        return new DirAssets(root);
    }

    /** A nativeLibraryDir with the Prepare's executables (empty files: present is all the launcher checks). */
    static File fakeNativeDir(File root) throws IOException {
        File d = new File(root, "lib");
        write(new File(d, Toolchain.PYTHON), new byte[0]);
        write(new File(d, Toolchain.CC), new byte[0]);
        write(new File(d, Toolchain.SHELL_LIB), new byte[0]);
        return d;
    }

    static List<String> sh(File script, String... args) {
        List<String> c = new ArrayList<>();
        c.add("/bin/sh");
        c.add(script.getAbsolutePath());
        java.util.Collections.addAll(c, args);
        return c;
    }

    /** Draws a view into build/screenshots/<name>.png (the xba.screenshots system property). */
    static File screenshot(View root, String name) throws IOException {
        String dir = System.getProperty("xba.screenshots", "build/screenshots");
        File out = new File(dir, name + ".png");
        out.getParentFile().mkdirs();
        Bitmap bm = Bitmap.createBitmap(Math.max(1, root.getWidth()), Math.max(1, root.getHeight()),
                                        Bitmap.Config.ARGB_8888);
        root.draw(new android.graphics.Canvas(bm));
        try (OutputStream o = new FileOutputStream(out)) {
            bm.compress(Bitmap.CompressFormat.PNG, 100, o);
        }
        return out;
    }

    /**
     * Puts the stock colours back after each test (Theme's tokens are mutable statics, GameTheme applies a game's),
     * and bakes a missing game theme in place rather than on a thread (the launcher's re-show stays posted), so a
     * launcher test sees the same colours at every run:
     * {@code @Rule public final TestSupport.StockTheme stock = new TestSupport.StockTheme();}
     */
    static final class StockTheme extends ExternalResource {
        @Override
        protected void before() {
            Theme.reset();
            LauncherActivity.themeExecutor = Runnable::run;
        }

        @Override
        protected void after() {
            Theme.reset();
            LauncherActivity.themeExecutor = LauncherActivity.THEME_THREAD;
        }
    }

    static InputStream bytes(byte[] b) {
        return new ByteArrayInputStream(b);
    }

    static File filesDir(Context c) {
        return c.getFilesDir();
    }
}
