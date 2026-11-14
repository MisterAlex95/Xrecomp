package fr.adserver.xrecomp;

import static org.junit.Assert.assertFalse;
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
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Looper;
import android.provider.Settings;

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
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * A game's own app goes straight into the game (docs/android/app-shell.md « Straight into the game »): a ready game
 * starts with no screen; the game's files where XRecomp reads them (handed over with the pack, All files access asked
 * once) are used in place and checked against the disc the app is locked to; XRecomp's private copy, a refused access
 * or another disc lead to the game choice; the game ending by itself says why with Play; the « Launcher » shortcut
 * keeps Home; « Change game » is not offered. XRecomp itself is unchanged. Synthetic XBEs and packs only.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherGameAppTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = PackHandoverTest.TID;
    private static final String PARENT = PackHandoverTest.PARENT;
    private static final byte[] XBE = TestSupport.xbe(0x4D530004, "Sample Game");
    private Application app;
    private File parentFiles, folder;

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        Toolchain.assetsOverride = TestSupport.toolAssets(tmp.newFolder("assets"), "1");
        Toolchain.nativeDirOverride = TestSupport.fakeNativeDir(tmp.getRoot()).getAbsolutePath();
        parentFiles = tmp.newFolder("parent");
        PackProvider.filesDirOverride = parentFiles;
        // Where XRecomp reads the game from: a folder on shared storage (here a temporary one).
        folder = new File(tmp.newFolder("storage"), "Sample Game");
        TestSupport.write(new File(folder, "default.xbe"), XBE);
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

    // ---- Helpers -------------------------------------------------------------------------------------------------

    /** This app becomes the game's own app of TID, made from PARENT and locked to XBE. */
    private void asGameApp() throws Exception {
        PackageInfo pi = shadowOf(app.getPackageManager()).getInternalMutablePackageInfo(app.getPackageName());
        Bundle meta = new Bundle();
        meta.putString(TitleApk.META_TITLE_ID, TID);
        meta.putString(TitleApk.META_PARENT, PARENT);
        meta.putString(TitleApk.META_XBE_SHA256, PackHandoverTest.sha256(XBE));
        pi.applicationInfo.metaData = meta;
        TitleApp.resetMeta();
        PackageInfo p = new PackageInfo();
        p.packageName = PARENT;
        p.lastUpdateTime = 1000;
        p.applicationInfo = new ApplicationInfo();
        p.applicationInfo.packageName = PARENT;
        shadowOf(app.getPackageManager()).installPackage(p);
    }

    /** The game's files in this app's own storage and a valid pack from them: everything ready. */
    private void readyHere() throws Exception {
        TestSupport.write(new File(GameStore.defaultGameDir(app), "default.xbe"), XBE);
        TestSupport.writePack(app.getFilesDir(), TID, 1000);
    }

    /**
     * All files access on or off: Environment.isExternalStorageManager reads the app op first (Robolectric allows
     * an op nobody set), then the permission. AppOpsManager.setMode(String, …) is a system API: by reflection,
     * Robolectric's shadow serves it.
     */
    private void allFilesAccess(boolean on) throws Exception {
        if (on)
            shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        else
            shadowOf(app).denyPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        android.app.AppOpsManager ops = app.getSystemService(android.app.AppOpsManager.class);
        ops.getClass().getMethod("setMode", String.class, int.class, String.class, int.class)
                .invoke(ops, "android:manage_external_storage", android.os.Process.myUid(),
                        app.getPackageName(), on ? android.app.AppOpsManager.MODE_ALLOWED
                                                 : android.app.AppOpsManager.MODE_ERRORED);
    }

    private LauncherActivity launch(Intent in) {
        LauncherActivity a = (in == null ? Robolectric.buildActivity(LauncherActivity.class)
                                         : Robolectric.buildActivity(LauncherActivity.class, in)).setup().get();
        shadowOf(Looper.getMainLooper()).idle();
        return a;
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    /** The parent's answer: its pack (withPack) or NO_PACK, and where it reads the game from (kind, path). */
    private Intent answer(boolean withPack, PackHandover.GameKind kind, String path) throws Exception {
        Intent d;
        if (withPack) {
            org.json.JSONObject m = PackHandoverTest.writePack(parentFiles, TID, 40_000, 7, null, false);
            m.put("xbe_sha256", PackHandoverTest.sha256(XBE));
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
            d = new Intent().putExtra(PackHandover.EXTRA_TITLE_ID, TID).putExtra(PackHandover.EXTRA_FILES, names)
                    .putExtra(PackHandover.EXTRA_SIZES, sizes);
            d.setClipData(clip);
        } else {
            d = new Intent().putExtra(PackHandover.EXTRA_REFUSAL, "NO_PACK");
        }
        PackHandover.putSource(d, kind == null ? null : new PackHandover.GameSource(kind, path));
        return d;
    }

    private void reply(LauncherActivity a, boolean withPack, PackHandover.GameKind kind, String path)
            throws Exception {
        ShadowActivity.IntentForResult r = shadowOf(a).getNextStartedActivityForResult();
        assertNotNull("XRecomp was asked", r);
        assertEquals(LauncherActivity.REQ_PACK_HANDOVER, r.requestCode);
        shadowOf(a).receiveResult(r.intent, withPack ? Activity.RESULT_OK : Activity.RESULT_CANCELED,
                                  answer(withPack, kind, path));
        idle(50);
    }

    /** Waits for the files check and the pack copy, then lets their outcomes run. */
    private void settle(LauncherActivity a) throws Exception {
        for (int k = 0; k < 3; k++) {
            Thread w = a.workerForTest(), h = a.handoverWorkerForTest();
            if (w != null)
                w.join(20_000);
            if (h != null)
                h.join(20_000);
            idle(100);
        }
    }

    private void prepareDone() throws Exception {
        TestSupport.writePack(app.getFilesDir(), TID, 1000);
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.DONE;
        s.titleId = TID;
        s.startedAt = s.updatedAt = System.currentTimeMillis();
        PrepareProtocol.writeStatus(PrepareService.statusFile(app, TID), s);
        idle(1200);
    }

    private List<String> shortcutIds() {
        List<String> ids = new ArrayList<>();
        for (ShortcutInfo s : app.getSystemService(ShortcutManager.class).getDynamicShortcuts())
            ids.add(s.getId());
        return ids;
    }

    private void shot(LauncherActivity a, String name) throws Exception {
        idle(50);
        TestSupport.screenshot(a.getWindow().getDecorView(), "game-app-" + name);
    }

    // ---- Tests ---------------------------------------------------------------------------------------------------

    @Test
    public void aReadyGameStartsWithNoScreen() throws Exception {
        asGameApp();
        readyHere();
        LauncherActivity a = launch(null);
        assertNull("nothing was shown", a.screenForTest());
        LauncherPackHandoverTest.assertGameStarted(a);
        assertTrue("XRecomp was not asked", !LauncherPackHandoverTest.askedParent(a));
        // Locked to its disc: no « Change game » on a long press, « Launcher » stays.
        assertEquals(java.util.Set.of("launcher"), new java.util.HashSet<>(shortcutIds()));
    }

    @Test
    public void theFolderXRecompUsesIsReadInPlace() throws Exception {
        asGameApp();
        allFilesAccess(true);
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Direct.WAIT, a.directStateForTest());
        shot(a, "1-wait");
        reply(a, true, PackHandover.GameKind.FOLDER, folder.getAbsolutePath());
        settle(a);
        // The folder became this app's game, read where it is; XRecomp's pack was copied; the game started.
        Library.Record r = Library.read(Library.gamesFile(app.getFilesDir())).get(TID);
        assertNotNull(r);
        assertEquals(folder.getAbsolutePath(), r.dataDir);
        assertEquals(Library.KIND_IN_PLACE, r.dataKind);
        assertEquals(Pack.Check.VALID, Pack.check(app.getFilesDir(), TID));
        assertEquals(folder.getAbsolutePath(), GameStore.chosenDir(app));
        LauncherPackHandoverTest.assertGameStarted(a);
    }

    @Test
    public void allFilesAccessIsAskedOnceForThatFolder() throws Exception {
        asGameApp();
        allFilesAccess(false);
        LauncherActivity a = launch(null);
        reply(a, false, PackHandover.GameKind.FOLDER, folder.getAbsolutePath());
        assertEquals(LauncherActivity.Direct.ASK_ACCESS, a.directStateForTest());
        assertEquals(app.getString(R.string.DIRECT_ACCESS, folder.getAbsolutePath()), a.directDetailForTest());
        assertEquals(app.getString(R.string.ALL_FILES_GRANT), a.directPrimaryForTest().title);
        shot(a, "2-ask-access");
        a.directPrimaryForTest().performClick();
        ShadowActivity.IntentForResult settings = shadowOf(a).getNextStartedActivityForResult();
        assertNotNull(settings);
        assertEquals(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, settings.intent.getAction());

        allFilesAccess(true);   // the player turned it on
        shadowOf(a).receiveResult(settings.intent, Activity.RESULT_OK, null);
        settle(a);
        // The files are this app's game now; XRecomp had no pack: the Prepare runs, saying why, then the game.
        assertEquals(folder.getAbsolutePath(), Library.read(Library.gamesFile(app.getFilesDir())).get(TID).dataDir);
        assertEquals(LauncherActivity.Direct.PREPARING, a.directStateForTest());
        assertTrue(a.directDetailForTest(), a.directDetailForTest().startsWith(app.getString(R.string.HANDOVER_NO_PACK)));
        shot(a, "3-preparing");
        prepareDone();
        LauncherPackHandoverTest.assertGameStarted(a);
    }

    @Test
    public void aRefusedAccessLeadsToTheGameChoice() throws Exception {
        asGameApp();
        allFilesAccess(false);
        LauncherActivity a = launch(null);
        reply(a, false, PackHandover.GameKind.FOLDER, folder.getAbsolutePath());
        a.directPrimaryForTest().performClick();
        ShadowActivity.IntentForResult settings = shadowOf(a).getNextStartedActivityForResult();
        shadowOf(a).receiveResult(settings.intent, Activity.RESULT_CANCELED, null);
        idle(50);
        assertEquals(LauncherActivity.Direct.CHOOSE, a.directStateForTest());
        assertEquals(app.getString(R.string.DIRECT_ACCESS_REFUSED), a.directDetailForTest());
    }

    @Test
    public void xrecompsOwnCopyMeansChoosingTheGameOnce() throws Exception {
        asGameApp();
        allFilesAccess(true);
        LauncherActivity a = launch(null);
        reply(a, true, PackHandover.GameKind.NOT_SHARED, null);
        settle(a);
        assertEquals(LauncherActivity.Direct.CHOOSE, a.directStateForTest());
        assertEquals(app.getString(R.string.DIRECT_NOT_SHARED), a.directDetailForTest());
        assertEquals(app.getString(R.string.CHOOSE_GAME), a.directPrimaryForTest().title);
        shot(a, "4-choose");
        a.directPrimaryForTest().performClick();
        idle(50);
        assertEquals(LauncherActivity.Screen.SOURCE, a.screenForTest());
        LauncherPackHandoverTest.assertGameNotStarted(a);
    }

    @Test
    public void anotherDiscIsRefusedWithTheChoice() throws Exception {
        asGameApp();
        allFilesAccess(true);
        TestSupport.write(new File(folder, "default.xbe"), TestSupport.xbe(0x4D530004, "Sample Game (other region)"));
        LauncherActivity a = launch(null);
        reply(a, false, PackHandover.GameKind.FOLDER, folder.getAbsolutePath());
        settle(a);
        assertEquals(LauncherActivity.Direct.ERROR, a.directStateForTest());
        assertEquals(app.getString(R.string.STATE_WRONG), a.directLineForTest());
        assertEquals(app.getString(R.string.WRONG_GAME), a.directDetailForTest());
        assertEquals(app.getString(R.string.CHOOSE_GAME), a.directPrimaryForTest().title);
        assertNull(Library.read(Library.gamesFile(app.getFilesDir())).get(TID));
        shot(a, "5-wrong-disc");
        LauncherPackHandoverTest.assertGameNotStarted(a);
    }

    @Test
    public void aFolderNoLongerThereSaysSo() throws Exception {
        asGameApp();
        allFilesAccess(true);
        LauncherActivity a = launch(null);
        reply(a, false, PackHandover.GameKind.FOLDER, new File(folder.getParentFile(), "Gone").getAbsolutePath());
        assertEquals(LauncherActivity.Direct.ERROR, a.directStateForTest());
        assertEquals(app.getString(R.string.STATE_MISSING), a.directLineForTest());
        assertEquals(app.getString(R.string.CHOOSE_GAME), a.directPrimaryForTest().title);
    }

    @Test
    public void theGameEndingSaysWhyWithPlay() throws Exception {
        asGameApp();
        readyHere();
        LauncherActivity a = launch(ShellActivity.titleExitedIntent(app, "the game returned to the dashboard"));
        assertEquals(LauncherActivity.Direct.ERROR, a.directStateForTest());
        assertEquals(app.getString(R.string.DIRECT_ENDED), a.directLineForTest());
        assertEquals(LauncherActivity.titleExitedNote(app, "the game returned to the dashboard"),
                     a.directDetailForTest());
        assertEquals(app.getString(R.string.PLAY), a.directPrimaryForTest().title);
        LauncherPackHandoverTest.assertGameNotStarted(a);
        a.directPrimaryForTest().performClick();
        LauncherPackHandoverTest.assertGameStarted(a);
    }

    @Test
    public void theLauncherShortcutStillOpensHome() throws Exception {
        asGameApp();
        readyHere();
        LauncherActivity a = launch(new Intent(app, LauncherActivity.class)
                                            .setAction(LauncherActivity.ACTION_SHOW_LAUNCHER));
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());
        LauncherPackHandoverTest.assertGameNotStarted(a);
    }

    /** Settings › General: no « Start in the game » in a game's own app (it always does); the other rows stay. */
    @Test
    public void settingsHaveNoStartInTheGameRow() throws Exception {
        asGameApp();
        readyHere();
        LauncherActivity a = launch(new Intent(app, LauncherActivity.class)
                                            .setAction(LauncherActivity.ACTION_SHOW_LAUNCHER));
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(300);
        java.util.Set<String> titles = new java.util.HashSet<>();
        java.util.ArrayDeque<android.view.View> q = new java.util.ArrayDeque<>();
        q.add(a.getWindow().getDecorView());
        while (!q.isEmpty()) {
            android.view.View v = q.poll();
            if (v instanceof Widgets.MenuRow)
                titles.add(((Widgets.MenuRow)v).title);
            if (v instanceof android.view.ViewGroup)
                for (int i = 0; i < ((android.view.ViewGroup)v).getChildCount(); i++)
                    q.add(((android.view.ViewGroup)v).getChildAt(i));
        }
        assertTrue("Settings has its rows", titles.contains(app.getString(R.string.UI_SOUND)));
        assertFalse("no « Start in the game »", titles.contains(app.getString(R.string.OPEN_DIRECTLY)));
    }

    @Test
    public void xrecompItselfIsUnchanged() throws Exception {
        readyHere();
        LauncherActivity a = launch(null);
        assertEquals(LauncherActivity.Screen.HOME, a.screenForTest());   // « Open the game directly » is off
        assertNull(a.homePrepForTest());
        LauncherPackHandoverTest.assertGameNotStarted(a);
        assertEquals(java.util.Set.of("change_game", "launcher"), new java.util.HashSet<>(shortcutIds()));
    }
}
