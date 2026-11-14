package fr.adserver.xrecomp;

import android.content.Context;
import android.content.pm.PackageManager;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.Closeable;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.time.format.DateTimeParseException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.BooleanSupplier;
import java.util.function.LongSupplier;
import java.util.regex.Pattern;

import javax.net.ssl.HttpsURLConnection;

/**
 * The list of Vulkan drivers (Mesa Turnip) one can download, and the download itself (docs/android/app-shell.md
 * « Vulkan drivers »). No driver is ever part of the app: this class only reads the public GitHub Releases of the
 * repositories in {@link #SOURCES} and fetches the asset the player picks.
 *
 * - Sources: the unauthenticated GitHub Releases API, one request per source
 *   (GET /repos/<owner>/<repo>/releases?per_page=30, User-Agent « XRecomp », nothing about the player or the device).
 * - Network use only when the Drivers screen asks ({@link #refresh}, {@link #download}): nothing in the constructor,
 *   in {@link #cached}, or in the background. A source's list is asked again only when the copy is older than
 *   {@link #TTL_MS} (or on the player's Refresh), with If-None-Match: a 304 keeps the copy (and does not count against
 *   GitHub's limit of 60 requests an hour per address).
 * - Offline, limit reached, an error: the cached list, with a {@link State} per source for the screen's note; never an
 *   exception to the UI.
 * - Filter ({@link #accept}): .zip assets of published (non-draft) releases, on github.com under the source's own
 *   releases; for a source with {@code turnipOnly} the asset's name must contain « turnip »; an asset or a release
 *   naming « qualcomm » is never listed (K11MCH1/AdrenoToolsDrivers also hosts Qualcomm's proprietary drivers). The
 *   same file (sha256) in several releases is listed once, in the newest. The cache keeps every .zip asset and filters
 *   when read, so a new filter in an update applies to a list cached before it.
 * - Download ({@link #download}): https only, github.com then a redirect to *.githubusercontent.com (at most
 *   {@link #MAX_REDIRECTS}), to getCacheDir()/vkd-<asset>.part, size and sha256 checked against the release's
 *   {@code size} and {@code digest} (when GitHub gives one), then handed to the driver store ({@link Store}); the
 *   temporary file is deleted in every case.
 *
 * The HTTP layer is the small {@link Http} interface ({@link UrlHttp} on the device), so the tests feed it fixtures.
 * Every network method blocks: call it off the UI thread (the launcher's worker).
 */
final class VkDriverIndex {
    /** A source: a GitHub repository's releases. */
    static final class Source {
        final String owner, repo;
        /** Only assets whose name contains « turnip » (the repository also hosts other drivers). */
        final boolean turnipOnly;

        Source(String owner, String repo, boolean turnipOnly) {
            this.owner = owner;
            this.repo = repo;
            this.turnipOnly = turnipOnly;
        }

        /** « owner/repo ». */
        String id() {
            return owner + "/" + repo;
        }

        String releasesUrl() {
            return API + "/repos/" + owner + "/" + repo + "/releases?per_page=" + PER_PAGE;
        }

        /** The cache file's base name (owner_repo; GitHub names are [A-Za-z0-9._-]). */
        String cacheName() {
            return safeName(owner) + "_" + safeName(repo);
        }
    }

    /** The sources, in the order the screen lists them. Another repository is one line here. */
    static final List<Source> SOURCES = Collections.unmodifiableList(Arrays.asList(
        new Source("MrPurple666", "purple-turnip", false),
        new Source("K11MCH1", "AdrenoToolsDrivers", true)));

    static final String API = "https://api.github.com";
    static final String USER_AGENT = "XRecomp";
    static final int PER_PAGE = 30;
    /** A source's list is asked again after this. */
    static final long TTL_MS = 24L * 60 * 60 * 1000;
    /** The largest releases page read (a page of 30 is ~15-60 KB). */
    static final int MAX_LIST_BYTES = 4 << 20;
    /** The largest download when the release gives no size (a Turnip package is ~2-4 MB). */
    static final long MAX_DOWNLOAD_BYTES = 256L << 20;
    static final int MAX_REDIRECTS = 5;
    /** The cache file's format; another value is ignored (asked again, no If-None-Match). */
    static final int CACHE_FORMAT = 1;

    private static final Pattern SHA256 = Pattern.compile("[0-9a-f]{64}");
    private static final Pattern ASSET_NAME = Pattern.compile("[A-Za-z0-9._+-]{1,200}");

    /** One downloadable package: a release asset. Immutable. */
    static final class Entry {
        /** The source, « owner/repo ». */
        final String source;
        /** The release's tag and title (the title may be empty). */
        final String tag, release;
        final boolean prerelease;
        /** published_at, epoch ms (0 when GitHub gave none). */
        final long publishedAt;
        /** The asset: file name, size in bytes (0 unknown), sha256 in lower-case hex or null, download URL. */
        final String assetName;
        final long size;
        final String sha256;
        final String url;

        Entry(String source, String tag, String release, boolean prerelease, long publishedAt, String assetName,
              long size, String sha256, String url) {
            this.source = source;
            this.tag = tag;
            this.release = release == null ? "" : release;
            this.prerelease = prerelease;
            this.publishedAt = publishedAt;
            this.assetName = assetName;
            this.size = size;
            this.sha256 = sha256;
            this.url = url;
        }

        /** A stable key for the screen: source, tag, asset. */
        String key() {
            return source + "/" + tag + "/" + assetName;
        }

        /** Whether the store has this package already (same sha256; unknown without a digest). */
        boolean installedIn(Set<String> installedSha256) {
            return sha256 != null && installedSha256 != null && installedSha256.contains(sha256);
        }

        @Override
        public String toString() {
            return key();
        }
    }

    /** What happened to a source's list in the last {@link #refresh} (or {@link #cached}). */
    enum State {
        /** Never fetched and nothing cached ({@link #cached} before any refresh). */
        NONE,
        /** The cached copy is younger than {@link #TTL_MS}: no request made. */
        CACHED,
        /** {@link #cached} only: the copy is older than {@link #TTL_MS}; the next {@link #refresh} asks again. */
        STALE,
        /** Fetched now (200). */
        UPDATED,
        /** Asked now, unchanged (304): the copy is kept and counts as fresh. */
        NOT_MODIFIED,
        /** No connection (DNS, connect, timeout, TLS): the cached copy, if any. */
        OFFLINE,
        /** GitHub's hourly limit reached (403/429): the cached copy; {@link SourceStatus#resetAt} says when to retry. */
        RATE_LIMITED,
        /** Another HTTP status or an unreadable answer: the cached copy. */
        FAILED,
        /** The app may not use the network (a game's own app has no INTERNET permission): no request. */
        NO_NETWORK
    }

    static final class SourceStatus {
        final Source source;
        final State state;
        /** When the list shown was fetched (epoch ms; 0: nothing cached). */
        final long fetchedAt;
        /** RATE_LIMITED: when GitHub's limit resets (epoch ms; 0 unknown). */
        final long resetAt;
        /** FAILED: the HTTP status (0 when not an HTTP status), and a short detail for the log. */
        final int httpCode;
        final String detail;

        SourceStatus(Source source, State state, long fetchedAt, long resetAt, int httpCode, String detail) {
            this.source = source;
            this.state = state;
            this.fetchedAt = fetchedAt;
            this.resetAt = resetAt;
            this.httpCode = httpCode;
            this.detail = detail == null ? "" : detail;
        }

        /** Whether the list shown is not the latest (fetched before and not confirmed now). */
        boolean stale() {
            return state != State.UPDATED && state != State.NOT_MODIFIED && state != State.CACHED;
        }
    }

    /** The screen's list: every source's entries (source order, newest first), and a status per source. */
    static final class Listing {
        final List<Entry> entries;
        final List<SourceStatus> status;

        Listing(List<Entry> entries, List<SourceStatus> status) {
            this.entries = Collections.unmodifiableList(entries);
            this.status = Collections.unmodifiableList(status);
        }

        /** The entries the store does not have yet (the installed ones are listed with the installed drivers). */
        List<Entry> notInstalled(Set<String> installedSha256) {
            List<Entry> out = new ArrayList<>();
            for (Entry e : entries) {
                if (!e.installedIn(installedSha256))
                    out.add(e);
            }
            return out;
        }

        /** The oldest fetch time of the sources shown (0 when one has none): « list from <date> ». */
        long oldestFetch() {
            long t = Long.MAX_VALUE;
            for (SourceStatus s : status)
                t = Math.min(t, s.fetchedAt);
            return t == Long.MAX_VALUE ? 0 : t;
        }
    }

    // ---- the driver store (VkDrivers) ----------------------------------------------------------------------------

    /**
     * The driver store as this class needs it (VkDrivers adapts to it). install is given a verified download: a zip
     * whose sha256 is expectedSha256 (computed while downloading, and equal to the release's digest when GitHub gave
     * one) and the entry it came from (source, tag, asset, size, for origin.json). It validates and keeps the package,
     * or throws. The file is deleted by the caller afterwards.
     */
    interface Store {
        Set<String> installedSha256();

        void install(File zip, String expectedSha256, Entry from) throws IOException;
    }

    /** Download progress: bytes so far and the total (-1 when unknown). Called on the downloading thread. */
    interface Progress {
        void onProgress(long done, long total);
    }

    /** Why a download stopped (other than a plain network IOException). */
    static final class DownloadException extends IOException {
        enum Reason { BAD_URL, HTTP, SIZE, SHA256, CANCELLED }

        final Reason reason;

        DownloadException(Reason reason, String message) {
            super(message);
            this.reason = reason;
        }
    }

    // ---- HTTP --------------------------------------------------------------------------------------------------

    /** One GET, redirects not followed. */
    interface Http {
        Response get(String url, Map<String, String> headers) throws IOException;
    }

    interface Response extends Closeable {
        int code();

        /** A header (name case-insensitive), or null. */
        String header(String name);

        /** Content-Length, or -1. */
        long length();

        /** The body (the error body for a status >= 400). */
        InputStream body() throws IOException;
    }

    /** {@link Http} with HttpsURLConnection: https only, no redirects followed, no HTTP cache, timeouts. */
    static final class UrlHttp implements Http {
        static final int CONNECT_TIMEOUT_MS = 15000, READ_TIMEOUT_MS = 30000;

        @Override
        public Response get(String url, Map<String, String> headers) throws IOException {
            URL u = new URL(url);
            if (!"https".equals(u.getProtocol()))
                throw new IOException("not https: " + url);
            HttpsURLConnection c = (HttpsURLConnection)u.openConnection();
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setConnectTimeout(CONNECT_TIMEOUT_MS);
            c.setReadTimeout(READ_TIMEOUT_MS);
            for (Map.Entry<String, String> h : headers.entrySet())
                c.setRequestProperty(h.getKey(), h.getValue());
            int code = c.getResponseCode();
            return new Response() {
                @Override
                public int code() {
                    return code;
                }

                @Override
                public String header(String name) {
                    return c.getHeaderField(name);
                }

                @Override
                public long length() {
                    return c.getContentLengthLong();
                }

                @Override
                public InputStream body() throws IOException {
                    InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
                    return in != null ? in : new java.io.ByteArrayInputStream(new byte[0]);
                }

                @Override
                public void close() {
                    c.disconnect();
                }
            };
        }
    }

    // ---- the index ---------------------------------------------------------------------------------------------

    private final File indexDir, tmpDir;
    private final Http http;
    private final LongSupplier now;
    private final boolean networkAllowed;
    private final List<Source> sources;

    VkDriverIndex(File indexDir, File tmpDir, Http http, LongSupplier now, boolean networkAllowed,
                  List<Source> sources) {
        this.indexDir = indexDir;
        this.tmpDir = tmpDir;
        this.http = http;
        this.now = now;
        this.networkAllowed = networkAllowed;
        this.sources = sources;
    }

    /** The device's index: files/vk_drivers/index/, downloads in getCacheDir(), the real network when allowed. */
    static VkDriverIndex forContext(Context c) {
        return new VkDriverIndex(new File(new File(c.getFilesDir(), "vk_drivers"), "index"), c.getCacheDir(),
                                 new UrlHttp(), System::currentTimeMillis, networkAllowed(c), SOURCES);
    }

    /** Whether this app may use the network: XRecomp has INTERNET, a game's own app does not (TitleApk drops it). */
    static boolean networkAllowed(Context c) {
        return c.checkSelfPermission(android.Manifest.permission.INTERNET) == PackageManager.PERMISSION_GRANTED;
    }

    /** The cached list only: no request, ever. Fast (a few small files); fine to call when the screen opens. */
    Listing cached() {
        List<Entry> entries = new ArrayList<>();
        List<SourceStatus> status = new ArrayList<>();
        for (Source s : sources) {
            Cache c = readCache(s);
            State st = c == null || c.fetchedAt == 0 ? State.NONE
                       : networkAllowed && now.getAsLong() < c.resetAt ? State.RATE_LIMITED
                       : fresh(c) ? State.CACHED : State.STALE;
            if (!networkAllowed && st != State.NONE)
                st = State.NO_NETWORK;
            if (c != null)
                entries.addAll(filter(s, c.assets));
            status.add(new SourceStatus(s, st, c == null ? 0 : c.fetchedAt, c == null ? 0 : c.resetAt, 0, null));
        }
        return new Listing(dedupe(entries), status);
    }

    /**
     * The list, asked again for each source whose copy is older than {@link #TTL_MS} (every source when force: the
     * player's Refresh). Blocks (network): off the UI thread. Never throws: a failed source keeps its cached entries
     * and says why in its status.
     */
    synchronized Listing refresh(boolean force) {
        List<Entry> entries = new ArrayList<>();
        List<SourceStatus> status = new ArrayList<>();
        for (Source s : sources) {
            Cache c = readCache(s);
            SourceStatus st = refreshOne(s, c, force);
            Cache after = readCache(s);
            if (after != null)
                entries.addAll(filter(s, after.assets));
            status.add(st);
        }
        return new Listing(dedupe(entries), status);
    }

    private SourceStatus refreshOne(Source s, Cache c, boolean force) {
        long t = now.getAsLong();
        long fetchedAt = c == null ? 0 : c.fetchedAt;
        if (!networkAllowed)
            return new SourceStatus(s, State.NO_NETWORK, fetchedAt, 0, 0, null);
        if (!force && c != null && fresh(c))
            return new SourceStatus(s, State.CACHED, fetchedAt, 0, 0, null);
        if (!force && c != null && t < c.resetAt)
            return new SourceStatus(s, State.RATE_LIMITED, fetchedAt, c.resetAt, 0, null);
        Map<String, String> h = new LinkedHashMap<>();
        h.put("User-Agent", USER_AGENT);
        h.put("Accept", "application/vnd.github+json");
        h.put("X-GitHub-Api-Version", "2022-11-28");
        boolean conditional = c != null && c.etag != null && !c.etag.isEmpty() && c.fetchedAt != 0;
        if (conditional)
            h.put("If-None-Match", c.etag);
        try (Response r = http.get(s.releasesUrl(), h)) {
            int code = r.code();
            if (code == 200) {
                byte[] body = readAll(r.body(), MAX_LIST_BYTES);
                List<Entry> assets = parseReleases(s, new String(body, StandardCharsets.UTF_8));
                writeCache(s, new Cache(r.header("ETag"), t, 0, assets));
                return new SourceStatus(s, State.UPDATED, t, 0, code, null);
            }
            if (code == 304 && conditional) {
                writeCache(s, new Cache(c.etag, t, 0, c.assets));
                return new SourceStatus(s, State.NOT_MODIFIED, t, 0, code, null);
            }
            long reset = rateLimitReset(r, t);
            if (reset >= 0) {
                writeCache(s, c == null ? new Cache(null, 0, reset, new ArrayList<>())
                                        : new Cache(c.etag, c.fetchedAt, reset, c.assets));
                return new SourceStatus(s, State.RATE_LIMITED, fetchedAt, reset, code, null);
            }
            return new SourceStatus(s, State.FAILED, fetchedAt, 0, code, "HTTP " + code);
        } catch (JSONException e) {
            return new SourceStatus(s, State.FAILED, fetchedAt, 0, 0, "bad JSON: " + e.getMessage());
        } catch (SecurityException e) {
            return new SourceStatus(s, State.NO_NETWORK, fetchedAt, 0, 0, String.valueOf(e.getMessage()));
        } catch (IOException | RuntimeException e) {
            return new SourceStatus(s, State.OFFLINE, fetchedAt, 0, 0, e.getClass().getSimpleName() + ": "
                                                                       + e.getMessage());
        }
    }

    private boolean fresh(Cache c) {
        long t = now.getAsLong();
        return c.fetchedAt != 0 && c.fetchedAt <= t && t - c.fetchedAt < TTL_MS;
    }

    /**
     * GitHub's « limit reached » answer (403 or 429 with x-ratelimit-remaining 0, or a Retry-After): when it resets
     * (epoch ms, 0 unknown); -1 when the answer is not that.
     */
    static long rateLimitReset(Response r, long now) {
        int code = r.code();
        if (code != 403 && code != 429)
            return -1;
        String remaining = r.header("x-ratelimit-remaining");
        String retryAfter = r.header("Retry-After");
        boolean limited = "0".equals(remaining == null ? null : remaining.trim()) || retryAfter != null
                          || code == 429;
        if (!limited)
            return -1;
        long reset = parseLong(r.header("x-ratelimit-reset"), 0);
        if (reset > 0)
            return reset * 1000;
        long after = parseLong(retryAfter, 0);
        return after > 0 ? now + after * 1000 : 0;
    }

    // ---- parsing and the filter --------------------------------------------------------------------------------

    /**
     * Every .zip asset of the published releases in a GitHub releases page (unfiltered but for drafts and assets
     * that cannot be downloaded at all); {@link #filter} applies the source's rules.
     */
    static List<Entry> parseReleases(Source s, String json) throws JSONException {
        JSONArray releases = new JSONArray(json);
        List<Entry> out = new ArrayList<>();
        for (int i = 0; i < releases.length(); i++) {
            JSONObject r = releases.optJSONObject(i);
            if (r == null || r.optBoolean("draft", false))
                continue;
            String tag = r.optString("tag_name", "");
            if (tag.isEmpty())
                continue;
            String name = r.isNull("name") ? "" : r.optString("name", "").trim();
            boolean pre = r.optBoolean("prerelease", false);
            long published = parseTime(r.isNull("published_at") ? null : r.optString("published_at", null));
            JSONArray assets = r.optJSONArray("assets");
            if (assets == null)
                continue;
            for (int j = 0; j < assets.length(); j++) {
                JSONObject a = assets.optJSONObject(j);
                if (a == null)
                    continue;
                String an = a.optString("name", "");
                String url = a.isNull("browser_download_url") ? "" : a.optString("browser_download_url", "");
                if (!an.toLowerCase(Locale.ROOT).endsWith(".zip") || url.isEmpty())
                    continue;
                out.add(new Entry(s.id(), tag, name, pre, published, an, Math.max(0, a.optLong("size", 0)),
                                  parseDigest(a.isNull("digest") ? null : a.optString("digest", null)), url));
            }
        }
        return out;
    }

    /** « sha256:<64 hex> » to the lower-case hex, else null (no digest, or another algorithm). */
    static String parseDigest(String digest) {
        if (digest == null || !digest.regionMatches(true, 0, "sha256:", 0, 7))
            return null;
        String hex = digest.substring(7).trim().toLowerCase(Locale.ROOT);
        return SHA256.matcher(hex).matches() ? hex : null;
    }

    static long parseTime(String iso) {
        if (iso == null || iso.isEmpty())
            return 0;
        try {
            return Instant.parse(iso).toEpochMilli();
        } catch (DateTimeParseException e) {
            return 0;
        }
    }

    /** Whether an asset of this source is listed (see the class comment). */
    static boolean accept(Source s, Entry e) {
        String n = e.assetName.toLowerCase(Locale.ROOT);
        if (!ASSET_NAME.matcher(e.assetName).matches() || !n.endsWith(".zip") || n.equals(".zip"))
            return false;
        if (n.contains("qualcomm") || e.release.toLowerCase(Locale.ROOT).contains("qualcomm")
            || e.tag.toLowerCase(Locale.ROOT).contains("qualcomm"))
            return false;
        if (s.turnipOnly && !n.contains("turnip"))
            return false;
        if (!s.id().equals(e.source))
            return false;
        return releaseAssetUrl(s, e.url);
    }

    /** https://github.com/<owner>/<repo>/releases/download/... (the source's own releases). */
    static boolean releaseAssetUrl(Source s, String url) {
        try {
            URI u = new URI(url);
            String path = u.getRawPath();
            return "https".equals(u.getScheme()) && "github.com".equalsIgnoreCase(u.getHost()) && u.getPort() == -1
                   && u.getRawUserInfo() == null && path != null
                   && path.toLowerCase(Locale.ROOT).startsWith(("/" + s.owner + "/" + s.repo + "/releases/download/")
                                                               .toLowerCase(Locale.ROOT))
                   && !path.contains("/../") && !path.contains("%2f") && !path.contains("%2F");
        } catch (URISyntaxException e) {
            return false;
        }
    }

    /** A redirect target of a download: https on github.com or a *.githubusercontent.com host. */
    static boolean downloadHost(URI u) {
        String host = u.getHost() == null ? "" : u.getHost().toLowerCase(Locale.ROOT);
        return "https".equals(u.getScheme()) && u.getRawUserInfo() == null
               && (u.getPort() == -1 || u.getPort() == 443)
               && (host.equals("github.com") || host.endsWith(".githubusercontent.com"));
    }

    static List<Entry> filter(Source s, List<Entry> assets) {
        List<Entry> out = new ArrayList<>();
        for (Entry e : assets) {
            if (accept(s, e))
                out.add(e);
        }
        out.sort((a, b) -> Long.compare(b.publishedAt, a.publishedAt));
        return out;
    }

    /** The same file (sha256) listed once: its first place (its source's newest release). */
    static List<Entry> dedupe(List<Entry> entries) {
        Set<String> seen = new HashSet<>();
        List<Entry> out = new ArrayList<>();
        for (Entry e : entries) {
            if (e.sha256 == null || seen.add(e.sha256))
                out.add(e);
        }
        return out;
    }

    // ---- the cache ---------------------------------------------------------------------------------------------

    private static final class Cache {
        final String etag;
        final long fetchedAt, resetAt;
        final List<Entry> assets;

        Cache(String etag, long fetchedAt, long resetAt, List<Entry> assets) {
            this.etag = etag;
            this.fetchedAt = fetchedAt;
            this.resetAt = resetAt;
            this.assets = assets;
        }
    }

    File cacheFile(Source s) {
        return new File(indexDir, s.cacheName() + ".json");
    }

    private Cache readCache(Source s) {
        File f = cacheFile(s);
        if (!f.isFile() || f.length() > MAX_LIST_BYTES)
            return null;
        try {
            JSONObject o = new JSONObject(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
            if (o.optInt("format", 0) != CACHE_FORMAT || !s.id().equals(o.optString("source", "")))
                return null;
            List<Entry> assets = new ArrayList<>();
            JSONArray a = o.optJSONArray("assets");
            for (int i = 0; a != null && i < a.length(); i++) {
                JSONObject e = a.getJSONObject(i);
                String sha = e.isNull("sha256") ? null : e.optString("sha256", null);
                assets.add(new Entry(s.id(), e.getString("tag"), e.optString("release", ""),
                                     e.optBoolean("prerelease", false), e.optLong("publishedAt", 0),
                                     e.getString("name"), e.optLong("size", 0),
                                     sha != null && SHA256.matcher(sha).matches() ? sha : null, e.getString("url")));
            }
            return new Cache(o.isNull("etag") ? null : o.optString("etag", null), o.optLong("fetchedAt", 0),
                             o.optLong("resetAt", 0), assets);
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    private void writeCache(Source s, Cache c) throws IOException {
        JSONObject o = new JSONObject();
        try {
            o.put("format", CACHE_FORMAT);
            o.put("source", s.id());
            if (c.etag != null)
                o.put("etag", c.etag);
            o.put("fetchedAt", c.fetchedAt);
            o.put("resetAt", c.resetAt);
            JSONArray a = new JSONArray();
            for (Entry e : c.assets) {
                JSONObject j = new JSONObject();
                j.put("tag", e.tag);
                j.put("release", e.release);
                j.put("prerelease", e.prerelease);
                j.put("publishedAt", e.publishedAt);
                j.put("name", e.assetName);
                j.put("size", e.size);
                if (e.sha256 != null)
                    j.put("sha256", e.sha256);
                j.put("url", e.url);
                a.put(j);
            }
            o.put("assets", a);
        } catch (JSONException e) {
            throw new IOException(e);
        }
        if (!indexDir.isDirectory() && !indexDir.mkdirs())
            throw new IOException("cannot create " + indexDir);
        File f = cacheFile(s);
        File tmp = new File(indexDir, f.getName() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(o.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (!tmp.renameTo(f)) {
            tmp.delete();
            throw new IOException("cannot write " + f);
        }
    }

    // ---- the download ------------------------------------------------------------------------------------------

    /** The temporary file of a download: getCacheDir()/vkd-<asset>.part. */
    File partFile(Entry e) {
        return new File(tmpDir, "vkd-" + safeName(e.assetName) + ".part");
    }

    /**
     * Downloads e, checks its size and sha256 (the release's digest when GitHub gave one), and hands it to the store.
     * Returns the package's sha256. Blocks (network): off the UI thread; cancelled is polled between chunks. The
     * temporary file is gone when this returns or throws.
     *
     * @throws DownloadException a refused URL or redirect, an HTTP status, a size or sha256 mismatch, cancelled
     * @throws IOException the network, the disk, or the store's refusal
     */
    String download(Entry e, Store store, Progress progress, BooleanSupplier cancelled) throws IOException {
        Source s = sourceOf(e);
        if (s == null || !accept(s, e))
            throw new DownloadException(DownloadException.Reason.BAD_URL, "not a listed driver: " + e.url);
        if (!networkAllowed)
            throw new IOException("no network permission");
        File part = partFile(e);
        try {
            String sha = fetch(e, part, progress, cancelled);
            if (cancelled != null && cancelled.getAsBoolean())
                throw new DownloadException(DownloadException.Reason.CANCELLED, "cancelled");
            store.install(part, sha, e);
            return sha;
        } finally {
            part.delete();
        }
    }

    private Source sourceOf(Entry e) {
        for (Source s : sources) {
            if (s.id().equals(e.source))
                return s;
        }
        return null;
    }

    private String fetch(Entry e, File part, Progress progress, BooleanSupplier cancelled) throws IOException {
        String url = e.url;
        Map<String, String> h = new LinkedHashMap<>();
        h.put("User-Agent", USER_AGENT);
        h.put("Accept", "application/octet-stream");
        for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
            if (cancelled != null && cancelled.getAsBoolean())
                throw new DownloadException(DownloadException.Reason.CANCELLED, "cancelled");
            try (Response r = http.get(url, h)) {
                int code = r.code();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    String loc = r.header("Location");
                    URI next;
                    try {
                        next = loc == null ? null : new URI(url).resolve(loc.trim());
                    } catch (URISyntaxException | IllegalArgumentException x) {
                        next = null;
                    }
                    if (next == null || !downloadHost(next))
                        throw new DownloadException(DownloadException.Reason.BAD_URL, "redirect refused: " + loc);
                    url = next.toString();
                    continue;
                }
                if (code != 200)
                    throw new DownloadException(DownloadException.Reason.HTTP, "HTTP " + code);
                return copy(e, r, part, progress, cancelled);
            }
        }
        throw new DownloadException(DownloadException.Reason.BAD_URL, "too many redirects");
    }

    private static String copy(Entry e, Response r, File part, Progress progress, BooleanSupplier cancelled)
            throws IOException {
        long length = r.length();
        if (e.size > 0 && length >= 0 && length != e.size)
            throw new DownloadException(DownloadException.Reason.SIZE,
                                        "size " + length + " instead of " + e.size);
        long limit = e.size > 0 ? e.size : MAX_DOWNLOAD_BYTES;
        long total = e.size > 0 ? e.size : length;
        File dir = part.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs())
            throw new IOException("cannot create " + dir);
        MessageDigest md = sha256();
        long done = 0;
        try (InputStream in = r.body(); OutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[64 * 1024];
            if (progress != null)
                progress.onProgress(0, total);
            for (int n; (n = in.read(buf)) > 0; ) {
                if (cancelled != null && cancelled.getAsBoolean())
                    throw new DownloadException(DownloadException.Reason.CANCELLED, "cancelled");
                done += n;
                if (done > limit)
                    throw new DownloadException(DownloadException.Reason.SIZE, "more than " + limit + " bytes");
                md.update(buf, 0, n);
                out.write(buf, 0, n);
                if (progress != null)
                    progress.onProgress(done, total);
            }
        }
        if (e.size > 0 && done != e.size)
            throw new DownloadException(DownloadException.Reason.SIZE, "size " + done + " instead of " + e.size);
        String sha = hex(md.digest());
        if (e.sha256 != null && !e.sha256.equals(sha))
            throw new DownloadException(DownloadException.Reason.SHA256,
                                        "sha256 " + sha + " instead of " + e.sha256);
        return sha;
    }

    // ---- helpers -----------------------------------------------------------------------------------------------

    static String safeName(String s) {
        String t = s == null ? "" : s.replaceAll("[^A-Za-z0-9._-]", "_");
        return t.isEmpty() || t.startsWith(".") ? "_" + t : t;
    }

    private static long parseLong(String s, long dflt) {
        if (s == null)
            return dflt;
        try {
            return Long.parseLong(s.trim());
        } catch (NumberFormatException e) {
            return dflt;
        }
    }

    private static byte[] readAll(InputStream in, int max) throws IOException {
        try (InputStream i = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16 * 1024];
            for (int n; (n = i.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                if (out.size() > max)
                    throw new IOException("answer larger than " + max + " bytes");
            }
            return out.toByteArray();
        }
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b)
            sb.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return sb.toString();
    }
}
