package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.os.Bundle;
import android.os.Looper;
import android.provider.Settings;
import android.text.format.Formatter;
import android.view.View;
import android.view.ViewGroup;

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
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.nio.file.Files;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

/**
 * Manage › Create an app (docs/android/app-shell.md « A game's own app »): the row (its size, hidden in a game's own
 * app), « Install unknown apps » first when it is off, the build behind the progress screen handed to a
 * PackageInstaller session, the installer's outcomes as notes (TitleAppReceiver), and a game's own app locked to its
 * disc through its meta-data (GameStore.expectedSha256).
 *
 * Synthetic only: XBEs from TestSupport.xbe; the template is this build's resource APK (TitleApkTest.resourceApk).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LauncherTitleAppTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Application app;
    private File root, template;
    private String savedSourceDir;
    private static final String NAME = "Sample Quest";
    private static final int TID = 0x58420002;
    private static final String TID_HEX = "58420002";

    @Before
    public void setUp() throws Exception {
        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        app = RuntimeEnvironment.getApplication();
        shadowOf(app).grantPermissions(android.Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        root = tmp.newFolder("xbox");
        ActivityManager am = app.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        mi.totalMem = 8L << 30;
        mi.availMem = 4L << 30;
        shadowOf(am).setMemoryInfo(mi);
        // This build's resource APK stands for the installed APK.
        template = tmp.newFile("base.apk");
        Files.copy(TitleApkTest.resourceApk().toPath(), template.toPath(),
                   java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        savedSourceDir = app.getApplicationInfo().sourceDir;
        app.getApplicationInfo().sourceDir = template.getPath();
        TitleApp.testKey = TitleApkTest.makeKey("xba-launcher-test");
        TitleApp.resetMeta();
    }

    @After
    public void tearDown() {
        app.getApplicationInfo().sourceDir = savedSourceDir;
        TitleApp.testKey = null;
        TitleApp.resetMeta();
        TitleAppReceiver.listener = null;
    }

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    private void libraryGame() throws Exception {
        File d = new File(root, NAME);
        TestSupport.write(new File(d, "default.xbe"), TestSupport.xbe(TID, NAME, XbeInfo.REGION_WORLD, 0, 0x100));
        Library.setFolder(app, "content://com.android.externalstorage.documents/tree/primary%3Axbox",
                          root.getAbsolutePath());
    }

    private LauncherActivity launch() throws Exception {
        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        Thread t = a.scannerForTest();
        if (t != null)
            t.join(20_000);
        idle(400);
        return a;
    }

    /** The library game's Manage screen. */
    private LauncherActivity manage() throws Exception {
        libraryGame();
        launch().finish();      // the first start scans the library
        LauncherActivity a = launch();
        GameCard card = null;
        for (GameCard c : a.libraryCardsForTest()) {
            if (NAME.equals(c.titleForTest()))
                card = c;
        }
        assertNotNull(card);
        card.performClick();
        idle(300);
        a.goForTest(LauncherActivity.Screen.GAME_DATA);
        idle(300);
        return a;
    }

    private static List<View> allViews(View v, List<View> out) {
        out.add(v);
        if (v instanceof ViewGroup) {
            for (int i = 0; i < ((ViewGroup)v).getChildCount(); i++)
                allViews(((ViewGroup)v).getChildAt(i), out);
        }
        return out;
    }

    private Widgets.MenuRow row(LauncherActivity a, int title) {
        for (View v : allViews(a.getWindow().getDecorView(), new ArrayList<>())) {
            if (v instanceof Widgets.MenuRow && app.getString(title).equals(((Widgets.MenuRow)v).title))
                return (Widgets.MenuRow)v;
        }
        return null;
    }

    @Test
    public void rowSaysTheSizeAndAsksForInstallUnknownAppsFirst() throws Exception {
        shadowOf(app.getPackageManager()).setCanRequestPackageInstalls(false);
        LauncherActivity a = manage();
        Widgets.MenuRow r = row(a, R.string.TITLE_APP_ADD);
        assertNotNull("Manage offers « Create an app »", r);
        assertEquals(app.getString(R.string.TITLE_APP_ADD_DETAIL, Formatter.formatShortFileSize(a, template.length())),
                     r.description);
        r.performClick();
        idle(300);
        assertEquals(app.getString(R.string.TITLE_APP_ALLOW), a.noteForTest());
        Intent next = shadowOf(app).getNextStartedActivity();
        assertNotNull(next);
        assertEquals(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, next.getAction());
        assertEquals("package:" + app.getPackageName(), next.getDataString());
        assertNull("nothing was built", a.workerForTest());
    }

    @Test
    public void buildsAndHandsTheAppToTheInstaller() throws Exception {
        shadowOf(app.getPackageManager()).setCanRequestPackageInstalls(true);
        LauncherActivity a = manage();
        Widgets.MenuRow r = row(a, R.string.TITLE_APP_ADD);
        assertNotNull(r);
        r.performClick();
        // The progress screen while it builds (the small template may be done before the main looper runs again).
        assertEquals(LauncherActivity.Screen.IMPORT, a.screenForTest());
        Thread w = a.workerForTest();
        assertNotNull(w);
        w.join(60_000);
        idle(300);
        assertEquals(app.getString(R.string.TITLE_APP_CONFIRM, NAME), a.noteForTest());
        assertEquals(LauncherActivity.Screen.GAME_DATA, a.screenForTest());
        PackageInstaller pi = app.getPackageManager().getPackageInstaller();
        List<PackageInstaller.SessionInfo> sessions = pi.getAllSessions();
        assertEquals(1, sessions.size());
        assertEquals("fr.adserver.xboxrecomp.t" + TID_HEX, sessions.get(0).getAppPackageName());
        String[] left = TitleApp.workDir(app).list();
        assertTrue("the work directory is empty", left == null || left.length == 0);
    }

    @Test
    public void installerOutcomesBecomeNotes() throws Exception {
        LauncherActivity a = manage();
        TitleAppReceiver rcv = new TitleAppReceiver();
        Intent in = new Intent(TitleAppReceiver.ACTION).putExtra(TitleAppReceiver.EXTRA_LABEL, NAME)
                .putExtra(PackageInstaller.EXTRA_PACKAGE_NAME, "fr.adserver.xboxrecomp.t58420002");

        // Android's confirmation first: the receiver starts it.
        Intent confirm = new Intent("android.content.pm.action.CONFIRM_INSTALL");
        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS,
                                                   PackageInstaller.STATUS_PENDING_USER_ACTION)
                                         .putExtra(Intent.EXTRA_INTENT, confirm));
        Intent started = shadowOf(app).getNextStartedActivity();
        assertEquals("android.content.pm.action.CONFIRM_INSTALL", started.getAction());
        assertTrue((started.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0);
        assertNull(TitleApp.takeOutcome(app));

        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_SUCCESS));
        idle(100);
        assertEquals(app.getString(R.string.TITLE_APP_INSTALLED, NAME), a.noteForTest());

        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS,
                                                   PackageInstaller.STATUS_FAILURE_ABORTED));
        idle(100);
        assertEquals(app.getString(R.string.TITLE_APP_CANCELLED), a.noteForTest());

        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS,
                                                   PackageInstaller.STATUS_FAILURE_CONFLICT));
        idle(100);
        assertEquals(app.getString(R.string.TITLE_APP_CONFLICT, NAME), a.noteForTest());

        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
                                         .putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE, "INSTALL_FAILED_X"));
        idle(100);
        assertEquals(app.getString(R.string.TITLE_APP_FAILED, "INSTALL_FAILED_X"), a.noteForTest());

        // In the background: kept, then shown at the next resume.
        a.onPause();
        rcv.onReceive(app, new Intent(in).putExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_SUCCESS));
        a.onResume();
        assertEquals(app.getString(R.string.TITLE_APP_INSTALLED, NAME), a.noteForTest());
    }

    @Test
    public void aGameOwnAppIsLockedToItsDiscAndOffersNoCopy() throws Exception {
        PackageInfo pi = shadowOf(app.getPackageManager()).getInternalMutablePackageInfo(app.getPackageName());
        Bundle meta = new Bundle();
        meta.putString(TitleApk.META_TITLE_ID, TID_HEX);
        meta.putString(TitleApk.META_XBE_SHA256, "ABCDEF" + "0".repeat(58));
        pi.applicationInfo.metaData = meta;
        TitleApp.resetMeta();
        assertEquals(TID_HEX, TitleApp.ownTitleId(app));
        assertEquals("abcdef" + "0".repeat(58), GameStore.expectedSha256(app));
        assertTrue(GameStore.hashMatches(app, "abcdef" + "0".repeat(58)));
        assertTrue(!GameStore.hashMatches(app, "0".repeat(64)));
        LauncherActivity a = manage();
        assertNull("no « Create an app » in a game's own app", row(a, R.string.TITLE_APP_ADD));
        assertNotNull(row(a, R.string.SHORTCUT_ADD));
    }

    @Test
    public void xrecompItselfHasNoLock() {
        assertNull(TitleApp.ownTitleId(app));
        assertEquals(app.getString(R.string.xba_xbe_sha256).trim(), GameStore.expectedSha256(app));
    }
}
