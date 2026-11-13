package fr.adserver.xboxrecomp;

import com.android.apksig.ApkSigner;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.BooleanSupplier;

/**
 * A game's own app, made on the device from this app's installed APK (docs/android/app-shell.md « A game's own
 * app »): no build tools, only a copy with three changes, then a signature.
 *
 * 1. AndroidManifest.xml (binary XML, BinaryXml): package fr.adserver.xboxrecomp.t<titleid>, the game's name as the
 *    label (a literal string), the icon slot mipmap/xba_title_icon as icon and roundIcon, every provider authority and
 *    package-derived name (permissions, task affinity, process) moved to the new package, the component class names
 *    written in full (they stay fr.adserver.xboxrecomp.*), and the meta-data that put the copy in one-game mode
 *    (META_TITLE_ID, META_XBE_SHA256, META_PARENT).
 * 2. The icon slot's PNG (res/drawable-nodpi/xba_title_icon_art.png in the template) replaced by the game's icon:
 *    resources.arsc only names the file, so nothing else changes.
 * 3. The old signature files (META-INF/*.SF, .RSA, .DSA, .EC, MANIFEST.MF) and the source stamp dropped.
 *
 * ApkZip copies every other entry raw (no recompression); apksig then signs with APK Signature Scheme v2 (minSdk 30:
 * no v1 needed; v3 brings key rotation, which a key that never leaves the device does not need) and realigns.
 */
final class TitleApk {
    private TitleApk() {}

    /** The generated app's package: the prefix and the title ID in lower case (a package segment starts with a letter). */
    static final String PACKAGE_PREFIX = "fr.adserver.xboxrecomp.t";
    /** The manifest meta-data of a generated app (application level). */
    static final String META_TITLE_ID = "fr.adserver.xboxrecomp.TITLE_ID",
                        META_XBE_SHA256 = "fr.adserver.xboxrecomp.XBE_SHA256",
                        META_PARENT = "fr.adserver.xboxrecomp.PARENT";
    /** The icon slot's layer in the template (res/drawable-nodpi), the PNG the copy replaces. */
    static final String ICON_LAYER = "xba_title_icon_art";
    /** The longest label written (launchers cut far earlier; the install dialog shows it whole). */
    static final int LABEL_MAX = 50;

    // android.R.attr ids (public API, fixed).
    static final int ATTR_LABEL = 0x01010001, ATTR_ICON = 0x01010002, ATTR_NAME = 0x01010003,
                     ATTR_PERMISSION = 0x01010006, ATTR_READ_PERMISSION = 0x01010007,
                     ATTR_WRITE_PERMISSION = 0x01010008, ATTR_PROCESS = 0x01010011, ATTR_TASK_AFFINITY = 0x01010012,
                     ATTR_AUTHORITIES = 0x01010018, ATTR_VALUE = 0x01010024, ATTR_TARGET_ACTIVITY = 0x01010202,
                     ATTR_ROUND_ICON = 0x0101052c;

    private static final List<String> COMPONENTS = java.util.Arrays.asList(
        "application", "activity", "activity-alias", "service", "receiver", "provider", "instrumentation");
    private static final List<String> PERMISSION_TAGS = java.util.Arrays.asList(
        "permission", "permission-group", "permission-tree", "uses-permission", "uses-permission-sdk-23");

    /** What the copy is: the game's title ID, label, the XBE it is locked to, the icon PNG and where it goes. */
    static final class Spec {
        String titleId, label, xbeSha256 = "", parent = "";
        byte[] iconPng;
        /** The zip entry of the icon slot's layer (Resources.getValue(...).string of the template). */
        String iconEntry;
        /** R.mipmap.xba_title_icon: the same id in the template, which is this very APK. */
        int iconRes;

        String packageName() {
            return TitleApk.packageName(titleId);
        }
    }

    static String packageName(String titleId) {
        return PACKAGE_PREFIX + titleId.toLowerCase(Locale.ROOT);
    }

    static String label(String title) {
        String t = title == null ? "" : title.trim().replaceAll("\\s+", " ");
        return t.codePointCount(0, t.length()) <= LABEL_MAX ? t : t.substring(0, t.offsetByCodePoints(0, LABEL_MAX));
    }

    /** Signature files and the source stamp of the template, left out of the copy. */
    static boolean oldSignature(String name) {
        String n = name.toUpperCase(Locale.ROOT);
        if (n.equals("STAMP-CERT-SHA256"))
            return true;
        if (!n.startsWith("META-INF/") || n.indexOf('/', 9) >= 0)
            return false;
        return n.equals("META-INF/MANIFEST.MF") || n.endsWith(".SF") || n.endsWith(".RSA") || n.endsWith(".DSA")
               || n.endsWith(".EC");
    }

    /** The package of a binary manifest. */
    static String manifestPackage(byte[] axml) throws IOException {
        BinaryXml x = BinaryXml.parse(axml);
        int root = x.root();
        return root < 0 ? null : x.value(x.plainAttr(root, "package"));
    }

    /**
     * The template's binary manifest made into the copy's (see the class comment). Patching a manifest that was
     * patched already gives the same result (the meta-data are replaced, not added twice).
     */
    static byte[] patchManifest(byte[] axml, Spec s) throws IOException {
        BinaryXml x = BinaryXml.parse(axml);
        int root = x.root();
        if (root < 0 || !"manifest".equals(x.name(root)))
            throw new IOException("AndroidManifest.xml: no <manifest>");
        BinaryXml.Attr pkgAttr = x.plainAttr(root, "package");
        String old = x.value(pkgAttr);
        if (old == null || old.isEmpty())
            throw new IOException("AndroidManifest.xml: no package");
        String pkg = s.packageName();

        for (int el : x.descendants(root)) {
            String tag = x.name(el);
            if (COMPONENTS.contains(tag)) {
                qualify(x, x.attr(el, ATTR_NAME), old);
                qualify(x, x.attr(el, ATTR_TARGET_ACTIVITY), old);
            }
            if (PERMISSION_TAGS.contains(tag))
                derive(x, x.attr(el, ATTR_NAME), old, pkg);
            for (int id : new int[] { ATTR_PERMISSION, ATTR_READ_PERMISSION, ATTR_WRITE_PERMISSION,
                                      ATTR_TASK_AFFINITY })
                derive(x, x.attr(el, id), old, pkg);
            BinaryXml.Attr proc = x.attr(el, ATTR_PROCESS);
            String p = x.value(proc);
            if (p != null && (p.equals(old) || p.startsWith(old + ":")))
                x.setString(proc, pkg + p.substring(old.length()));
            BinaryXml.Attr auth = x.attr(el, ATTR_AUTHORITIES);
            String a = x.value(auth);
            if (a != null) {
                StringBuilder sb = new StringBuilder();
                for (String one : a.split(";")) {
                    if (sb.length() > 0)
                        sb.append(';');
                    sb.append(derived(one, old, pkg));
                }
                x.setString(auth, sb.toString());
            }
        }
        x.setString(pkgAttr, pkg);

        List<Integer> apps = x.children(root, "application");
        if (apps.isEmpty())
            throw new IOException("AndroidManifest.xml: no <application>");
        int app = apps.get(0);
        x.setString(x.androidAttr(app, "label", ATTR_LABEL), label(s.label));
        if (s.iconRes != 0) {
            for (int id : new int[] { ATTR_ICON, ATTR_ROUND_ICON }) {
                BinaryXml.Attr icon = x.androidAttr(app, id == ATTR_ICON ? "icon" : "roundIcon", id);
                icon.raw = -1;
                icon.type = BinaryXml.TYPE_REFERENCE;
                icon.data = s.iconRes;
            }
        }
        // The one-game meta-data: replaced when there (a copy patched again), then added.
        // The parent: the spec's, else the one a copy patched again names already, else the template itself.
        String parent = s.parent == null || s.parent.isEmpty() ? old : s.parent;
        app = x.children(root, "application").get(0);
        for (int i = x.children(app, "meta-data").size() - 1; i >= 0; i--) {
            int md = x.children(app, "meta-data").get(i);
            String name = x.value(x.attr(md, ATTR_NAME));
            if (META_PARENT.equals(name) && (s.parent == null || s.parent.isEmpty())
                && x.value(x.attr(md, ATTR_VALUE)) != null)
                parent = x.value(x.attr(md, ATTR_VALUE));
            if (META_TITLE_ID.equals(name) || META_XBE_SHA256.equals(name) || META_PARENT.equals(name))
                x.remove(md);
        }
        addMeta(x, root, META_TITLE_ID, s.titleId.toUpperCase(Locale.ROOT));
        addMeta(x, root, META_XBE_SHA256, s.xbeSha256 == null ? "" : s.xbeSha256);
        addMeta(x, root, META_PARENT, parent);
        return x.toBytes();
    }

    private static void addMeta(BinaryXml x, int root, String name, String value) {
        int app = x.children(root, "application").get(0);
        int md = x.addChild(app, "meta-data");
        x.setString(x.androidAttr(md, "name", ATTR_NAME), name);
        x.setString(x.androidAttr(md, "value", ATTR_VALUE), value);
    }

    /** A relative class name (« .LauncherActivity », « LauncherActivity ») written in full against the old package. */
    private static void qualify(BinaryXml x, BinaryXml.Attr a, String old) {
        String v = x.value(a);
        if (v == null || v.isEmpty())
            return;
        if (v.startsWith("."))
            x.setString(a, old + v);
        else if (v.indexOf('.') < 0)
            x.setString(a, old + "." + v);
    }

    private static void derive(BinaryXml x, BinaryXml.Attr a, String old, String pkg) {
        String v = x.value(a);
        if (v != null && !v.equals(derived(v, old, pkg)))
            x.setString(a, derived(v, old, pkg));
    }

    /** old itself or old.<anything> moved to pkg; anything else unchanged. */
    static String derived(String v, String old, String pkg) {
        if (v.equals(old))
            return pkg;
        if (v.startsWith(old + "."))
            return pkg + v.substring(old.length());
        return v;
    }

    /** The template's manifest. */
    static byte[] readManifest(File apk) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(apk, "r")) {
            for (ApkZip.Entry e : ApkZip.entries(f)) {
                if (e.name.equals("AndroidManifest.xml"))
                    return ApkZip.read(f, e);
            }
        }
        throw new IOException(apk + ": no AndroidManifest.xml");
    }

    /**
     * Writes the unsigned copy of template to unsigned: the manifest patched, the icon slot's layer replaced, the old
     * signature left out.
     */
    static void rewrite(File template, File unsigned, Spec s, ApkZip.Progress progress, BooleanSupplier cancelled)
            throws IOException {
        if (s.iconEntry == null || s.iconPng == null)
            throw new IOException("no icon");
        boolean hasIcon = false;
        try (RandomAccessFile f = new RandomAccessFile(template, "r")) {
            for (ApkZip.Entry e : ApkZip.entries(f))
                hasIcon |= e.name.equals(s.iconEntry);
        }
        if (!hasIcon)
            throw new IOException("the template has no " + s.iconEntry);
        Map<String, byte[]> replace = new HashMap<>();
        replace.put("AndroidManifest.xml", patchManifest(readManifest(template), s));
        replace.put(s.iconEntry, s.iconPng);
        ApkZip.rewrite(template, unsigned, replace, TitleApk::oldSignature, progress, cancelled);
    }

    /** Signs unsigned into out: APK Signature Scheme v2 only, stored entries aligned (4, 16 KB for a .so). */
    static void sign(File unsigned, File out, PrivateKey key, X509Certificate cert) throws IOException {
        try {
            ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder(
                "XRECOMP", key, Collections.singletonList(cert)).build();
            new ApkSigner.Builder(Collections.singletonList(signer))
                .setInputApk(unsigned)
                .setOutputApk(out)
                .setMinSdkVersion(30)
                .setV1SigningEnabled(false)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(false)
                .setV4SigningEnabled(false)
                .setDebuggableApkPermitted(true)
                .setAlignmentPreserved(false)
                .setLibraryPageAlignmentBytes(ApkZip.SO_ALIGN)
                .setCreatedBy("XRecomp")
                .build()
                .sign();
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {     // ApkFormatException, NoSuchAlgorithmException, InvalidKeyException, SignatureException
            throw new IOException("signing failed: " + e.getMessage(), e);
        }
    }

    /** Both steps; unsigned is a scratch file, deleted afterwards. */
    static void build(File template, File unsigned, File out, Spec s, PrivateKey key, X509Certificate cert,
                      ApkZip.Progress progress, Runnable signing, BooleanSupplier cancelled) throws IOException {
        try {
            rewrite(template, unsigned, s, progress, cancelled);
            if (cancelled != null && cancelled.getAsBoolean())
                throw new java.io.InterruptedIOException("cancelled");
            if (signing != null)
                signing.run();
            sign(unsigned, out, key, cert);
        } finally {
            //noinspection ResultOfMethodCallIgnored
            unsigned.delete();
        }
    }
}
