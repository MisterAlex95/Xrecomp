package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import com.android.apksig.ApkVerifier;

import org.junit.BeforeClass;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * A game's own app (docs/android/app-shell.md « A game's own app »): the binary manifest model (BinaryXml: a round
 * trip gives the same bytes), the manifest patch (package, label, icon slot, authorities, class names, one-game
 * meta-data), the zip copy (entries, alignment, the old signature dropped) and the v2 signature checked by apksig's
 * ApkVerifier.
 *
 * The template is the resource APK the build makes for these tests (test_config.properties' android_resource_apk:
 * aapt2's output for this app, with the real merged manifest and resources.arsc, no dex): no game data. The signed
 * copy is also written to build/title-app/ for a check with aapt2 and apksigner (docs: « Tests »).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TitleApkTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String TID = "4D5300D1";
    static final String SHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    private static TitleApp.Key key, otherKey;

    @BeforeClass
    public static void keys() throws Exception {
        key = makeKey("xba-test-1");
        otherKey = makeKey("xba-test-2");
    }

    /** An EC P-256 key and its self-signed certificate made by the JDK's keytool (a PKCS12 file, then loaded). */
    static TitleApp.Key makeKey(String name) throws Exception {
        File dir = Files.createTempDirectory("xba-key").toFile();
        File ks = new File(dir, name + ".p12");
        String keytool = new File(System.getProperty("java.home"), "bin/keytool").getPath();
        Process p = new ProcessBuilder(keytool, "-genkeypair", "-keystore", ks.getPath(), "-storetype", "PKCS12",
                                       "-storepass", "android", "-keypass", "android", "-alias", "k", "-keyalg", "EC",
                                       "-groupname", "secp256r1", "-validity", "10000", "-dname", "CN=" + name)
            .redirectErrorStream(true).start();
        byte[] out = p.getInputStream().readAllBytes();
        assertEquals(new String(out), 0, p.waitFor());
        KeyStore k = KeyStore.getInstance("PKCS12");
        try (InputStream in = new FileInputStream(ks)) {
            k.load(in, "android".toCharArray());
        }
        return new TitleApp.Key((PrivateKey)k.getKey("k", "android".toCharArray()),
                                (X509Certificate)k.getCertificate("k"));
    }

    /** The resource APK of this build (aapt2 link of the app: binary manifest, resources.arsc, res/). */
    static File resourceApk() throws IOException {
        Properties p = new Properties();
        try (InputStream in = TitleApkTest.class.getClassLoader()
                                  .getResourceAsStream("com/android/tools/test_config.properties")) {
            assertNotNull("test_config.properties (includeAndroidResources)", in);
            p.load(in);
        }
        File f = new File(p.getProperty("android_resource_apk"));
        assertTrue(f + " exists", f.isFile());
        return f;
    }

    static String iconEntry(File apk) throws IOException {
        try (ZipFile z = new ZipFile(apk)) {
            for (ZipEntry e : java.util.Collections.list(z.entries())) {
                if (e.getName().contains(TitleApk.ICON_LAYER) && e.getName().endsWith(".png"))
                    return e.getName();
            }
        }
        throw new AssertionError("no icon slot layer in " + apk);
    }

    static byte[] entry(File apk, String name) throws IOException {
        try (ZipFile z = new ZipFile(apk)) {
            ZipEntry e = z.getEntry(name);
            if (e == null)
                return null;
            try (InputStream in = z.getInputStream(e)) {
                return in.readAllBytes();
            }
        }
    }

    static TitleApk.Spec spec(File template) throws IOException {
        TitleApk.Spec s = new TitleApk.Spec();
        s.titleId = TID;
        s.label = "Sample Quest: The Lost Tower";
        s.xbeSha256 = SHA;
        s.iconEntry = iconEntry(template);
        s.iconRes = R.mipmap.xba_title_icon;
        s.iconPng = TitleApp.iconPng(null, TID, s.label);
        return s;
    }

    // ---- BinaryXml ---------------------------------------------------------------------------------------------

    @Test
    public void binaryXmlRoundTripGivesTheSameBytes() throws Exception {
        byte[] m = TitleApk.readManifest(resourceApk());
        assertArrayEquals(m, BinaryXml.parse(m).toBytes());
    }

    @Test
    public void binaryXmlInsertedAttributeName() throws Exception {
        byte[] m = TitleApk.readManifest(resourceApk());
        BinaryXml x = BinaryXml.parse(m);
        int n = x.strings.size(), prefix = x.resIds.length;
        String pkg = x.value(x.plainAttr(x.root(), "package"));
        // A resource id the manifest has no attribute for (android:handleProfiling) moves every later index up by one.
        int at = x.attrName("handleProfiling", 0x01010022);
        assertEquals(prefix, at);
        assertEquals(n + 1, x.strings.size());
        BinaryXml y = BinaryXml.parse(x.toBytes());
        assertEquals(pkg, y.value(y.plainAttr(y.root(), "package")));
        assertEquals("application", y.name(y.children(y.root(), "application").get(0)));
        assertEquals(0x01010022, y.resIds[at]);
    }

    // ---- The manifest patch ----------------------------------------------------------------------------------

    @Test
    public void manifestPatch() throws Exception {
        File apk = resourceApk();
        byte[] m = TitleApk.readManifest(apk);
        String old = TitleApk.manifestPackage(m);
        assertNotNull(old);
        TitleApk.Spec s = spec(apk);
        byte[] p = TitleApk.patchManifest(m, s);
        BinaryXml x = BinaryXml.parse(p);
        int root = x.root();
        assertEquals("fr.adserver.xrecomp.t4d5300d1", x.value(x.plainAttr(root, "package")));
        int app = x.children(root, "application").get(0);
        BinaryXml.Attr label = x.attr(app, TitleApk.ATTR_LABEL);
        assertEquals(BinaryXml.TYPE_STRING, label.type);
        assertEquals("Sample Quest: The Lost Tower", x.value(label));
        for (int id : new int[] { TitleApk.ATTR_ICON, TitleApk.ATTR_ROUND_ICON }) {
            BinaryXml.Attr icon = x.attr(app, id);
            assertEquals(BinaryXml.TYPE_REFERENCE, icon.type);
            assertEquals(R.mipmap.xba_title_icon, icon.data);
        }
        // Every element's attributes stay sorted by resource id (the framework walks them in that order).
        for (int el : x.descendants(root)) {
            int last = 0;
            for (BinaryXml.Attr a : x.nodes.get(el).attrs) {
                int id = a.name < x.resIds.length ? x.resIds[a.name] : 0;
                if (id == 0)
                    continue;
                assertTrue(x.name(el) + " attributes sorted", Integer.compareUnsigned(id, last) > 0);
                last = id;
            }
        }
        // The providers' authorities follow the package (the copy's never clash with XRecomp's: logs, and the
        // packs of the handover, XBA-176); components keep their full class names.
        List<String> names = new ArrayList<>();
        List<String> authorities = new ArrayList<>();
        for (int el : x.descendants(app)) {
            String tag = x.name(el);
            if (tag.equals("provider"))
                authorities.add(x.value(x.attr(el, TitleApk.ATTR_AUTHORITIES)));
            if (tag.equals("activity") || tag.equals("service") || tag.equals("receiver") || tag.equals("provider"))
                names.add(x.value(x.attr(el, TitleApk.ATTR_NAME)));
        }
        assertEquals(java.util.Arrays.asList("fr.adserver.xrecomp.t4d5300d1.logs",
                                             "fr.adserver.xrecomp.t4d5300d1.packs"), authorities);
        assertTrue(names.contains("fr.adserver.xrecomp.PackHandoverActivity"));
        // The handover's <queries>: the copy still looks for its parent, by the parent's package.
        List<String> queried = new ArrayList<>();
        for (int el : x.descendants(root)) {
            if (x.name(el).equals("package"))
                queried.add(x.value(x.attr(el, TitleApk.ATTR_NAME)));
        }
        assertEquals(java.util.Collections.singletonList(old), queried);
        assertTrue(names.contains("fr.adserver.xrecomp.LauncherActivity"));
        assertTrue(names.contains("fr.adserver.xrecomp.ShellActivity"));
        assertTrue(names.contains("fr.adserver.xrecomp.TitleAppReceiver"));
        for (String n : names)
            assertTrue(n, n.startsWith("fr.adserver.xrecomp.") && !n.startsWith(s.packageName()));
        // Nothing of the old package is left in a value (it may stay as an unused pool string), but the parent's
        // name in META_PARENT.
        for (int el : x.descendants(root)) {
            if (x.name(el).equals("meta-data") && TitleApk.META_PARENT.equals(x.value(x.attr(el, TitleApk.ATTR_NAME))))
                continue;
            if (x.name(el).equals("package"))   // <queries><package>: the parent, on purpose
                continue;
            for (BinaryXml.Attr a : x.nodes.get(el).attrs) {
                String v = x.value(a);
                if (v != null)
                    assertFalse(x.name(el) + ": " + v, v.equals(old) || v.startsWith(old + ".")
                                                       || v.startsWith(old + ":"));
            }
        }
        // INTERNET (the Drivers screen, VkDriverIndex) is XRecomp's only: the template asks for it, the copy does not.
        // The other permissions stay.
        assertTrue(usesPermissions(BinaryXml.parse(m)).contains("android.permission.INTERNET"));
        List<String> perms = usesPermissions(x);
        assertFalse(perms.toString(), perms.contains("android.permission.INTERNET"));
        assertTrue(perms.contains("android.permission.VIBRATE"));
        assertTrue(perms.contains("android.permission.REQUEST_INSTALL_PACKAGES"));
        assertEquals(usesPermissions(BinaryXml.parse(m)).size() - 1, perms.size());
        // The one-game meta-data.
        Map<String, String> meta = meta(x);
        assertEquals(TID, meta.get(TitleApk.META_TITLE_ID));
        assertEquals(SHA, meta.get(TitleApk.META_XBE_SHA256));
        assertEquals(old, meta.get(TitleApk.META_PARENT));
        // Patching the copy again changes nothing more (the meta-data are replaced, not added twice).
        byte[] again = TitleApk.patchManifest(p, s);
        assertEquals(3, metaCount(BinaryXml.parse(again)));
        assertEquals(meta, meta(BinaryXml.parse(again)));
        assertFalse(usesPermissions(BinaryXml.parse(again)).contains("android.permission.INTERNET"));
    }

    private static List<String> usesPermissions(BinaryXml x) {
        List<String> l = new ArrayList<>();
        for (int el : x.descendants(x.root())) {
            if (x.name(el).startsWith("uses-permission"))
                l.add(x.value(x.attr(el, TitleApk.ATTR_NAME)));
        }
        return l;
    }

    private static Map<String, String> meta(BinaryXml x) {
        Map<String, String> m = new HashMap<>();
        int app = x.children(x.root(), "application").get(0);
        for (int md : x.children(app, "meta-data"))
            m.put(x.value(x.attr(md, TitleApk.ATTR_NAME)), x.value(x.attr(md, TitleApk.ATTR_VALUE)));
        return m;
    }

    private static int metaCount(BinaryXml x) {
        int app = x.children(x.root(), "application").get(0);
        int n = 0;
        for (int md : x.children(app, "meta-data")) {
            String name = x.value(x.attr(md, TitleApk.ATTR_NAME));
            if (name.startsWith("fr.adserver.xrecomp."))
                n++;
        }
        return n;
    }

    @Test
    public void derivedNamesAndLabels() {
        String old = "fr.adserver.xrecomp.shell", pkg = "fr.adserver.xrecomp.t4d5300d1";
        assertEquals(pkg, TitleApk.derived(old, old, pkg));
        assertEquals(pkg + ".logs", TitleApk.derived(old + ".logs", old, pkg));
        assertEquals("fr.adserver.xrecomp.LauncherActivity",
                     TitleApk.derived("fr.adserver.xrecomp.LauncherActivity", old, pkg));
        assertEquals("fr.adserver.xrecomp.shellfish", TitleApk.derived("fr.adserver.xrecomp.shellfish", old, pkg));
        assertEquals("fr.adserver.xrecomp.tabcdef01", TitleApk.packageName("ABCDEF01"));
        assertEquals("Burnout 3 Takedown", TitleApk.label("  Burnout 3\t Takedown "));
        assertEquals(TitleApk.LABEL_MAX, TitleApk.label("x".repeat(80)).length());
        assertTrue(TitleApk.oldSignature("META-INF/MANIFEST.MF"));
        assertTrue(TitleApk.oldSignature("META-INF/CERT.SF"));
        assertTrue(TitleApk.oldSignature("META-INF/CERT.RSA"));
        assertTrue(TitleApk.oldSignature("META-INF/KEY.EC"));
        assertTrue(TitleApk.oldSignature("stamp-cert-sha256"));
        assertFalse(TitleApk.oldSignature("META-INF/services/foo"));
        assertFalse(TitleApk.oldSignature("META-INF/com/android/build/gradle/app-metadata.properties"));
        assertFalse(TitleApk.oldSignature("res/raw/x.RSA"));
        assertEquals("res/drawable-nodpi-v4/xba_title_icon_art.png",
                     TitleApp.entryName("res/drawable-nodpi-v4/xba_title_icon_art.png"));
        assertEquals("res/drawable-nodpi-v4/xba_title_icon_art.png",
                     TitleApp.entryName("/tmp/robolectric/app/res/drawable-nodpi-v4/xba_title_icon_art.png"));
    }

    // ---- The zip copy ----------------------------------------------------------------------------------------

    /**
     * A template with what an installed APK has around the manifest: stored and deflated entries with names of every
     * length (so alignment padding varies), a stored .so, a v1 signature and a source stamp.
     */
    private File template() throws Exception {
        File res = resourceApk();
        File t = tmp.newFile("template.apk");
        try (ZipFile in = new ZipFile(res); ZipOutputStream out = new ZipOutputStream(new FileOutputStream(t))) {
            for (ZipEntry e : java.util.Collections.list(in.entries())) {
                byte[] b;
                try (InputStream is = in.getInputStream(e)) {
                    b = is.readAllBytes();
                }
                put(out, e.getName(), b, e.getMethod() == ZipEntry.STORED);
            }
            for (int i = 0; i < 7; i++)
                put(out, "assets/pad" + "x".repeat(i) + ".bin", new byte[13 + i], true);
            put(out, "lib/arm64-v8a/libstored.so", new byte[5000], true);
            put(out, "lib/arm64-v8a/libdeflated.so", new byte[5000], false);
            put(out, "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n".getBytes(), false);
            put(out, "META-INF/CERT.SF", new byte[10], false);
            put(out, "META-INF/CERT.RSA", new byte[10], false);
            put(out, "stamp-cert-sha256", new byte[32], true);
        }
        return t;
    }

    private static void put(ZipOutputStream out, String name, byte[] b, boolean stored) throws IOException {
        ZipEntry e = new ZipEntry(name);
        if (stored) {
            CRC32 c = new CRC32();
            c.update(b);
            e.setMethod(ZipEntry.STORED);
            e.setSize(b.length);
            e.setCompressedSize(b.length);
            e.setCrc(c.getValue());
        }
        out.putNextEntry(e);
        out.write(b);
        out.closeEntry();
    }

    /** The data offset of every stored entry is aligned (4, a .so 16384). */
    private static void assertAligned(File apk, int soAlign) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(apk, "r")) {
            int stored = 0;
            for (ApkZip.Entry e : ApkZip.entries(f)) {
                if (e.method != ApkZip.STORED)
                    continue;
                stored++;
                int align = e.name.endsWith(".so") ? soAlign : 4;
                assertEquals(e.name + " at " + e.dataOffset, 0, e.dataOffset % align);
            }
            assertTrue(stored > 3);
        }
    }

    @Test
    public void zipCopyReplacesDropsAndAligns() throws Exception {
        File t = template();
        File out = tmp.newFile("unsigned.apk");
        TitleApk.Spec s = spec(t);
        long[] last = new long[2];
        TitleApk.rewrite(t, out, s, (d, n) -> {
            last[0] = d;
            last[1] = n;
        }, null);
        assertEquals(t.length(), last[1]);
        assertTrue(last[0] > 0);
        assertAligned(out, ApkZip.SO_ALIGN);
        List<String> before = names(t), after = names(out);
        for (String n : before) {
            if (TitleApk.oldSignature(n))
                assertFalse(n + " dropped", after.contains(n));
            else
                assertTrue(n + " kept", after.contains(n));
        }
        // Copied raw: the same bytes; replaced: the new ones; java.util.zip checks every CRC on reading.
        assertArrayEquals(entry(t, "resources.arsc"), entry(out, "resources.arsc"));
        assertArrayEquals(entry(t, "lib/arm64-v8a/libdeflated.so"), entry(out, "lib/arm64-v8a/libdeflated.so"));
        assertArrayEquals(s.iconPng, entry(out, s.iconEntry));
        assertEquals("fr.adserver.xrecomp.t4d5300d1",
                     TitleApk.manifestPackage(entry(out, "AndroidManifest.xml")));
        Bitmap icon = BitmapFactory.decodeByteArray(entry(out, s.iconEntry), 0, s.iconPng.length);
        assertEquals(TitleShortcut.SIDE, icon.getWidth());
    }

    @Test
    public void zipCopyCancels() throws Exception {
        File t = template();
        File out = tmp.newFile("cancelled.apk");
        try {
            TitleApk.rewrite(t, out, spec(t), null, () -> true);
            throw new AssertionError("not cancelled");
        } catch (java.io.InterruptedIOException expected) {
            // the work directory is cleaned by TitleApp
        }
    }

    @Test(expected = IOException.class)
    public void aTemplateWithoutTheIconSlotIsRefused() throws Exception {
        File t = template();
        TitleApk.Spec s = spec(t);
        s.iconEntry = "res/drawable/none.png";
        TitleApk.rewrite(t, tmp.newFile("x.apk"), s, null, null);
    }

    private static List<String> names(File apk) throws IOException {
        List<String> l = new ArrayList<>();
        try (ZipFile z = new ZipFile(apk)) {
            for (ZipEntry e : java.util.Collections.list(z.entries()))
                l.add(e.getName());
        }
        return l;
    }

    // ---- Signing ---------------------------------------------------------------------------------------------

    @Test
    public void signedCopyVerifies() throws Exception {
        File t = template();
        File out = tmp.newFile("signed.apk");
        boolean[] signing = new boolean[1];
        TitleApk.build(t, tmp.newFile("u.apk"), out, spec(t), key.key, key.cert, null, () -> signing[0] = true, null);
        assertTrue(signing[0]);
        ApkVerifier.Result r = new ApkVerifier.Builder(out).setMinCheckedPlatformVersion(30).build().verify();
        assertTrue("verified: " + r.getErrors() + " " + r.getAllErrors(), r.isVerified());
        assertTrue(r.isVerifiedUsingV2Scheme());
        assertFalse(r.isVerifiedUsingV1Scheme());
        assertEquals(key.cert, r.getSignerCertificates().get(0));
        assertAligned(out, 4096);      // apksig realigns: 4, and at least a 4 KB page for a stored .so
        assertNull(entry(out, "META-INF/CERT.RSA"));
        assertEquals("fr.adserver.xrecomp.t4d5300d1", TitleApk.manifestPackage(entry(out, "AndroidManifest.xml")));

        // A regeneration with the same key has the same certificate (Android installs it over the first one);
        // another key does not.
        File again = tmp.newFile("again.apk"), other = tmp.newFile("other.apk");
        TitleApk.build(t, tmp.newFile("u2.apk"), again, spec(t), key.key, key.cert, null, null, null);
        TitleApk.build(t, tmp.newFile("u3.apk"), other, spec(t), otherKey.key, otherKey.cert, null, null, null);
        X509Certificate c1 = new ApkVerifier.Builder(again).build().verify().getSignerCertificates().get(0);
        X509Certificate c2 = new ApkVerifier.Builder(other).build().verify().getSignerCertificates().get(0);
        assertEquals(key.cert, c1);
        assertNotEquals(key.cert, c2);

        // For the host check with aapt2 and apksigner (docs « A game's own app », Tests).
        File keep = new File(new File(System.getProperty("xba.screenshots", "build/screenshots")).getAbsoluteFile()
                                 .getParentFile(), "title-app");
        if (keep.mkdirs() || keep.isDirectory())
            Files.copy(out.toPath(), new File(keep, "title-app.apk").toPath(),
                       java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    @Test
    public void localHeadersCarryTheSizes() throws Exception {
        File t = template();
        File out = tmp.newFile("unsigned2.apk");
        TitleApk.rewrite(t, out, spec(t), null, null);
        try (RandomAccessFile f = new RandomAccessFile(out, "r")) {
            byte[] h = new byte[30];
            for (ApkZip.Entry e : ApkZip.entries(f)) {
                f.seek(e.localOffset);
                f.readFully(h);
                ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
                assertEquals(e.name, 0, b.getShort(6) & 0x0008);
                assertEquals(e.name, e.crc, b.getInt(14) & 0xffffffffL);
                assertEquals(e.name, e.compressedSize, b.getInt(18) & 0xffffffffL);
            }
        }
    }
}
