package fr.adserver.xrecomp;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.os.Build;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.BooleanSupplier;
import java.util.regex.Pattern;

/**
 * XRecomp's own updates (docs/android/app-shell.md « Updates »): the launcher asks the public GitHub releases of
 * {@link #OWNER}/{@link #REPO} whether a newer XRecomp APK is out, offers it (a modal at the launch, Settings ›
 * General › Updates), downloads it when the player says so and hands it to Android's installer.
 *
 * - Which releases: « xrecomp-v<version> » (the releases channel) and, on the test builds channel, the rolling
 *   « xrecomp-latest » prerelease too; the asset is the XRecomp APK of the release (« xrecomp-v<version>.apk »,
 *   « xrecomp-<commit>.apk »), on github.com under this repository's releases.
 * - Newer: every APK we publish has the version code « commit time / 60 » (release.yml, ci.yml), so a release's
 *   version code is its commit's committer time (GET /repos/…/git/commits/<target_commitish>, a small answer, kept
 *   in a cache since a commit never changes). An update is a release whose version code is above this app's.
 * - Network: one releases request per check (with If-None-Match: a 304 keeps the copy and does not count against
 *   GitHub's 60 requests an hour), plus one per commit not seen before. User-Agent « XRecomp », nothing about the
 *   player or the device. Automatically at most every {@link #AUTO_EVERY_MS}, at the launcher's start, when
 *   « Check for updates » is on (the default); never in a game's own app (no INTERNET).
 * - Download: only when the player chooses Install: https, github.com then *.githubusercontent.com, size and the
 *   release's sha256 digest checked (VkDriverIndex.fetch), then the APK itself: same package, a higher version code,
 *   signed by this app's own certificate ({@link #checkApk}). Android checks the signature again when it installs.
 * - Only an APK signed by the published key ({@link #CERT_SHA256}) offers updates: a build signed by another key
 *   (a local or debug build) could not install ours over itself.
 *
 * Every network method blocks: call it off the UI thread.
 */
final class AppUpdate {
    private AppUpdate() {}

    static final String OWNER = "MisterAlex95", REPO = "Xrecomp";
    static final String API = VkDriverIndex.API + "/repos/" + OWNER + "/" + REPO;
    /** The certificate of every XRecomp APK we publish (docs/releases/README.md), SHA-256 in lower-case hex. */
    static final String CERT_SHA256 = "bcf20446e6fe386f3cfaa1b875b25e997a770ae61ab24868dd4c9ce318f839fd";
    static final String LATEST_TAG = "xrecomp-latest", RELEASE_PREFIX = "xrecomp-v";
    /** The automatic check: at the launcher's start, at most this often. */
    static final long AUTO_EVERY_MS = 60L * 60 * 1000;
    static final int PER_PAGE = 20;
    static final int MAX_LIST_BYTES = 4 << 20, MAX_COMMIT_BYTES = 64 << 10;
    /** The largest APK accepted when GitHub gives no size (ours is ~50 MB). */
    static final long MAX_APK_BYTES = 1L << 30;
    static final String DIR = "update", PREFS = "app_update";

    private static final Pattern SHA1 = Pattern.compile("[0-9a-f]{40}");
    private static final Pattern RELEASE_TAG = Pattern.compile("xrecomp-v[0-9][0-9A-Za-z.+-]{0,40}");
    private static final Pattern LATEST_ASSET = Pattern.compile("xrecomp-[0-9a-f]{7,40}\\.apk");

    /** Where updates come from: tagged releases only, or the test builds (xrecomp-latest) too. */
    enum Channel { RELEASES, LATEST }

    /** One XRecomp APK on GitHub. Immutable. */
    static final class Release {
        final String tag, name;
        final boolean prerelease;
        final long publishedAt;
        /** The release's notes (Markdown), possibly empty. */
        final String notes;
        final String assetName;
        final long size;
        /** The asset's sha256 (lower-case hex) or null when GitHub gave none. */
        final String sha256;
        final String url;
        /** The full commit SHA the release was built from. */
        final String commit;
        /** The APK's version code (the commit's time / 60), 0 while unknown. */
        final long versionCode;

        Release(String tag, String name, boolean prerelease, long publishedAt, String notes, String assetName,
                long size, String sha256, String url, String commit, long versionCode) {
            this.tag = tag;
            this.name = name == null ? "" : name;
            this.prerelease = prerelease;
            this.publishedAt = publishedAt;
            this.notes = notes == null ? "" : notes;
            this.assetName = assetName;
            this.size = size;
            this.sha256 = sha256;
            this.url = url;
            this.commit = commit;
            this.versionCode = versionCode;
        }

        Release withVersionCode(long code) {
            return new Release(tag, name, prerelease, publishedAt, notes, assetName, size, sha256, url, commit, code);
        }

        /** A test build (xrecomp-latest) rather than a tagged release. */
        boolean testBuild() {
            return LATEST_TAG.equals(tag);
        }

        /** What the player reads: « 0.13.0 » for a release, the short commit for a test build. */
        String version() {
            if (tag.startsWith(RELEASE_PREFIX))
                return tag.substring(RELEASE_PREFIX.length());
            return commit.substring(0, 8);
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("tag", tag).put("name", name).put("prerelease", prerelease).put("publishedAt", publishedAt)
             .put("notes", notes).put("asset", assetName).put("size", size).put("url", url).put("commit", commit)
             .put("versionCode", versionCode);
            if (sha256 != null)
                o.put("sha256", sha256);
            return o;
        }

        static Release fromJson(JSONObject o) throws JSONException {
            return new Release(o.getString("tag"), o.optString("name", ""), o.optBoolean("prerelease", false),
                               o.optLong("publishedAt", 0), o.optString("notes", ""), o.getString("asset"),
                               o.optLong("size", 0), o.isNull("sha256") ? null : o.optString("sha256", null),
                               o.getString("url"), o.getString("commit"), o.optLong("versionCode", 0));
        }
    }

    /** What a check found. */
    enum State {
        /** Nothing asked yet. */
        NONE,
        UP_TO_DATE,
        AVAILABLE,
        /** No answer (no connection, a timeout). */
        OFFLINE,
        /** GitHub's limit of requests an hour is reached. */
        RATE_LIMITED,
        /** An HTTP error or an answer that could not be read. */
        FAILED
    }

    static final class Check {
        final State state;
        /** AVAILABLE: the update; UP_TO_DATE: the newest release seen (or null). */
        final Release release;
        /** When it was made (epoch ms). */
        final long at;
        final String detail;

        Check(State state, Release release, long at, String detail) {
            this.state = state;
            this.release = release;
            this.at = at;
            this.detail = detail == null ? "" : detail;
        }
    }

    // ---- This app ----------------------------------------------------------------------------------------------

    /** Why this app does not update itself, or null when it does. */
    enum Unsupported {
        /** A game's own app: XRecomp updates, the game's app is made again from it. */
        GAME_APP,
        /** Signed by another key than the published one (a local or debug build). */
        OTHER_SIGNER
    }

    /** Tests: stands in for {@link #unsupported} when set (its null answer: this app updates itself). */
    static java.util.function.Function<Context, Unsupported> unsupportedHook;

    /** Null when this app may update itself; else why not. */
    static Unsupported unsupported(Context c) {
        if (unsupportedHook != null)
            return unsupportedHook.apply(c);
        if (TitleApp.ownTitleId(c) != null || !VkDriverIndex.networkAllowed(c))
            return Unsupported.GAME_APP;
        return CERT_SHA256.equals(ownCertSha256(c)) ? null : Unsupported.OTHER_SIGNER;
    }

    /** The SHA-256 of this app's signing certificate (lower-case hex), or "" when it cannot be read. */
    static String ownCertSha256(Context c) {
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(),
                                                                  PackageManager.GET_SIGNING_CERTIFICATES);
            List<String> certs = certs(pi);
            return certs.size() == 1 ? certs.get(0) : "";
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            return "";
        }
    }

    /** The SHA-256 of each certificate an APK is signed with (its current signers). */
    static List<String> certs(PackageInfo pi) {
        List<String> out = new ArrayList<>();
        SigningInfo si = pi == null ? null : pi.signingInfo;
        Signature[] s = si == null ? null : si.getApkContentsSigners();
        for (int i = 0; s != null && i < s.length; i++)
            out.add(VkDriverIndex.hex(sha256().digest(s[i].toByteArray())));
        return out;
    }

    /** This app's version code (« commit time / 60 » for ours). */
    static long ownVersionCode(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).getLongVersionCode();
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            return 0;
        }
    }

    /** The channel when the player never chose one: a release build follows releases, a test build test builds. */
    static Channel defaultChannel(String ownVersionName) {
        return ownVersionName != null && ownVersionName.matches("[0-9]+\\.[0-9]+.*") ? Channel.RELEASES
                                                                                     : Channel.LATEST;
    }

    // ---- Preferences ---------------------------------------------------------------------------------------------

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** « Check for updates » (on by default). */
    static boolean autoOn(Context c) {
        return prefs(c).getBoolean("auto", true);
    }

    static boolean setAutoOn(Context c, boolean on) {
        return prefs(c).edit().putBoolean("auto", on).commit();
    }

    static Channel channel(Context c) {
        String s = prefs(c).getString("channel", null);
        if (s != null) {
            try {
                return Channel.valueOf(s);
            } catch (IllegalArgumentException ignored) {
            }
        }
        return defaultChannel(c.getString(R.string.xba_version));
    }

    static void setChannel(Context c, Channel ch) {
        prefs(c).edit().putString("channel", ch.name()).remove("skipped").apply();
    }

    /** The version code the player said « Skip this version » to (0: none). */
    static long skipped(Context c) {
        return prefs(c).getLong("skipped", 0);
    }

    static void skip(Context c, long versionCode) {
        prefs(c).edit().putLong("skipped", versionCode).apply();
    }

    /** Whether the automatic check is due (on, supported, and the last one older than AUTO_EVERY_MS). */
    static boolean autoDue(Context c, long now) {
        if (!autoOn(c) || unsupported(c) != null)
            return false;
        long last = prefs(c).getLong("checkedAt", 0);
        return last <= 0 || last > now || now - last >= AUTO_EVERY_MS;
    }

    /** The last check, as saved (state NONE when there was none); an update already installed is UP_TO_DATE. */
    static Check lastCheck(Context c) {
        SharedPreferences p = prefs(c);
        State st;
        try {
            st = State.valueOf(p.getString("state", State.NONE.name()));
        } catch (IllegalArgumentException e) {
            st = State.NONE;
        }
        Release r = null;
        String j = p.getString("release", null);
        if (j != null) {
            try {
                r = Release.fromJson(new JSONObject(j));
            } catch (JSONException ignored) {
            }
        }
        if (st == State.AVAILABLE && (r == null || r.versionCode <= ownVersionCode(c)))
            st = State.UP_TO_DATE;
        return new Check(st, r, p.getLong("checkedAt", 0), p.getString("detail", ""));
    }

    private static void saveCheck(Context c, Check k) {
        SharedPreferences.Editor e = prefs(c).edit().putString("state", k.state.name()).putLong("checkedAt", k.at)
                .putString("detail", k.detail);
        if (k.release != null) {
            try {
                e.putString("release", k.release.toJson().toString());
            } catch (JSONException ignored) {
            }
        } else if (k.state == State.UP_TO_DATE) {
            e.remove("release");
        }
        e.apply();
    }

    /**
     * Once per install: true when this app's version code went up since the launcher last ran (an update went in),
     * and the update's leftovers are deleted. False on the first run.
     */
    static boolean justUpdated(Context c) {
        long own = ownVersionCode(c), seen = prefs(c).getLong("seenCode", 0);
        if (seen == own)
            return false;
        prefs(c).edit().putLong("seenCode", own).apply();
        clean(c);
        return seen != 0 && own > seen;
    }

    static File dir(Context c) {
        return new File(c.getFilesDir(), DIR);
    }

    /** Deletes what a download left (the APK was copied into the installer's session). */
    static void clean(Context c) {
        File[] l = dir(c).listFiles();
        for (int i = 0; l != null && i < l.length; i++) {
            if (!l[i].getName().endsWith(".json"))
                //noinspection ResultOfMethodCallIgnored
                l[i].delete();
        }
    }

    // ---- The check ---------------------------------------------------------------------------------------------

    /** Checks now (blocks: network) and saves the outcome. Never throws. */
    static Check check(Context c, VkDriverIndex.Http http, long now) {
        Check k = check(http, dir(c), channel(c), ownVersionCode(c), now);
        Log.i(LauncherActivity.TAG, "launcher: update check (" + channel(c) + "): " + k.state
                                    + (k.release == null ? "" : " " + k.release.tag + " " + k.release.version()
                                                                + " code " + k.release.versionCode)
                                    + (k.detail.isEmpty() ? "" : " (" + k.detail + ")"));
        saveCheck(c, k);
        return k;
    }

    /** The check itself: the releases (cached in dir with their ETag), the newest APK of the channel, its code. */
    static Check check(VkDriverIndex.Http http, File dir, Channel channel, long ownCode, long now) {
        String json;
        try {
            json = releasesJson(http, dir);
        } catch (RateLimited e) {
            return new Check(State.RATE_LIMITED, null, now, e.getMessage());
        } catch (HttpStatus e) {
            return new Check(State.FAILED, null, now, e.getMessage());
        } catch (IOException | RuntimeException e) {
            return new Check(State.OFFLINE, null, now, e.getClass().getSimpleName() + ": " + e.getMessage());
        }
        try {
            List<Release> all = parseReleases(json);
            Release best = null;
            for (Release r : candidates(all, channel)) {
                long code = commitCode(http, dir, r.commit);
                if (code <= 0)
                    continue;
                Release rc = r.withVersionCode(code);
                if (best == null || rc.versionCode > best.versionCode
                    || (rc.versionCode == best.versionCode && best.testBuild() && !rc.testBuild()))
                    best = rc;
            }
            if (best == null)
                return new Check(State.UP_TO_DATE, null, now, "no XRecomp APK in the releases");
            return new Check(best.versionCode > ownCode ? State.AVAILABLE : State.UP_TO_DATE, best, now, null);
        } catch (RateLimited e) {
            return new Check(State.RATE_LIMITED, null, now, e.getMessage());
        } catch (HttpStatus | JSONException e) {
            return new Check(State.FAILED, null, now, e.getMessage());
        } catch (IOException | RuntimeException e) {
            return new Check(State.OFFLINE, null, now, e.getClass().getSimpleName() + ": " + e.getMessage());
        }
    }

    /** The releases the channel takes: the newest tagged release, and on LATEST the test build too. */
    static List<Release> candidates(List<Release> all, Channel channel) {
        List<Release> out = new ArrayList<>();
        Release newest = null, latest = null;
        for (Release r : all) {
            if (r.testBuild()) {
                if (latest == null)
                    latest = r;
            } else if (!r.prerelease && (newest == null || r.publishedAt > newest.publishedAt)) {
                newest = r;
            }
        }
        if (newest != null)
            out.add(newest);
        if (channel == Channel.LATEST && latest != null)
            out.add(latest);
        return out;
    }

    /** XRecomp APKs in a GitHub releases page: one per release (see the class comment), drafts left out. */
    static List<Release> parseReleases(String json) throws JSONException {
        JSONArray a = new JSONArray(json);
        List<Release> out = new ArrayList<>();
        for (int i = 0; i < a.length(); i++) {
            JSONObject r = a.optJSONObject(i);
            if (r == null || r.optBoolean("draft", false))
                continue;
            String tag = r.optString("tag_name", "");
            boolean latest = LATEST_TAG.equals(tag);
            if (!latest && !RELEASE_TAG.matcher(tag).matches())
                continue;
            String commit = r.optString("target_commitish", "").toLowerCase(Locale.ROOT);
            if (!SHA1.matcher(commit).matches())
                continue;
            JSONArray assets = r.optJSONArray("assets");
            for (int j = 0; assets != null && j < assets.length(); j++) {
                JSONObject x = assets.optJSONObject(j);
                String name = x == null ? "" : x.optString("name", "");
                boolean ours = latest ? LATEST_ASSET.matcher(name).matches() : name.equals(tag + ".apk");
                String url = x == null || x.isNull("browser_download_url") ? ""
                                                                           : x.optString("browser_download_url", "");
                if (!ours || !assetUrl(url, tag, name))
                    continue;
                out.add(new Release(tag, r.isNull("name") ? "" : r.optString("name", ""),
                                    r.optBoolean("prerelease", false),
                                    VkDriverIndex.parseTime(r.isNull("published_at") ? null
                                                                                     : r.optString("published_at")),
                                    r.isNull("body") ? "" : r.optString("body", ""), name,
                                    Math.max(0, x.optLong("size", 0)),
                                    VkDriverIndex.parseDigest(x.isNull("digest") ? null : x.optString("digest", null)),
                                    url, commit, 0));
                break;
            }
        }
        return out;
    }

    /** https://github.com/OWNER/REPO/releases/download/<tag>/<name>, exactly. */
    static boolean assetUrl(String url, String tag, String name) {
        return url.equals("https://github.com/" + OWNER + "/" + REPO + "/releases/download/" + tag + "/" + name);
    }

    private static final class RateLimited extends IOException {
        RateLimited(String m) {
            super(m);
        }
    }

    private static final class HttpStatus extends IOException {
        HttpStatus(String m) {
            super(m);
        }
    }

    private static Map<String, String> apiHeaders() {
        Map<String, String> h = new LinkedHashMap<>();
        h.put("User-Agent", VkDriverIndex.USER_AGENT);
        h.put("Accept", "application/vnd.github+json");
        h.put("X-GitHub-Api-Version", "2022-11-28");
        return h;
    }

    /** The releases page: asked with If-None-Match when a copy is kept; a 304 gives the copy. */
    private static String releasesJson(VkDriverIndex.Http http, File dir) throws IOException {
        File body = new File(dir, "releases.json"), etagFile = new File(dir, "releases.etag.json");
        String etag = null;
        if (body.isFile() && etagFile.isFile()) {
            try {
                etag = new JSONObject(read(etagFile, 4096)).optString("etag", null);
            } catch (JSONException | IOException ignored) {
            }
        }
        Map<String, String> h = apiHeaders();
        if (etag != null && !etag.isEmpty())
            h.put("If-None-Match", etag);
        try (VkDriverIndex.Response r = http.get(API + "/releases?per_page=" + PER_PAGE, h)) {
            int code = r.code();
            if (code == 304 && etag != null)
                return read(body, MAX_LIST_BYTES);
            if (code == 200) {
                String json = new String(readAll(r.body(), MAX_LIST_BYTES), StandardCharsets.UTF_8);
                String tag = r.header("ETag");
                write(dir, body, json);
                try {
                    JSONObject o = new JSONObject();
                    if (tag != null)
                        o.put("etag", tag);
                    write(dir, etagFile, o.toString());
                } catch (JSONException ignored) {
                }
                return json;
            }
            throw statusError(r, code);
        }
    }

    private static IOException statusError(VkDriverIndex.Response r, int code) {
        if (VkDriverIndex.rateLimitReset(r, System.currentTimeMillis()) >= 0)
            return new RateLimited("HTTP " + code + ": rate limit");
        return new HttpStatus("HTTP " + code);
    }

    /** A commit's committer time / 60 (our APKs' version code), from the cache or GitHub's git commits API. */
    static long commitCode(VkDriverIndex.Http http, File dir, String sha) throws IOException, JSONException {
        File cache = new File(dir, "commits.json");
        JSONObject codes = new JSONObject();
        if (cache.isFile()) {
            try {
                codes = new JSONObject(read(cache, MAX_LIST_BYTES));
            } catch (JSONException | IOException ignored) {
                codes = new JSONObject();
            }
        }
        long known = codes.optLong(sha, 0);
        if (known > 0)
            return known;
        try (VkDriverIndex.Response r = http.get(API + "/git/commits/" + sha, apiHeaders())) {
            int code = r.code();
            if (code != 200)
                throw statusError(r, code);
            JSONObject o = new JSONObject(new String(readAll(r.body(), MAX_COMMIT_BYTES), StandardCharsets.UTF_8));
            JSONObject committer = o.optJSONObject("committer");
            long t = VkDriverIndex.parseTime(committer == null ? null : committer.optString("date", null));
            if (t <= 0)
                return 0;
            long v = t / 1000 / 60;
            codes.put(sha, v);
            write(dir, cache, codes.toString());
            return v;
        }
    }

    // ---- The download and the install ----------------------------------------------------------------------------

    /** The APK checked before it goes to the installer; tests put a fake one here (Robolectric parses no APK). */
    interface ApkCheck {
        /** Null when apk may be installed over this app, else why not. */
        String check(Context c, File apk);
    }

    static ApkCheck apkCheck = AppUpdate::checkApk;

    /** Same package, a higher version code, signed by this app's certificate: null, else why not. */
    static String checkApk(Context c, File apk) {
        PackageInfo pi;
        try {
            pi = c.getPackageManager().getPackageArchiveInfo(apk.getPath(), PackageManager.GET_SIGNING_CERTIFICATES);
        } catch (RuntimeException e) {
            pi = null;
        }
        if (pi == null)
            return "not an APK";
        if (!c.getPackageName().equals(pi.packageName))
            return "package " + pi.packageName;
        if (pi.getLongVersionCode() <= ownVersionCode(c))
            return "version code " + pi.getLongVersionCode() + " is not newer";
        String own = ownCertSha256(c);
        List<String> theirs = certs(pi);
        if (own.isEmpty() || theirs.size() != 1 || !own.equals(theirs.get(0)))
            return "signed by another key";
        return null;
    }

    /** The download is refused: the file is not an update of this app (checkApk's reason). */
    static final class BadApk extends IOException {
        BadApk(String m) {
            super(m);
        }
    }

    /**
     * Downloads r into files/update/ (size and digest checked), then checks the APK. Returns the file. Blocks.
     *
     * @throws VkDriverIndex.DownloadException a refused URL, an HTTP status, a size or sha256 mismatch, cancelled
     * @throws BadApk the file is not an update of this app
     */
    static File download(Context c, VkDriverIndex.Http http, Release r, VkDriverIndex.Progress p,
                         BooleanSupplier cancelled) throws IOException {
        if (!assetUrl(r.url, r.tag, r.assetName))
            throw new VkDriverIndex.DownloadException(VkDriverIndex.DownloadException.Reason.BAD_URL, r.url);
        File d = dir(c);
        if (!d.isDirectory() && !d.mkdirs())
            throw new IOException("cannot create " + d);
        clean(c);
        File part = new File(d, "update.apk.part"), apk = new File(d, "update.apk");
        try {
            VkDriverIndex.fetch(http, r.url, r.size, r.sha256, MAX_APK_BYTES, part, p, cancelled);
            if (cancelled != null && cancelled.getAsBoolean())
                throw new VkDriverIndex.DownloadException(VkDriverIndex.DownloadException.Reason.CANCELLED,
                                                          "cancelled");
            String why = apkCheck.check(c, part);
            if (why != null)
                throw new BadApk(why);
            if (!part.renameTo(apk))
                throw new IOException("cannot write " + apk);
            return apk;
        } finally {
            //noinspection ResultOfMethodCallIgnored
            part.delete();
        }
    }

    /** Tests: stands in for {@link #install} (Robolectric's installer does not run sessions). */
    interface Installer {
        void install(Context c, File apk, String version) throws IOException;
    }

    static Installer installer = AppUpdate::install;

    /**
     * Hands apk to Android's installer as an update of this app (AppUpdateReceiver hears it). Android asks the player
     * to confirm, except from Android 12 when this app installed the version running (it is then its own installer
     * of record: UPDATE_PACKAGES_WITHOUT_USER_ACTION). Android ends this app's processes to replace it.
     */
    static void install(Context c, File apk, String version) throws IOException {
        PackageInstaller pi = c.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(c.getPackageName());
        params.setSize(apk.length());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        int id = pi.createSession(params);
        boolean committed = false;
        try (PackageInstaller.Session session = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; )
                    out.write(buf, 0, n);
                session.fsync(out);
            }
            Intent i = new Intent(c, AppUpdateReceiver.class).setAction(AppUpdateReceiver.ACTION)
                    .putExtra(AppUpdateReceiver.EXTRA_VERSION, version);
            PendingIntent pend = PendingIntent.getBroadcast(c, id, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pend.getIntentSender());
            committed = true;
            Log.i(LauncherActivity.TAG, "launcher: update " + version + " handed to the installer (session " + id + ")");
        } finally {
            if (!committed) {
                try {
                    pi.abandonSession(id);
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    // ---- The installer's outcome (AppUpdateReceiver) --------------------------------------------------------------

    static void saveOutcome(Context c, int status, String message, String version) {
        prefs(c).edit().putInt("outStatus", status).putString("outMessage", message == null ? "" : message)
                .putString("outVersion", version == null ? "" : version).putBoolean("outPending", true).apply();
    }

    /** The outcome not shown yet as {status, message, version}, cleared; null when there is none. */
    static String[] takeOutcome(Context c) {
        SharedPreferences p = prefs(c);
        if (!p.getBoolean("outPending", false))
            return null;
        p.edit().putBoolean("outPending", false).apply();
        return new String[] { String.valueOf(p.getInt("outStatus", PackageInstaller.STATUS_FAILURE)),
                              p.getString("outMessage", ""), p.getString("outVersion", "") };
    }

    // ---- helpers -------------------------------------------------------------------------------------------------

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    private static String read(File f, int max) throws IOException {
        if (f.length() > max)
            throw new IOException(f + " larger than " + max + " bytes");
        return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
    }

    private static void write(File dir, File f, String text) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs())
            throw new IOException("cannot create " + dir);
        File tmp = new File(dir, f.getName() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
        if (!tmp.renameTo(f)) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw new IOException("cannot write " + f);
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
}
