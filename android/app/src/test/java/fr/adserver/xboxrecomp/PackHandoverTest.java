package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.app.Application;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.net.Uri;
import android.os.Bundle;

import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.shadow.api.Shadow;
import org.robolectric.shadows.ShadowSigningInfo;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;
import java.util.Random;

/**
 * The pack handover (XBA-176, PackHandover; docs/android/prepare.md « Handing a pack to a game's own app »): the
 * parent's caller check (package and title ID, the game apps' certificate), its answer and its provider, then the
 * clone's copy (lib_sha256, sizes, fit, freshness, free space) and its atomic publish. Synthetic packs only.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PackHandoverTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String TID = "4D530004";
    static final String CLONE = "fr.adserver.xboxrecomp.t4d530004";
    static final String PARENT = "fr.adserver.xboxrecomp.parent";

    private Application app;
    private TitleApp.Key key, otherKey;
    private File parentFiles, cloneFiles;

    @Before
    public void setUp() throws Exception {
        app = RuntimeEnvironment.getApplication();
        key = TitleApkTest.makeKey("xba-handover-1");
        otherKey = TitleApkTest.makeKey("xba-handover-2");
        TitleApp.testKey = key;
        TitleApp.resetMeta();
        parentFiles = tmp.newFolder("parent");
        cloneFiles = tmp.newFolder("clone");
        PackProvider.filesDirOverride = parentFiles;
    }

    @After
    public void tearDown() {
        TitleApp.testKey = null;
        TitleApp.resetMeta();
        PackProvider.filesDirOverride = null;
    }

    // ---- Helpers -------------------------------------------------------------------------------------------------

    /** Installs pkg signed by the given certificates (one signer each). */
    static void install(Application app, String pkg, java.security.cert.X509Certificate... certs) throws Exception {
        SigningInfo si = Shadow.newInstanceOf(SigningInfo.class);
        Signature[] sigs = new Signature[certs.length];
        for (int i = 0; i < certs.length; i++)
            sigs[i] = new Signature(certs[i].getEncoded());
        ((ShadowSigningInfo)Shadow.extract(si)).setSignatures(sigs);
        PackageInfo pi = new PackageInfo();
        pi.packageName = pkg;
        pi.signingInfo = si;
        pi.applicationInfo = new ApplicationInfo();
        pi.applicationInfo.packageName = pkg;
        shadowOf(app.getPackageManager()).installPackage(pi);
    }

    static String sha256(byte[] b) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(b))
            sb.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return sb.toString();
    }

    /** A whole pack (libtitle.so of libSize random bytes, its real sha256, an image) under filesDir/packs/TID. */
    static JSONObject writePack(File filesDir, String tid, int libSize, long seed, String toolkit, boolean image)
            throws Exception {
        File dir = Pack.packDir(filesDir, tid);
        byte[] lib = new byte[libSize];
        new Random(seed).nextBytes(lib);
        TestSupport.write(new File(dir, "libtitle.so"), lib);
        JSONObject o = new JSONObject();
        o.put("format", 1);
        o.put("title_id", tid);
        o.put("title", "Sample Game");
        o.put("xbe_sha256", "ab".repeat(32));
        o.put("abi", "arm64-v8a");
        o.put("shell_abi", Pack.SHELL_ABI);
        o.put("toolkit", "test");
        if (toolkit != null)
            o.put("toolkit_sha256", toolkit);
        o.put("lib", "libtitle.so");
        o.put("lib_sha256", sha256(lib));
        o.put("lib_size", libSize);
        o.put("entry", "recomp_title_main");
        o.put("cc", "clang");
        o.put("built_at", "2026-11-12T00:00:00Z");
        o.put("title_layer", "off");
        if (image) {
            TestSupport.write(new File(dir, "libimage-1.so"), new byte[] { 1, 2, 3, 4, 5 });
            o.put("images", new JSONObject().put("game.xbe", "libimage-1.so"));
        }
        TestSupport.write(new File(dir, "pack.json"), o.toString(1));
        return o;
    }

    private Intent request(String tid) {
        return new Intent(PackHandover.ACTION).putExtra(PackHandover.EXTRA_TITLE_ID, tid);
    }

    /** The parent's answer rewritten as if the parent were PARENT (its URIs on PARENT.packs). */
    private static Intent asParent(Intent data) {
        ClipData clip = data.getClipData();
        ClipData out = null;
        for (int i = 0; i < clip.getItemCount(); i++) {
            Uri u = clip.getItemAt(i).getUri();
            Uri v = u.buildUpon().authority(PARENT + PackProvider.AUTHORITY_SUFFIX).build();
            if (out == null)
                out = ClipData.newRawUri("pack", v);
            else
                out.addItem(new ClipData.Item(v));
        }
        Intent d = new Intent(data);
        d.setClipData(out);
        return d;
    }

    /** The offer of the parent's pack, read from parentFiles directly. */
    private PackHandover.Offer offer() {
        PackHandover.Answer a = PackHandover.serve(app, CLONE, request(TID));
        assertEquals(Activity.RESULT_OK, a.code);
        PackHandover.Offer o = PackHandover.offer(asParent(a.data), PARENT, TID);
        assertNotNull(o);
        return o;
    }

    private PackHandover.Source files() {
        return name -> {
            File f = new File(Pack.packDir(parentFiles, TID), name);
            if (!f.isFile())
                throw new FileNotFoundException(name);
            return new FileInputStream(f);
        };
    }

    private PackHandover.Result take(PackHandover.Offer o, String locked, String toolkit) {
        return PackHandover.take(files(), o, cloneFiles, locked, toolkit, null, null);
    }

    // ---- The parent: who gets a pack -----------------------------------------------------------------------------

    @Test
    public void theGameAppSignedByTheGameAppsKeyIsAccepted() throws Exception {
        install(app, CLONE, key.cert);
        assertNull(PackHandover.checkCaller(app, CLONE, TID));
        assertNull("any spelling of the title ID", PackHandover.checkCaller(app, CLONE, "0x4d530004"));
    }

    @Test
    public void anotherPackageOrTitleIsRefused() throws Exception {
        install(app, CLONE, key.cert);
        install(app, "com.example.other", key.cert);
        install(app, "fr.adserver.xboxrecomp.t4d530005", key.cert);
        assertEquals(PackHandover.Refusal.WRONG_PACKAGE, PackHandover.checkCaller(app, "com.example.other", TID));
        // A game app of another title asks for this one, or this one for another title.
        assertEquals(PackHandover.Refusal.WRONG_PACKAGE,
                     PackHandover.checkCaller(app, "fr.adserver.xboxrecomp.t4d530005", TID));
        assertEquals(PackHandover.Refusal.WRONG_PACKAGE, PackHandover.checkCaller(app, CLONE, "4D530005"));
        assertEquals(PackHandover.Refusal.BAD_TITLE, PackHandover.checkCaller(app, CLONE, "not-hex"));
        assertEquals(PackHandover.Refusal.BAD_TITLE, PackHandover.checkCaller(app, CLONE, null));
        // Not started for a result (adb shell am start): Android gives no calling package.
        assertEquals(PackHandover.Refusal.NO_CALLER, PackHandover.checkCaller(app, null, TID));
    }

    @Test
    public void anotherCertificateIsRefused() throws Exception {
        install(app, CLONE, otherKey.cert);
        assertEquals(PackHandover.Refusal.WRONG_CERT, PackHandover.checkCaller(app, CLONE, TID));
        // Two signers, ours among them: refused too.
        install(app, CLONE, key.cert, otherKey.cert);
        assertEquals(PackHandover.Refusal.WRONG_CERT, PackHandover.checkCaller(app, CLONE, TID));
        // Not installed (or not visible): refused.
        assertEquals(PackHandover.Refusal.WRONG_CERT,
                     PackHandover.checkCaller(app, "fr.adserver.xboxrecomp.t4d5300d1", "4D5300D1"));
    }

    @Test
    public void noKeyOrAGameAppItselfServesNothing() throws Exception {
        install(app, CLONE, key.cert);
        TitleApp.testKey = null;   // no game app was ever made: no key (Robolectric has no AndroidKeyStore either)
        assertEquals(PackHandover.Refusal.NO_KEY, PackHandover.checkCaller(app, CLONE, TID));
        TitleApp.testKey = key;
        PackageInfo pi = shadowOf(app.getPackageManager()).getInternalMutablePackageInfo(app.getPackageName());
        Bundle meta = new Bundle();
        meta.putString(TitleApk.META_TITLE_ID, "4D5300D1");
        pi.applicationInfo.metaData = meta;
        TitleApp.resetMeta();
        assertEquals(PackHandover.Refusal.NOT_A_PARENT, PackHandover.checkCaller(app, CLONE, TID));
    }

    @Test
    public void theAnswerGrantsEachFileOfTheValidPack() throws Exception {
        install(app, CLONE, key.cert);
        assertEquals(PackHandover.Refusal.NO_PACK, PackHandover.serve(app, CLONE, request(TID)).refusal);
        writePack(parentFiles, TID, 4096, 1, null, true);
        PackHandover.Answer a = PackHandover.serve(app, CLONE, request(TID));
        assertNull(a.refusal);
        assertEquals(Activity.RESULT_OK, a.code);
        assertTrue((a.data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0);
        assertArrayEquals(new String[] { "pack.json", "libtitle.so", "libimage-1.so" },
                          a.data.getStringArrayExtra(PackHandover.EXTRA_FILES));
        assertEquals(4096, a.data.getLongArrayExtra(PackHandover.EXTRA_SIZES)[1]);
        ClipData clip = a.data.getClipData();
        assertEquals(3, clip.getItemCount());
        assertEquals("content://" + app.getPackageName() + ".packs/4D530004/libtitle.so",
                     clip.getItemAt(1).getUri().toString());
        // A refusal carries its reason; the wrong caller gets nothing of the pack.
        PackHandover.Answer r = PackHandover.serve(app, "com.example.other", request(TID));
        assertEquals(Activity.RESULT_CANCELED, r.code);
        assertEquals("WRONG_PACKAGE", r.data.getStringExtra(PackHandover.EXTRA_REFUSAL));
        assertNull(r.data.getClipData());
        // A pack that is not whole (lib of the wrong size) is not served.
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "libtitle.so"), new byte[10]);
        assertEquals(PackHandover.Refusal.NO_PACK, PackHandover.serve(app, CLONE, request(TID)).refusal);
    }

    @Test
    public void theActivityAnswersTheCallerAndFinishes() throws Exception {
        install(app, CLONE, key.cert);
        writePack(parentFiles, TID, 512, 2, null, false);
        ActivityController<PackHandoverActivity> c =
            Robolectric.buildActivity(PackHandoverActivity.class, request(TID));
        shadowOf(c.get()).setCallingPackage(CLONE);
        PackHandoverActivity a = c.create().get();
        assertTrue(a.isFinishing());
        assertEquals(Activity.RESULT_OK, shadowOf(a).getResultCode());
        assertEquals(2, shadowOf(a).getResultIntent().getClipData().getItemCount());

        // Started without a result (adb shell am start): refused.
        PackHandoverActivity b = Robolectric.buildActivity(PackHandoverActivity.class, request(TID)).create().get();
        assertTrue(b.isFinishing());
        assertEquals(Activity.RESULT_CANCELED, shadowOf(b).getResultCode());
        assertEquals("NO_CALLER", shadowOf(b).getResultIntent().getStringExtra(PackHandover.EXTRA_REFUSAL));
    }

    @Test
    public void theProviderServesOnlyThePackFiles() throws Exception {
        writePack(parentFiles, TID, 256, 3, null, true);
        TestSupport.write(new File(parentFiles, "secret.txt"), "no");
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "other.so"), new byte[3]);
        Robolectric.setupContentProvider(PackProvider.class, PackProvider.authority(app));
        Uri lib = PackProvider.uriFor(app, TID, "libtitle.so");
        try (InputStream in = app.getContentResolver().openInputStream(lib)) {
            assertEquals(256, in.readAllBytes().length);
        }
        assertNotNull(PackProvider.servable(parentFiles, PackProvider.uriFor(app, TID, "libimage-1.so")));
        assertNotNull(PackProvider.servable(parentFiles, PackProvider.uriFor(app, TID, "pack.json")));
        // Not named by pack.json, outside the pack, another spelling of the title ID, a deeper path.
        assertNull(PackProvider.servable(parentFiles, PackProvider.uriFor(app, TID, "other.so")));
        assertNull(PackProvider.servable(parentFiles, PackProvider.uriFor(app, TID, "..")));
        assertNull(PackProvider.servable(parentFiles, Uri.parse("content://x.packs/4D530004/..%2F..%2Fsecret.txt")));
        assertNull(PackProvider.servable(parentFiles, PackProvider.uriFor(app, "4d530004", "libtitle.so")));
        assertNull(PackProvider.servable(parentFiles, Uri.parse("content://x.packs/4D530004/a/libtitle.so")));
        try {
            app.getContentResolver().openFileDescriptor(lib, "w");
            throw new AssertionError("writable");
        } catch (SecurityException expected) {
        }
    }

    // ---- The clone: the copy -------------------------------------------------------------------------------------

    @Test
    public void theOfferMustComeFromTheParentsProvider() throws Exception {
        install(app, CLONE, key.cert);
        writePack(parentFiles, TID, 128, 4, null, false);
        PackHandover.Answer a = PackHandover.serve(app, CLONE, request(TID));
        assertNull("URIs of another authority", PackHandover.offer(a.data, PARENT, TID));
        assertNull("another title", PackHandover.offer(asParent(a.data), PARENT, "4D530005"));
        assertNotNull(PackHandover.offer(asParent(a.data), PARENT, TID));
        Intent bad = asParent(a.data).putExtra(PackHandover.EXTRA_FILES, new String[] { "pack.json", "../x" });
        assertNull("a name that is not plain", PackHandover.offer(bad, PARENT, TID));
    }

    @Test
    public void copiesChecksAndPublishes() throws Exception {
        install(app, CLONE, key.cert);
        JSONObject meta = writePack(parentFiles, TID, 200_000, 5, null, true);
        long[] last = { 0, 0 };
        PackHandover.Result r = PackHandover.take(files(), offer(), cloneFiles, "", "",
                                                  (d, t) -> { last[0] = d; last[1] = t; }, null);
        assertEquals(r.detail, PackHandover.Outcome.COPIED, r.outcome);
        assertFalse(r.olderToolkit);
        assertEquals(Pack.Check.VALID, Pack.check(cloneFiles, TID));
        File dir = Pack.packDir(cloneFiles, TID);
        assertEquals(meta.getString("lib_sha256"),
                     sha256(Files.readAllBytes(new File(dir, "libtitle.so").toPath())));
        assertTrue(new File(dir, "libimage-1.so").isFile());
        assertEquals("progress up to the whole", last[1], last[0]);
        assertEquals(200_005, last[1]);
        assertEquals(Arrays.asList(TID), Arrays.asList(Pack.packsDir(cloneFiles).list()));   // no .partial, no .old
        assertFalse(new File(dir, "pack.json.tmp").exists());
    }

    @Test
    public void aStalePackIsReplacedAtomically() throws Exception {
        install(app, CLONE, key.cert);
        String oldToolkit = "11".repeat(32), newToolkit = "22".repeat(32);
        writePack(cloneFiles, TID, 1000, 6, oldToolkit, false);
        assertTrue(PackHandover.wanted(cloneFiles, TID, newToolkit));
        writePack(parentFiles, TID, 3000, 7, newToolkit, false);
        PackHandover.Result r = take(offer(), "", newToolkit);
        assertEquals(PackHandover.Outcome.COPIED, r.outcome);
        assertEquals(newToolkit, Pack.toolkitSha256(cloneFiles, TID));
        assertEquals(3000, new File(Pack.packDir(cloneFiles, TID), "libtitle.so").length());
        assertFalse(PackHandover.wanted(cloneFiles, TID, newToolkit));
        assertEquals(Arrays.asList(TID), Arrays.asList(Pack.packsDir(cloneFiles).list()));
    }

    @Test
    public void aParentPackNoNewerThanTheOwnIsNotTaken() throws Exception {
        install(app, CLONE, key.cert);
        String oldToolkit = "11".repeat(32), newToolkit = "22".repeat(32);
        writePack(cloneFiles, TID, 1000, 8, oldToolkit, false);
        writePack(parentFiles, TID, 3000, 9, oldToolkit, false);
        assertEquals(PackHandover.Outcome.NOT_NEWER, take(offer(), "", newToolkit).outcome);
        assertEquals(1000, new File(Pack.packDir(cloneFiles, TID), "libtitle.so").length());
        // With no pack of its own, an older toolkit's pack is taken: it plays (Prepare again offered).
        GameStore.deleteTree(Pack.packDir(cloneFiles, TID));
        PackHandover.Result r = take(offer(), "", newToolkit);
        assertEquals(PackHandover.Outcome.COPIED, r.outcome);
        assertTrue(r.olderToolkit);
    }

    @Test
    public void aDamagedLibraryIsNeverPublished() throws Exception {
        install(app, CLONE, key.cert);
        writePack(cloneFiles, TID, 1000, 10, null, false);   // the clone's own (it stays)
        JSONObject meta = writePack(parentFiles, TID, 2000, 11, null, false);
        PackHandover.Offer o = offer();
        // Same size, other bytes: the sha256 differs from pack.json's.
        byte[] other = new byte[2000];
        new Random(99).nextBytes(other);
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "libtitle.so"), other);
        PackHandover.Result r = take(o, "", "");
        // Its own pack is valid and the parent's no newer: kept, nothing read.
        assertEquals(PackHandover.Outcome.NOT_NEWER, r.outcome);
        GameStore.deleteTree(Pack.packDir(cloneFiles, TID));
        r = take(o, "", "");
        assertEquals(PackHandover.Outcome.DAMAGED, r.outcome);
        assertTrue(r.detail, r.detail.contains("sha256"));
        assertEquals(Pack.Check.NO_PACK, Pack.check(cloneFiles, TID));
        String[] left = Pack.packsDir(cloneFiles).list();
        assertTrue("no .partial left", left == null || left.length == 0);
        assertNotNull(meta);
    }

    @Test
    public void anUnfitPackIsRefused() throws Exception {
        install(app, CLONE, key.cert);
        JSONObject meta = writePack(parentFiles, TID, 100, 12, null, false);
        PackHandover.Offer o = offer();
        meta.put("shell_abi", Pack.SHELL_ABI + 1);
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "pack.json"), meta.toString());
        PackHandover.Result r = take(o, "", "");
        assertEquals(PackHandover.Outcome.UNFIT, r.outcome);
        assertTrue(r.detail, r.detail.startsWith("shell_abi"));
        meta.put("shell_abi", Pack.SHELL_ABI).put("title_id", "4D530005");
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "pack.json"), meta.toString());
        assertEquals(PackHandover.Outcome.UNFIT, take(o, "", "").outcome);
        meta.put("title_id", TID).put("format", 2);
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "pack.json"), meta.toString());
        assertEquals(PackHandover.Outcome.UNFIT, take(o, "", "").outcome);
        assertEquals(Pack.Check.NO_PACK, Pack.check(cloneFiles, TID));
    }

    @Test
    public void anotherDiscOrNoSpaceFallsBack() throws Exception {
        install(app, CLONE, key.cert);
        writePack(parentFiles, TID, 100, 13, null, true);
        PackHandover.Offer o = offer();
        assertEquals(PackHandover.Outcome.OTHER_XBE, take(o, "cd".repeat(32), "").outcome);
        assertEquals(PackHandover.Outcome.COPIED, take(o, "AB".repeat(32), "").outcome);   // the locked disc
        GameStore.deleteTree(Pack.packDir(cloneFiles, TID));
        // A pack larger than the free space.
        o.sizes.put("libimage-1.so", Long.MAX_VALUE / 4);
        PackHandover.Result r = take(o, "", "");
        assertEquals(PackHandover.Outcome.NO_SPACE, r.outcome);
        assertTrue(Long.parseLong(r.detail) > 0);
        assertEquals(Pack.Check.NO_PACK, Pack.check(cloneFiles, TID));
    }

    @Test
    public void recoverUndoesACutPublish() throws Exception {
        writePack(cloneFiles, TID, 100, 14, null, false);
        File packs = Pack.packsDir(cloneFiles);
        assertTrue(new File(packs, TID).renameTo(new File(packs, TID + ".old")));
        PackHandover.recover(cloneFiles, TID);
        assertEquals(Pack.Check.VALID, Pack.check(cloneFiles, TID));
        assertFalse(new File(packs, TID + ".old").exists());
    }
}
