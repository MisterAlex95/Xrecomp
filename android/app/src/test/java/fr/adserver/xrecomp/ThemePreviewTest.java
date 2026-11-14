package fr.adserver.xrecomp;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.app.Application;
import android.os.Looper;
import android.widget.FrameLayout;

import org.junit.Assume;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowEnvironment;

import java.io.File;
import java.time.Duration;

/**
 * Opt-in: the real launcher, the in-game menu and the bottom panel in the theme of a game of your own, rendered to
 * build/screenshots/preview-*.png (git-ignored). Skipped unless the system property xba.previewGameDir names a game
 * folder (its default.xbe): gradle testDebugUnitTest -PxbaPreviewGameDir=/path/to/game --tests '*ThemePreviewTest'.
 * The game's files are only read where they are; nothing of them is copied into the repo.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "w1920dp-h1080dp-land-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class ThemePreviewTest {
    @Rule
    public final TestSupport.StockTheme stockTheme = new TestSupport.StockTheme();

    private static void idle(long ms) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms));
    }

    @Test
    public void previewOwnGame() throws Exception {
        String dir = System.getProperty("xba.previewGameDir", "");
        Assume.assumeTrue("set -PxbaPreviewGameDir=/path/to/game to render a preview", !dir.isEmpty());
        File game = new File(dir);
        File xbe = GameStore.findXbe(game);
        assertNotNull("no default.xbe in " + game, xbe);
        String tid = Pack.xbeTitleId(xbe);
        assertNotNull("default.xbe without a title ID", tid);

        ShadowEnvironment.addExternalDir("sdcard");
        ShadowEnvironment.setExternalStorageState("mounted");
        Application app = RuntimeEnvironment.getApplication();
        String title = GameStore.xbeTitle(xbe);
        GameStore.remember(app, game.getAbsolutePath(), title == null ? "" : title);
        GameTheme t = GameTheme.ensure(app, game, tid, title);
        assertNotNull("no theme (turned off?)", t);
        System.out.println(t.logLine());

        LauncherActivity a = Robolectric.buildActivity(LauncherActivity.class).setup().get();
        idle(300);
        a.forceFocusForTest();
        idle(400);
        assertTrue(TestSupport.screenshot(a.getWindow().getDecorView(), "preview-home-" + tid).length() > 0);
        a.goForTest(LauncherActivity.Screen.SETTINGS);
        idle(400);
        TestSupport.screenshot(a.getWindow().getDecorView(), "preview-settings-" + tid);

        Activity host = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout f = new FrameLayout(host);
        host.setContentView(f);
        LauncherPanel p = new LauncherPanel(host, null);
        f.addView(p, new FrameLayout.LayoutParams(1240, 1080));
        idle(0);
        PanelTesting.layout(p);
        p.setArt(android.graphics.BitmapFactory.decodeFile(TitleArt.cacheFile(app.getFilesDir(), tid).getPath()));
        p.set(title, app.getString(R.string.PANEL_READY), Theme.OK, true);
        PanelTesting.layout(p);
        TestSupport.screenshot(p, "preview-companion-" + tid);
    }
}
