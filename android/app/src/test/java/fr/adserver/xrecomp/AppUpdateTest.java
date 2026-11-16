package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.app.Application;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import java.util.List;

/**
 * AppUpdate (docs/android/app-shell.md « Updates »): which GitHub releases count, the channels, the version code from
 * the commit's time (cached), the ETag, the failure states, the download's checks, the preferences.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AppUpdateTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String SHA_REL = "9882babba7fe54f6a372a07b8fc27241d31175ac";
    static final String SHA_NEW = "aafc24641ecf3f63859285252dbfe5e5d6708e5e";
    static final String SHA_OLD = "8abfb3bebf9cad07a995864763dfdb3352f43f26";
    static final String RELEASES = AppUpdate.API + "/releases?per_page=" + AppUpdate.PER_PAGE;
    /** 2026-11-16T15:46:04Z / 60 s. */
    static final long CODE_REL = 29914066L;

    private VkDriverIndexTest.FakeHttp http;
    private Application app;

    @Before
    public void setUp() {
        http = new VkDriverIndexTest.FakeHttp();
        app = RuntimeEnvironment.getApplication();
    }

    @After
    public void tearDown() {
        AppUpdate.unsupportedHook = null;
        AppUpdate.apkCheck = AppUpdate::checkApk;
    }

    static JSONObject release(String tag, boolean pre, String sha, String published, String asset, byte[] apk)
            throws Exception {
        JSONObject a = new JSONObject().put("name", asset).put("size", apk.length)
                .put("digest", "sha256:" + VkDriverIndex.hex(java.security.MessageDigest.getInstance("SHA-256")
                                                                                       .digest(apk)))
                .put("browser_download_url",
                     "https://github.com/MisterAlex95/Xrecomp/releases/download/" + tag + "/" + asset);
        return new JSONObject().put("tag_name", tag).put("name", "XRecomp " + tag).put("draft", false)
                .put("prerelease", pre).put("target_commitish", sha).put("published_at", published)
                .put("body", "## What changed\n\n- **Faster** start\n- [Docs](https://example.org)\n")
                .put("assets", new JSONArray().put(a));
    }

    static String commit(String date) throws Exception {
        return new JSONObject().put("sha", "x").put("committer", new JSONObject().put("date", date)).toString();
    }

    static byte[] bytes(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    /** v0.12.1 (SHA_REL, 15:46:04) and xrecomp-latest (SHA_NEW, an hour later), plus things that never count. */
    String page() throws Exception {
        byte[] apk = bytes("apk");
        JSONArray a = new JSONArray()
            .put(release("xrecomp-latest", true, SHA_NEW, "2026-11-16T17:00:00Z", "xrecomp-aafc2464.apk", apk))
            .put(release("xrecomp-v0.12.1", false, SHA_REL, "2026-11-16T16:36:34Z", "xrecomp-v0.12.1.apk", apk))
            .put(release("xrecomp-v0.12.0", false, SHA_OLD, "2026-11-16T05:55:31Z", "xrecomp-v0.12.0.apk", apk))
            // upstream's tags, the shell, a draft, a foreign URL: never offered
            .put(release("v0.5.0", false, SHA_NEW, "2026-11-16T18:00:00Z", "v0.5.0.apk", apk))
            .put(release("shell-latest", true, SHA_NEW, "2026-11-16T18:00:00Z", "xboxrecomp-shell-aafc2464.apk", apk))
            .put(release("xrecomp-v0.13.0", false, SHA_NEW, "2026-11-16T18:00:00Z", "xrecomp-v0.13.0.apk", apk)
                     .put("draft", true));
        JSONObject evil = release("xrecomp-v0.14.0", false, SHA_NEW, "2026-11-16T19:00:00Z", "xrecomp-v0.14.0.apk", apk);
        evil.getJSONArray("assets").getJSONObject(0).put("browser_download_url", "https://example.org/x.apk");
        a.put(evil);
        return a.toString();
    }

    void serveCommits() throws Exception {
        String rel = commit("2026-11-16T15:46:04Z"), nw = commit("2026-11-16T16:46:04Z");
        http.on(AppUpdate.API + "/git/commits/" + SHA_REL, r -> VkDriverIndexTest.resp(200, bytes(rel)));
        http.on(AppUpdate.API + "/git/commits/" + SHA_NEW, r -> VkDriverIndexTest.resp(200, bytes(nw)));
    }

    @Test
    public void parseKeepsOnlyXrecompApks() throws Exception {
        List<AppUpdate.Release> l = AppUpdate.parseReleases(page());
        assertEquals(3, l.size());
        assertEquals("xrecomp-latest", l.get(0).tag);
        assertEquals("xrecomp-aafc2464.apk", l.get(0).assetName);
        assertTrue(l.get(0).testBuild());
        assertEquals("aafc2464", l.get(0).version());
        assertEquals("0.12.1", l.get(1).version());
        assertEquals(SHA_REL, l.get(1).commit);
        assertEquals(3, l.get(1).size);
        assertNotNull(l.get(1).sha256);
        assertEquals(Instant.parse("2026-11-16T16:36:34Z").toEpochMilli(), l.get(1).publishedAt);
    }

    @Test
    public void releasesChannelOffersTheNewestRelease() throws Exception {
        String p = page();
        http.on(RELEASES, r -> VkDriverIndexTest.resp(200, bytes(p), "ETag", "\"e1\""));
        serveCommits();
        AppUpdate.Check k = AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, CODE_REL - 100, 1);
        assertEquals(AppUpdate.State.AVAILABLE, k.state);
        assertEquals("xrecomp-v0.12.1", k.release.tag);
        assertEquals(CODE_REL, k.release.versionCode);
        // The headers: a plain User-Agent, nothing else about the device.
        assertEquals("XRecomp", http.requests.get(0).headers.get("User-Agent"));
    }

    @Test
    public void sameVersionIsUpToDate() throws Exception {
        String p = page();
        http.on(RELEASES, r -> VkDriverIndexTest.resp(200, bytes(p)));
        serveCommits();
        AppUpdate.Check k = AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, CODE_REL, 1);
        assertEquals(AppUpdate.State.UP_TO_DATE, k.state);
        assertEquals("the newest release is still named", "xrecomp-v0.12.1", k.release.tag);
    }

    @Test
    public void latestChannelTakesTheNewerTestBuild() throws Exception {
        String p = page();
        http.on(RELEASES, r -> VkDriverIndexTest.resp(200, bytes(p)));
        serveCommits();
        AppUpdate.Check k = AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.LATEST, CODE_REL, 1);
        assertEquals(AppUpdate.State.AVAILABLE, k.state);
        assertEquals("xrecomp-latest", k.release.tag);
        assertEquals(CODE_REL + 60, k.release.versionCode);
    }

    @Test
    public void notModifiedAndCachedCommitsCostOneRequest() throws Exception {
        String p = page();
        http.on(RELEASES, r -> VkDriverIndexTest.resp(200, bytes(p), "ETag", "\"e1\""));
        serveCommits();
        AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.LATEST, CODE_REL, 1);
        int before = http.requests.size();
        http.on(RELEASES, r -> {
            assertEquals("\"e1\"", r.headers.get("If-None-Match"));
            return VkDriverIndexTest.resp(304, new byte[0]);
        });
        AppUpdate.Check k = AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.LATEST, CODE_REL, 2);
        assertEquals(AppUpdate.State.AVAILABLE, k.state);
        assertEquals("one request: the 304; the commits come from the cache", before + 1, http.requests.size());
    }

    @Test
    public void failuresSayWhy() throws Exception {
        http.on(RELEASES, r -> { throw new java.net.UnknownHostException("api.github.com"); });
        assertEquals(AppUpdate.State.OFFLINE,
                     AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, 1, 1).state);
        http.on(RELEASES, r -> VkDriverIndexTest.resp(403, new byte[0], "x-ratelimit-remaining", "0"));
        assertEquals(AppUpdate.State.RATE_LIMITED,
                     AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, 1, 1).state);
        http.on(RELEASES, r -> VkDriverIndexTest.resp(500, new byte[0]));
        assertEquals(AppUpdate.State.FAILED,
                     AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, 1, 1).state);
        http.on(RELEASES, r -> VkDriverIndexTest.resp(200, bytes("not json")));
        assertEquals(AppUpdate.State.FAILED,
                     AppUpdate.check(http, tmp.getRoot(), AppUpdate.Channel.RELEASES, 1, 1).state);
    }

    @Test
    public void defaultChannelFollowsTheBuild() {
        assertEquals(AppUpdate.Channel.RELEASES, AppUpdate.defaultChannel("0.12.1+9882babb"));
        assertEquals(AppUpdate.Channel.LATEST, AppUpdate.defaultChannel("9882babb"));
        assertEquals(AppUpdate.Channel.LATEST, AppUpdate.defaultChannel("dev"));
    }

    @Test
    public void prefsAndPolicy() {
        AppUpdate.unsupportedHook = c -> null;
        assertTrue("on by default", AppUpdate.autoOn(app));
        assertTrue("never checked: due", AppUpdate.autoDue(app, 1000));
        AppUpdate.setAutoOn(app, false);
        assertFalse(AppUpdate.autoDue(app, 1000));
        AppUpdate.setAutoOn(app, true);
        AppUpdate.unsupportedHook = c -> AppUpdate.Unsupported.OTHER_SIGNER;
        assertFalse("a build that cannot update never asks", AppUpdate.autoDue(app, 1000));
    }

    @Test
    public void unsupportedWithoutOurKey() {
        // Robolectric's app is not signed with the published key; a game's own app has no INTERNET.
        org.robolectric.Shadows.shadowOf(app).grantPermissions(android.Manifest.permission.INTERNET);
        assertEquals(AppUpdate.Unsupported.OTHER_SIGNER, AppUpdate.unsupported(app));
        org.robolectric.Shadows.shadowOf(app).denyPermissions(android.Manifest.permission.INTERNET);
        assertEquals(AppUpdate.Unsupported.GAME_APP, AppUpdate.unsupported(app));
    }

    @Test
    public void downloadChecksDigestAndApk() throws Exception {
        byte[] apk = bytes("apk");
        AppUpdate.Release r = AppUpdate.parseReleases(new JSONArray()
            .put(release("xrecomp-v0.12.1", false, SHA_REL, "2026-11-16T16:36:34Z", "xrecomp-v0.12.1.apk", apk))
            .toString()).get(0);
        String cdn = "https://release-assets.githubusercontent.com/x";
        http.on(r.url, q -> VkDriverIndexTest.resp(302, new byte[0], "Location", cdn));
        http.on(cdn, q -> VkDriverIndexTest.resp(200, apk));
        AppUpdate.apkCheck = (c, f) -> null;
        File f = AppUpdate.download(app, http, r, null, null);
        assertEquals("apk", new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));

        // Another file than the release's digest: refused, nothing kept.
        http.on(r.url, q -> VkDriverIndexTest.resp(200, bytes("xyz")));
        try {
            AppUpdate.download(app, http, r, null, null);
            fail("digest mismatch accepted");
        } catch (VkDriverIndex.DownloadException e) {
            assertEquals(VkDriverIndex.DownloadException.Reason.SHA256, e.reason);
        }
        // Not an update of this app (another key, an older version): refused.
        http.on(r.url, q -> VkDriverIndexTest.resp(200, apk));
        AppUpdate.apkCheck = (c, x) -> "signed by another key";
        try {
            AppUpdate.download(app, http, r, null, null);
            fail("bad APK accepted");
        } catch (AppUpdate.BadApk e) {
            assertEquals("signed by another key", e.getMessage());
        }
        File[] left = AppUpdate.dir(app).listFiles((d, n) -> n.endsWith(".apk") || n.endsWith(".part"));
        assertEquals(0, left == null ? 0 : left.length);
        // The real check refuses a file that is not an APK.
        assertEquals("not an APK", AppUpdate.checkApk(app, f));
    }

    @Test
    public void outcomeIsTakenOnce() {
        assertNull(AppUpdate.takeOutcome(app));
        AppUpdate.saveOutcome(app, android.content.pm.PackageInstaller.STATUS_FAILURE_ABORTED, "", "0.13.0");
        String[] o = AppUpdate.takeOutcome(app);
        assertEquals("0.13.0", o[2]);
        assertNull(AppUpdate.takeOutcome(app));
    }

    @Test
    public void notesExcerptIsPlainText() {
        String s = LauncherActivity.notesExcerpt("## What changed\n\n- **Faster** start\n- [Docs](https://x.org)\n", 600);
        assertEquals("What changed\n• Faster start\n• Docs", s);
        assertTrue(LauncherActivity.notesExcerpt("a".repeat(700), 600).endsWith("…"));
    }
}
