package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.Looper;
import android.view.View;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowActivity;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;

/**
 * A game's own app at its start (XBA-176, docs/android/prepare.md « Handing a pack to a game's own app »): it asks
 * XRecomp for its pack, Home says « Copying the prepared game from XRecomp… » instead of « Needs preparing », then
 * Play; XRecomp missing, without a pack or refusing: the normal Prepare, with a note saying why. XRecomp itself asks
 * nobody. Synthetic XBE and packs only.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherPackHandoverTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = PackHandoverTest.TID;
    private static final String PARENT = PackHandoverTest.PARENT;
    private Application app;
    private File parentFiles;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        File game = GameStore.defaultGameDir(app);
        TestSupport.write(new File(game, "default.xbe"), TestSupport.xbe(0x4D530004, "Sample Game"));
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        parentFiles = tmp.newFolder("parent");
        PackProvider.filesDirOverride = parentFiles;
        LauncherActivity.handoverAsked = false;
        TitleApp.resetMeta();
    }

    @After
    public void tearDown() {
        Toolchain.assetsOverride = null;
        Toolchain.nativeDirOverride = null;
        PackProvider.filesDirOverride = null;
        LauncherActivity.handoverAsked = false;
        TitleApp.resetMeta();
        PackFreshness.forget();
    }

    /** This app becomes the game's own app of TID, made from PARENT. */
    private void asGameApp() {
        PackageInfo pi = shadowOf(app.getPackageManager()).getInternalMutablePackageInfo(app.getPackageName());
        Bundle meta = new Bundle();
        meta.putString(TitleApk.META_TITLE_ID, TID);
        meta.putString(TitleApk.META_PARENT, PARENT);
        pi.applicationInfo.metaData = meta;
        TitleApp.resetMeta();
    }

    private void parentInstalled() {
        PackageInfo pi = new PackageInfo();
        pi.packageName = PARENT;
        pi.lastUpdateTime = 1000;
        pi.applicationInfo = new ApplicationInfo();
        pi.applicationInfo.packageName = PARENT;
        shadowOf(app.getPackageManager()).installPackage(pi);
    }

    private LauncherActivity launch() {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        return a;
    }

    /** What the parent answers with its pack (as PackHandover.serve would, its URIs on PARENT.packs). */
    private Intent parentAnswer() throws Exception {
        org.json.JSONObject m = PackHandoverTest.writePack(parentFiles, TID, 50_000, 21, null, false);
        // Built from this very default.xbe (Home compares them once it has hashed it).
        m.put("xbe_sha256", PackHandoverTest.sha256(TestSupport.xbe(0x4D530004, "Sample Game")));
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "pack.json"), m.toString());
        Robolectric.setupContentProvider(PackProvider.class, PARENT + PackProvider.AUTHORITY_SUFFIX);
        String[] names = { "pack.json", "libtitle.so" };
        ClipData clip = null;
        long[] sizes = new long[names.length];
        for (int i = 0; i < names.length; i++) {
            Uri u = Uri.parse("content://" + PARENT + ".packs/" + TID + "/" + names[i]);
            sizes[i] = new File(Pack.packDir(parentFiles, TID), names[i]).length();
            if (clip == null)
                clip = ClipData.newRawUri("pack", u);
            else
                clip.addItem(new ClipData.Item(u));
        }
        Intent d = new Intent().putExtra(PackHandover.EXTRA_TITLE_ID, TID)
                .putExtra(PackHandover.EXTRA_FILES, names).putExtra(PackHandover.EXTRA_SIZES, sizes);
        d.setClipData(clip);
        d.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return d;
    }

    private ShadowActivity.IntentForResult asked(LauncherActivity a) {
        ShadowActivity.IntentForResult r = shadowOf(a).getNextStartedActivityForResult();
        assertNotNull("the parent was asked", r);
        assertEquals(LauncherActivity.REQ_PACK_HANDOVER, r.requestCode);
        assertEquals(PARENT, r.intent.getComponent().getPackageName());
        assertEquals(PackHandover.ACTIVITY, r.intent.getComponent().getClassName());
        assertEquals(TID, r.intent.getStringExtra(PackHandover.EXTRA_TITLE_ID));
        return r;
    }

    private File shot(LauncherActivity a, String name) throws Exception {
        View root = a.getWindow().getDecorView();
        shadowOf(Looper.getMainLooper()).idle();
        return TestSupport.screenshot(root, "handover-" + name);
    }

    @Test
    public void firstStartCopiesTheParentsPackThenPlays() throws Exception {
        asGameApp();
        parentInstalled();
        LauncherActivity a = launch();
        ShadowActivity.IntentForResult r = asked(a);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());

        shadowOf(a).receiveResult(r.intent, Activity.RESULT_OK, parentAnswer());
        // Copying: Home says so instead of « Needs preparing », its button dimmed.
        assertEquals(app.getString(R.string.HANDOVER_COPYING_BUTTON), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.HANDOVER_COPYING_NOTE), a.noteForTest());
        shot(a, "1-copying");
        // Play while copying waits.
        a.homePrimaryForTest().performClick();
        assertEquals(app.getString(R.string.HANDOVER_BUSY), a.noteForTest());

        Thread w = a.handoverWorkerForTest();
        assertNotNull(w);
        w.join(20_000);
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertNull("Home offers Play", a.homePrepForTest());
        assertEquals(app.getString(R.string.PLAY), a.homePrimaryForTest().title);
        assertEquals(app.getString(R.string.HANDOVER_DONE), a.noteForTest());
        // The parent's Enhancements choice came with its pack (title_layer off).
        assertTrue(!GameStore.enhancements(app, TID));
        shot(a, "2-copied");

        // A later start has a current pack: the parent is not asked again.
        LauncherActivity.handoverAsked = false;
        LauncherActivity b = launch();
        assertNull(shadowOf(b).getNextStartedActivityForResult());
    }

    @Test
    public void aParentWithoutThePackFallsBackToPrepare() throws Exception {
        asGameApp();
        parentInstalled();
        LauncherActivity a = launch();
        ShadowActivity.IntentForResult r = asked(a);
        shadowOf(a).receiveResult(r.intent, Activity.RESULT_CANCELED,
                                  new Intent().putExtra(PackHandover.EXTRA_REFUSAL, "NO_PACK"));
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertEquals(app.getString(R.string.HANDOVER_NO_PACK), a.noteForTest());
        shot(a, "3-no-pack");

        // Any other refusal says which.
        LauncherActivity.handoverAsked = false;
        LauncherActivity b = launch();
        shadowOf(b).receiveResult(asked(b).intent, Activity.RESULT_CANCELED,
                                  new Intent().putExtra(PackHandover.EXTRA_REFUSAL, "WRONG_CERT"));
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(LauncherActivity.Prep.NEEDED, b.homePrepForTest());
        assertEquals(app.getString(R.string.HANDOVER_REFUSED, "WRONG_CERT"), b.noteForTest());
    }

    @Test
    public void aDamagedCopyFallsBackToPrepare() throws Exception {
        asGameApp();
        parentInstalled();
        LauncherActivity a = launch();
        ShadowActivity.IntentForResult r = asked(a);
        Intent answer = parentAnswer();
        // The library changes under the parent's pack.json (same size, other bytes): its sha256 no longer matches.
        TestSupport.write(new File(Pack.packDir(parentFiles, TID), "libtitle.so"), new byte[50_000]);
        shadowOf(a).receiveResult(r.intent, Activity.RESULT_OK, answer);
        a.handoverWorkerForTest().join(20_000);
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(Pack.Check.NO_PACK, Pack.check(app.getFilesDir(), TID));
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertTrue(a.noteForTest(), a.noteForTest().startsWith(app.getString(R.string.HANDOVER_FAILED, "")
                                                                  .substring(0, 20)));
    }

    @Test
    public void withoutXRecompTheAppPreparesItself() throws Exception {
        asGameApp();
        LauncherActivity a = launch();
        assertNull(shadowOf(a).getNextStartedActivityForResult());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
        assertEquals(app.getString(R.string.HANDOVER_NO_PARENT), a.noteForTest());
    }

    @Test
    public void xrecompItselfAsksNobody() throws Exception {
        parentInstalled();
        LauncherActivity a = launch();
        assertNull(shadowOf(a).getNextStartedActivityForResult());
        assertEquals(LauncherActivity.Prep.NEEDED, a.homePrepForTest());
    }
}
