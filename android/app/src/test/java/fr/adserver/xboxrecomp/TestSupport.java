package fr.adserver.xboxrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.View;

import org.json.JSONObject;

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

    static InputStream bytes(byte[] b) {
        return new ByteArrayInputStream(b);
    }

    static File filesDir(Context c) {
        return c.getFilesDir();
    }
}
