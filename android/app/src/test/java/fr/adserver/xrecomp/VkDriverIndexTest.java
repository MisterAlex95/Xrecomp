package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * VkDriverIndex (docs/android/app-shell.md « Vulkan drivers »): the GitHub releases parsing and the Turnip filter on
 * recorded answers (src/test/resources/vkdriver: GitHub's releases pages of the two sources on 2026-11-14, trimmed to
 * the fields used), the ETag + 24 h cache, offline / limit / error states, and the verified download with a fake HTTP
 * layer. No driver binary: downloads are a few synthetic bytes.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class VkDriverIndexTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final VkDriverIndex.Source PURPLE = VkDriverIndex.SOURCES.get(0);
    static final VkDriverIndex.Source K11 = VkDriverIndex.SOURCES.get(1);
    static final String T30_SHA = "f65b2d3353fd4aa7190bb5426b94468e99ffea7a58a830bc0c4651db89353227";
    static final long HOUR = 3600_000L;

    long now = Instant.parse("2026-11-14T10:00:00Z").toEpochMilli();
    FakeHttp http;
    File indexDir, cacheDir;

    @Before
    public void setUp() throws IOException {
        http = new FakeHttp();
        indexDir = new File(tmp.getRoot(), "files/vk_drivers/index");
        cacheDir = tmp.newFolder("cache");
    }

    VkDriverIndex index() {
        return index(true);
    }

    VkDriverIndex index(boolean network) {
        return new VkDriverIndex(indexDir, cacheDir, http, () -> now, network, VkDriverIndex.SOURCES);
    }

    // ---- fake HTTP ---------------------------------------------------------------------------------------------

    static final class Req {
        final String url;
        final Map<String, String> headers;

        Req(String url, Map<String, String> headers) {
            this.url = url;
            this.headers = new HashMap<>(headers);
        }
    }

    interface Answer {
        VkDriverIndex.Response answer(Req r) throws IOException;
    }

    static final class FakeHttp implements VkDriverIndex.Http {
        final List<Req> requests = new ArrayList<>();
        final Map<String, Deque<Answer>> answers = new HashMap<>();

        void on(String url, Answer a) {
            answers.computeIfAbsent(url, k -> new ArrayDeque<>()).add(a);
        }

        @Override
        public VkDriverIndex.Response get(String url, Map<String, String> headers) throws IOException {
            Req r = new Req(url, headers);
            requests.add(r);
            Deque<Answer> q = answers.get(url);
            if (q == null || q.isEmpty())
                throw new AssertionError("unexpected request " + url);
            return q.poll().answer(r);
        }
    }

    static VkDriverIndex.Response resp(int code, byte[] body, String... headers) {
        Map<String, String> h = new HashMap<>();
        for (int i = 0; i + 1 < headers.length; i += 2)
            h.put(headers[i].toLowerCase(), headers[i + 1]);
        return new VkDriverIndex.Response() {
            @Override
            public int code() {
                return code;
            }

            @Override
            public String header(String name) {
                return h.get(name.toLowerCase());
            }

            @Override
            public long length() {
                return h.containsKey("content-length") ? Long.parseLong(h.get("content-length")) : -1;
            }

            @Override
            public InputStream body() {
                return new ByteArrayInputStream(body);
            }

            @Override
            public void close() {}
        };
    }

    static String fixture(String name) throws IOException {
        try (InputStream in = VkDriverIndexTest.class.getClassLoader().getResourceAsStream("vkdriver/" + name)) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    void serveBoth(String etagPurple, String etagK11) throws IOException {
        String p = fixture("purple-turnip.json"), k = fixture("AdrenoToolsDrivers.json");
        http.on(PURPLE.releasesUrl(), r -> resp(200, p.getBytes(StandardCharsets.UTF_8), "ETag", etagPurple));
        http.on(K11.releasesUrl(), r -> resp(200, k.getBytes(StandardCharsets.UTF_8), "ETag", etagK11));
    }

    static List<VkDriverIndex.Entry> of(VkDriverIndex.Listing l, VkDriverIndex.Source s) {
        List<VkDriverIndex.Entry> out = new ArrayList<>();
        for (VkDriverIndex.Entry e : l.entries) {
            if (e.source.equals(s.id()))
                out.add(e);
        }
        return out;
    }

    // ---- parsing and the filter --------------------------------------------------------------------------------

    @Test
    public void purpleTurnipT30() throws Exception {
        List<VkDriverIndex.Entry> l = VkDriverIndex.filter(PURPLE,
            VkDriverIndex.parseReleases(PURPLE, fixture("purple-turnip.json")));
        assertEquals(4, l.size());
        VkDriverIndex.Entry t30 = l.get(0);
        assertEquals("MrPurple666/purple-turnip", t30.source);
        assertEquals("vturnip_mrpurple_T30-toasted.adpkg", t30.tag);
        assertEquals("turnip_mrpurple_T30-toasted.adpkg.zip", t30.assetName);
        assertEquals(Instant.parse("2026-08-17T12:38:01Z").toEpochMilli(), t30.publishedAt);
        assertEquals(3713730L, t30.size);
        assertEquals(T30_SHA, t30.sha256);
        assertEquals("https://github.com/MrPurple666/purple-turnip/releases/download/"
                     + "vturnip_mrpurple_T30-toasted.adpkg/turnip_mrpurple_T30-toasted.adpkg.zip", t30.url);
        assertFalse(t30.prerelease);
        for (int i = 1; i < l.size(); i++)
            assertTrue("newest first", l.get(i - 1).publishedAt >= l.get(i).publishedAt);
    }

    @Test
    public void k11TurnipOnlyNeverQualcomm() throws Exception {
        List<VkDriverIndex.Entry> all = VkDriverIndex.parseReleases(K11, fixture("AdrenoToolsDrivers.json"));
        List<VkDriverIndex.Entry> l = VkDriverIndex.filter(K11, all);
        // The page has Qualcomm_840/837/849_adpkg.zip, Qualcomm_819.2_adpkg.zip and 8Elite2-842.6.zip (proprietary):
        // none listed; the 29 Turnip assets are.
        assertTrue(all.size() > l.size());
        assertEquals(29, l.size());
        Set<String> names = new HashSet<>();
        for (VkDriverIndex.Entry e : l) {
            names.add(e.assetName);
            String n = e.assetName.toLowerCase();
            assertTrue(n, n.contains("turnip") && n.endsWith(".zip") && !n.contains("qualcomm"));
            assertFalse(e.release, e.release.toLowerCase().contains("qualcomm"));
            assertTrue(e.sha256 != null && e.size > 0);
        }
        assertTrue(names.contains("Turnip_v26.0.0_R8.zip"));
        assertTrue(names.contains("Turnip_v26.0.0_R8_Gmem.zip"));
        assertTrue(names.contains("turnip_a8xx.zip"));
        assertFalse(names.contains("8Elite2-842.6.zip"));
        assertFalse(names.contains("Qualcomm_840_adpkg.zip"));
    }

    static VkDriverIndex.Entry entry(VkDriverIndex.Source s, String tag, String release, String asset, String url) {
        return new VkDriverIndex.Entry(s.id(), tag, release, false, 1, asset, 10, null, url);
    }

    static String dl(VkDriverIndex.Source s, String tag, String asset) {
        return "https://github.com/" + s.owner + "/" + s.repo + "/releases/download/" + tag + "/" + asset;
    }

    @Test
    public void filterRules() {
        // Turnip-only source: the name must say turnip; a Qualcomm release or asset never, even « turnip ».
        assertTrue(VkDriverIndex.accept(K11, entry(K11, "v1", "v1", "Turnip_x.zip", dl(K11, "v1", "Turnip_x.zip"))));
        assertFalse(VkDriverIndex.accept(K11, entry(K11, "v1", "v1", "Adreno_x.zip", dl(K11, "v1", "Adreno_x.zip"))));
        assertFalse(VkDriverIndex.accept(K11, entry(K11, "v1", "Qualcomm Driver v9", "turnip.zip",
                                                    dl(K11, "v1", "turnip.zip"))));
        assertFalse(VkDriverIndex.accept(K11, entry(K11, "v1", "", "Qualcomm_turnip.zip",
                                                    dl(K11, "v1", "Qualcomm_turnip.zip"))));
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "qualcomm_840.zip",
                                                       dl(PURPLE, "v1", "qualcomm_840.zip"))));
        // purple-turnip: any .zip of its own releases.
        assertTrue(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.zip", dl(PURPLE, "v1", "pkg.zip"))));
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.tar.gz",
                                                       dl(PURPLE, "v1", "pkg.tar.gz"))));
        // The URL: https on github.com under the source's own releases only.
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.zip", dl(K11, "v1", "pkg.zip"))));
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.zip",
            "http://github.com/MrPurple666/purple-turnip/releases/download/v1/pkg.zip")));
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.zip",
            "https://evil.example/MrPurple666/purple-turnip/releases/download/v1/pkg.zip")));
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "pkg.zip",
            "https://github.com:8443/MrPurple666/purple-turnip/releases/download/v1/pkg.zip")));
        // Odd asset names (they become a temporary file name).
        assertFalse(VkDriverIndex.accept(PURPLE, entry(PURPLE, "v1", "", "../x.zip", dl(PURPLE, "v1", "x.zip"))));
        // Digests.
        assertEquals(T30_SHA, VkDriverIndex.parseDigest("sha256:" + T30_SHA.toUpperCase()));
        assertNull(VkDriverIndex.parseDigest(null));
        assertNull(VkDriverIndex.parseDigest("sha1:abcd"));
        assertNull(VkDriverIndex.parseDigest("sha256:abcd"));
    }

    @Test
    public void draftsMissingDigestsAndDuplicates() throws Exception {
        String sha = "aa".repeat(32);
        String json = "[{\"tag_name\":\"v3\",\"name\":\"draft\",\"draft\":true,\"prerelease\":false,"
                      + "\"published_at\":null,\"assets\":[{\"name\":\"turnip_d.zip\",\"size\":5,\"digest\":null,"
                      + "\"browser_download_url\":\"" + dl(K11, "v3", "turnip_d.zip") + "\"}]},"
                      + "{\"tag_name\":\"v2\",\"name\":null,\"draft\":false,\"prerelease\":true,"
                      + "\"published_at\":\"2026-02-01T00:00:00Z\",\"assets\":["
                      + "{\"name\":\"turnip_a.zip\",\"size\":5,\"digest\":\"sha256:" + sha + "\","
                      + "\"browser_download_url\":\"" + dl(K11, "v2", "turnip_a.zip") + "\"},"
                      + "{\"name\":\"turnip_nodigest.zip\",\"size\":7,"
                      + "\"browser_download_url\":\"" + dl(K11, "v2", "turnip_nodigest.zip") + "\"}]},"
                      + "{\"tag_name\":\"v1\",\"name\":\"old\",\"draft\":false,\"prerelease\":false,"
                      + "\"published_at\":\"2026-01-01T00:00:00Z\",\"assets\":["
                      + "{\"name\":\"turnip_a_again.zip\",\"size\":5,\"digest\":\"sha256:" + sha + "\","
                      + "\"browser_download_url\":\"" + dl(K11, "v1", "turnip_a_again.zip") + "\"}]}]";
        List<VkDriverIndex.Entry> l = VkDriverIndex.dedupe(VkDriverIndex.filter(K11,
            VkDriverIndex.parseReleases(K11, json)));
        assertEquals(2, l.size());
        assertEquals("turnip_a.zip", l.get(0).assetName);   // the newer of the two copies of sha
        assertTrue(l.get(0).prerelease);
        assertEquals("", l.get(0).release);
        assertEquals("turnip_nodigest.zip", l.get(1).assetName);
        assertNull(l.get(1).sha256);
        // Install state: by sha256; unknown (never « installed ») without a digest.
        Set<String> installed = Collections.singleton(sha);
        assertTrue(l.get(0).installedIn(installed));
        assertFalse(l.get(1).installedIn(installed));
        VkDriverIndex.Listing listing = new VkDriverIndex.Listing(l, new ArrayList<>());
        assertEquals(1, listing.notInstalled(installed).size());
    }

    // ---- the cache and the states ------------------------------------------------------------------------------

    @Test
    public void noRequestUnlessAsked() {
        VkDriverIndex idx = index();
        VkDriverIndex.Listing l = idx.cached();
        assertTrue(l.entries.isEmpty());
        assertEquals(2, l.status.size());
        for (VkDriverIndex.SourceStatus s : l.status)
            assertEquals(VkDriverIndex.State.NONE, s.state);
        assertTrue(http.requests.isEmpty());
        assertFalse(indexDir.exists());
    }

    @Test
    public void refreshThenCacheThen304() throws Exception {
        serveBoth("W/\"p1\"", "W/\"k1\"");
        VkDriverIndex idx = index();
        VkDriverIndex.Listing l = idx.refresh(false);
        assertEquals(2, http.requests.size());
        for (Req r : http.requests) {
            assertEquals("XRecomp", r.headers.get("User-Agent"));
            assertEquals("application/vnd.github+json", r.headers.get("Accept"));
            assertNull(r.headers.get("If-None-Match"));
            assertTrue(r.url, r.url.startsWith("https://api.github.com/repos/") && r.url.endsWith("?per_page=30"));
        }
        for (VkDriverIndex.SourceStatus s : l.status) {
            assertEquals(VkDriverIndex.State.UPDATED, s.state);
            assertEquals(now, s.fetchedAt);
            assertFalse(s.stale());
        }
        assertEquals(4, of(l, PURPLE).size());
        assertEquals(29, of(l, K11).size());
        assertEquals(T30_SHA, l.entries.get(0).sha256);   // purple first, newest first
        assertTrue(idx.cacheFile(PURPLE).isFile());

        // Within 24 h: the copy, no request; the same list from a new instance (another screen opening, offline).
        now += 23 * HOUR;
        VkDriverIndex.Listing again = index().refresh(false);
        assertEquals(2, http.requests.size());
        for (VkDriverIndex.SourceStatus s : again.status)
            assertEquals(VkDriverIndex.State.CACHED, s.state);
        assertEquals(l.entries.size(), again.entries.size());
        VkDriverIndex.Listing cached = index().cached();
        assertEquals(l.entries.size(), cached.entries.size());
        assertEquals(VkDriverIndex.State.CACHED, cached.status.get(0).state);

        // After 24 h: asked again with the ETag; 304 keeps the list and refreshes its date.
        now += 2 * HOUR;
        assertEquals(VkDriverIndex.State.STALE, index().cached().status.get(0).state);
        http.on(PURPLE.releasesUrl(), r -> resp(304, new byte[0]));
        http.on(K11.releasesUrl(), r -> resp(304, new byte[0]));
        VkDriverIndex.Listing nm = index().refresh(false);
        assertEquals(4, http.requests.size());
        assertEquals("W/\"p1\"", http.requests.get(2).headers.get("If-None-Match"));
        assertEquals("W/\"k1\"", http.requests.get(3).headers.get("If-None-Match"));
        for (VkDriverIndex.SourceStatus s : nm.status) {
            assertEquals(VkDriverIndex.State.NOT_MODIFIED, s.state);
            assertEquals(now, s.fetchedAt);
        }
        assertEquals(l.entries.size(), nm.entries.size());

        // The player's Refresh asks even when fresh.
        http.on(PURPLE.releasesUrl(), r -> resp(304, new byte[0]));
        http.on(K11.releasesUrl(), r -> resp(304, new byte[0]));
        index().refresh(true);
        assertEquals(6, http.requests.size());
    }

    @Test
    public void rateLimited() throws Exception {
        serveBoth("a", "b");
        index().refresh(false);
        now += 25 * HOUR;
        long reset = (now / 1000 + 1800);
        http.on(PURPLE.releasesUrl(), r -> resp(403, "{\"message\":\"API rate limit exceeded\"}".getBytes(),
                                                "x-ratelimit-remaining", "0", "x-ratelimit-reset", "" + reset));
        http.on(K11.releasesUrl(), r -> resp(403, new byte[0], "x-ratelimit-remaining", "0",
                                             "x-ratelimit-reset", "" + reset));
        VkDriverIndex.Listing l = index().refresh(false);
        for (VkDriverIndex.SourceStatus s : l.status) {
            assertEquals(VkDriverIndex.State.RATE_LIMITED, s.state);
            assertEquals(reset * 1000, s.resetAt);
            assertEquals(403, s.httpCode);
            assertTrue(s.stale());
            assertEquals(now - 25 * HOUR, s.fetchedAt);
        }
        assertEquals(33, l.entries.size());   // the cached list
        // Before the reset, no automatic request; cached() says the same.
        now += 10 * 60 * 1000;
        int before = http.requests.size();
        VkDriverIndex.Listing l2 = index().refresh(false);
        assertEquals(before, http.requests.size());
        assertEquals(VkDriverIndex.State.RATE_LIMITED, l2.status.get(0).state);
        assertEquals(VkDriverIndex.State.RATE_LIMITED, index().cached().status.get(0).state);
        // A 403 that is not the limit is a plain failure.
        http.on(PURPLE.releasesUrl(), r -> resp(403, new byte[0], "x-ratelimit-remaining", "42"));
        http.on(K11.releasesUrl(), r -> resp(500, new byte[0]));
        VkDriverIndex.Listing l3 = index().refresh(true);
        assertEquals(VkDriverIndex.State.FAILED, l3.status.get(0).state);
        assertEquals(VkDriverIndex.State.FAILED, l3.status.get(1).state);
        assertEquals(500, l3.status.get(1).httpCode);
        assertEquals(33, l3.entries.size());
    }

    @Test
    public void rateLimitedWithoutCache() {
        http.on(PURPLE.releasesUrl(), r -> resp(429, new byte[0], "Retry-After", "60"));
        http.on(K11.releasesUrl(), r -> { throw new UnknownHostException("api.github.com"); });
        VkDriverIndex.Listing l = index().refresh(false);
        assertTrue(l.entries.isEmpty());
        assertEquals(VkDriverIndex.State.RATE_LIMITED, l.status.get(0).state);
        assertEquals(now + 60_000, l.status.get(0).resetAt);
        assertEquals(0, l.status.get(0).fetchedAt);
        assertEquals(VkDriverIndex.State.OFFLINE, l.status.get(1).state);
        assertEquals(0, l.oldestFetch());
    }

    @Test
    public void offlineKeepsTheCacheNeverThrows() throws Exception {
        serveBoth("a", "b");
        index().refresh(false);
        long fetched = now;
        now += 30 * HOUR;
        http.on(PURPLE.releasesUrl(), r -> { throw new UnknownHostException("api.github.com"); });
        http.on(K11.releasesUrl(), r -> { throw new IllegalStateException("boom"); });
        VkDriverIndex.Listing l = index().refresh(false);
        assertEquals(VkDriverIndex.State.OFFLINE, l.status.get(0).state);
        assertEquals(VkDriverIndex.State.OFFLINE, l.status.get(1).state);
        assertEquals(fetched, l.oldestFetch());
        assertEquals(33, l.entries.size());
        // A SecurityException (no INTERNET permission) is « no network ».
        http.on(PURPLE.releasesUrl(), r -> { throw new SecurityException("Permission denied (missing INTERNET)"); });
        http.on(K11.releasesUrl(), r -> resp(200, "not json".getBytes()));
        VkDriverIndex.Listing l2 = index().refresh(true);
        assertEquals(VkDriverIndex.State.NO_NETWORK, l2.status.get(0).state);
        assertEquals(VkDriverIndex.State.FAILED, l2.status.get(1).state);
        assertEquals(33, l2.entries.size());   // a bad answer does not replace the cache
    }

    @Test
    public void noNetworkPermissionNoRequest() throws Exception {
        VkDriverIndex.Listing l = index(false).refresh(true);
        assertTrue(http.requests.isEmpty());
        for (VkDriverIndex.SourceStatus s : l.status)
            assertEquals(VkDriverIndex.State.NO_NETWORK, s.state);
        try {
            index(false).download(t30(), new FakeStore(), null, null);
            fail();
        } catch (IOException expected) {
        }
        assertTrue(http.requests.isEmpty());
    }

    @Test
    public void appAsksForInternet() throws Exception {
        // XRecomp's merged manifest asks for INTERNET (a game's own app does not: TitleApkTest); networkAllowed
        // follows the grant.
        android.app.Application app = RuntimeEnvironment.getApplication();
        String[] asked = app.getPackageManager().getPackageInfo(app.getPackageName(),
            android.content.pm.PackageManager.GET_PERMISSIONS).requestedPermissions;
        assertTrue(java.util.Arrays.asList(asked).contains(android.Manifest.permission.INTERNET));
        org.robolectric.Shadows.shadowOf(app).denyPermissions(android.Manifest.permission.INTERNET);
        assertFalse(VkDriverIndex.networkAllowed(app));
        org.robolectric.Shadows.shadowOf(app).grantPermissions(android.Manifest.permission.INTERNET);
        assertTrue(VkDriverIndex.networkAllowed(app));
    }

    // ---- the download ------------------------------------------------------------------------------------------

    static final class FakeStore implements VkDriverIndex.Store {
        File gotFile;
        byte[] gotBytes;
        String gotSha;
        VkDriverIndex.Entry gotFrom;
        IOException refuse;

        @Override
        public Set<String> installedSha256() {
            return gotSha == null ? Collections.emptySet() : Collections.singleton(gotSha);
        }

        @Override
        public void install(File zip, String expectedSha256, VkDriverIndex.Entry from) throws IOException {
            gotFile = zip;
            gotBytes = Files.readAllBytes(zip.toPath());
            gotSha = expectedSha256;
            gotFrom = from;
            if (refuse != null)
                throw refuse;
        }
    }

    static String sha(byte[] b) throws Exception {
        return VkDriverIndex.hex(MessageDigest.getInstance("SHA-256").digest(b));
    }

    static final byte[] PKG = "PK\3\4 not really a driver, just bytes".getBytes(StandardCharsets.US_ASCII);
    static final String CDN = "https://release-assets.githubusercontent.com/github-production-release-asset/1/x?sig=y";

    VkDriverIndex.Entry t30() {
        return entryFor(PKG.length, T30_SHA);
    }

    VkDriverIndex.Entry entryFor(long size, String sha) {
        return new VkDriverIndex.Entry(PURPLE.id(), "vturnip_mrpurple_T30-toasted.adpkg", "", false, 1,
                                       "turnip_mrpurple_T30-toasted.adpkg.zip", size, sha,
                                       dl(PURPLE, "vturnip_mrpurple_T30-toasted.adpkg",
                                          "turnip_mrpurple_T30-toasted.adpkg.zip"));
    }

    void serveDownload(VkDriverIndex.Entry e, byte[] body) {
        http.on(e.url, r -> resp(302, new byte[0], "Location", CDN));
        http.on(CDN, r -> resp(200, body, "Content-Length", "" + body.length));
    }

    @Test
    public void downloadVerifiedThenInstalled() throws Exception {
        VkDriverIndex.Entry e = entryFor(PKG.length, sha(PKG));
        serveDownload(e, PKG);
        FakeStore store = new FakeStore();
        List<long[]> progress = new ArrayList<>();
        VkDriverIndex idx = index();
        String got = idx.download(e, store, (done, total) -> progress.add(new long[] { done, total }), () -> false);
        assertEquals(sha(PKG), got);
        assertEquals(sha(PKG), store.gotSha);
        assertArrayEquals(PKG, store.gotBytes);
        assertEquals(e, store.gotFrom);
        assertEquals(idx.partFile(e), store.gotFile);
        assertEquals(cacheDir, store.gotFile.getParentFile());
        assertFalse("temporary file deleted", store.gotFile.exists());
        assertEquals(2, http.requests.size());
        assertEquals("XRecomp", http.requests.get(1).headers.get("User-Agent"));
        long[] last = progress.get(progress.size() - 1);
        assertEquals(PKG.length, last[0]);
        assertEquals(PKG.length, last[1]);
        assertTrue(e.installedIn(store.installedSha256()));
    }

    @Test
    public void downloadWithoutDigestPassesTheComputedSha() throws Exception {
        VkDriverIndex.Entry e = entryFor(PKG.length, null);
        serveDownload(e, PKG);
        FakeStore store = new FakeStore();
        index().download(e, store, null, null);
        assertEquals(sha(PKG), store.gotSha);
    }

    void expectFailure(VkDriverIndex.Entry e, VkDriverIndex.DownloadException.Reason reason, FakeStore store,
                       java.util.function.BooleanSupplier cancelled) throws Exception {
        VkDriverIndex idx = index();
        try {
            idx.download(e, store, null, cancelled);
            fail("expected " + reason);
        } catch (VkDriverIndex.DownloadException x) {
            assertEquals(x.getMessage(), reason, x.reason);
        }
        assertFalse("temporary file deleted", idx.partFile(e).exists());
        assertEquals(0, cacheDir.list().length);
    }

    @Test
    public void downloadDigestMismatchDeletedAndReported() throws Exception {
        VkDriverIndex.Entry e = t30();   // T30's digest, other bytes of the same size
        serveDownload(e, PKG);
        FakeStore store = new FakeStore();
        expectFailure(e, VkDriverIndex.DownloadException.Reason.SHA256, store, null);
        assertNull("not handed to the store", store.gotFile);
    }

    @Test
    public void downloadSizeMismatch() throws Exception {
        VkDriverIndex.Entry e = entryFor(PKG.length + 1, sha(PKG));
        serveDownload(e, PKG);   // Content-Length says PKG.length
        expectFailure(e, VkDriverIndex.DownloadException.Reason.SIZE, new FakeStore(), null);
        // Without Content-Length: more bytes than the release says.
        VkDriverIndex.Entry small = entryFor(4, null);
        http.on(small.url, r -> resp(200, PKG));
        expectFailure(small, VkDriverIndex.DownloadException.Reason.SIZE, new FakeStore(), null);
    }

    @Test
    public void downloadRedirectsCheckedAndCancel() throws Exception {
        VkDriverIndex.Entry e = entryFor(PKG.length, sha(PKG));
        http.on(e.url, r -> resp(302, new byte[0], "Location", "https://evil.example/x.zip"));
        expectFailure(e, VkDriverIndex.DownloadException.Reason.BAD_URL, new FakeStore(), null);
        http.on(e.url, r -> resp(302, new byte[0], "Location", "http://objects.githubusercontent.com/x.zip"));
        expectFailure(e, VkDriverIndex.DownloadException.Reason.BAD_URL, new FakeStore(), null);
        for (int i = 0; i <= VkDriverIndex.MAX_REDIRECTS; i++)
            http.on(e.url, r -> resp(302, new byte[0], "Location", e.url));
        expectFailure(e, VkDriverIndex.DownloadException.Reason.BAD_URL, new FakeStore(), null);
        http.on(e.url, r -> resp(404, new byte[0]));
        expectFailure(e, VkDriverIndex.DownloadException.Reason.HTTP, new FakeStore(), null);
        serveDownload(e, PKG);
        int[] polls = { 0 };
        expectFailure(e, VkDriverIndex.DownloadException.Reason.CANCELLED, new FakeStore(), () -> ++polls[0] > 2);
        // An entry that is not a listed driver is refused before any request.
        int n = http.requests.size();
        VkDriverIndex.Entry other = new VkDriverIndex.Entry(PURPLE.id(), "v", "", false, 1, "x.zip", 1, null,
                                                            "https://example.com/x.zip");
        expectFailure(other, VkDriverIndex.DownloadException.Reason.BAD_URL, new FakeStore(), null);
        assertEquals(n, http.requests.size());
    }

    @Test
    public void storeRefusalPropagatesAndCleansUp() throws Exception {
        VkDriverIndex.Entry e = entryFor(PKG.length, sha(PKG));
        serveDownload(e, PKG);
        FakeStore store = new FakeStore();
        store.refuse = new IOException("meta.json missing");
        VkDriverIndex idx = index();
        try {
            idx.download(e, store, null, null);
            fail();
        } catch (IOException x) {
            assertEquals("meta.json missing", x.getMessage());
        }
        assertFalse(idx.partFile(e).exists());
    }
}
