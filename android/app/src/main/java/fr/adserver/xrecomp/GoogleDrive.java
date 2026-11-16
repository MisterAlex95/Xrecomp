package fr.adserver.xrecomp;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import javax.net.ssl.HttpsURLConnection;

/**
 * Google sign-in and the app's hidden Drive folder, for cloud saves (CloudSaves; docs/android/app-shell.md « Cloud
 * saves »). No Play services: OAuth 2.0 for TV and limited-input devices (a code the player types at
 * google.com/device, on this device or a phone), which works the same in XRecomp, in a game's own app and on a
 * handheld without Google apps. Scopes: drive.appdata (the app's own hidden folder, nothing else of the player's
 * Drive; non-sensitive) and email (to say which account is connected).
 *
 * The client (a « TVs and Limited Input devices » OAuth client of the project's Google Cloud project) comes with the
 * build (-PxbaGoogleClientId / -PxbaGoogleClientSecret, see build.gradle): a build without it has no cloud saves.
 * Plain Java over {@link Http}, so the JVM tests answer for Google.
 */
final class GoogleDrive {
    static final String DEVICE_CODE_URL = "https://oauth2.googleapis.com/device/code";
    static final String TOKEN_URL = "https://oauth2.googleapis.com/token";
    static final String REVOKE_URL = "https://oauth2.googleapis.com/revoke";
    static final String FILES_URL = "https://www.googleapis.com/drive/v3/files";
    static final String UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3/files";
    static final String SCOPES = "email https://www.googleapis.com/auth/drive.appdata";
    static final String DEVICE_GRANT = "urn:ietf:params:oauth:grant-type:device_code";

    // ---- HTTP --------------------------------------------------------------------------------------------------

    interface Http {
        /** One request; body may be null. The reply's body is the error body for a status >= 400. */
        Reply send(String method, String url, Map<String, String> headers, byte[] body) throws IOException;
    }

    static final class Reply {
        final int code;
        final byte[] body;

        Reply(int code, byte[] body) {
            this.code = code;
            this.body = body == null ? new byte[0] : body;
        }

        String text() {
            return new String(body, StandardCharsets.UTF_8);
        }

        JSONObject json() throws IOException {
            try {
                return new JSONObject(text());
            } catch (JSONException e) {
                throw new IOException("not JSON (HTTP " + code + ")");
            }
        }
    }

    /** {@link Http} with HttpsURLConnection: https only, no redirects, no cache, timeouts; PATCH as an override. */
    static final class UrlHttp implements Http {
        final int connectMs, readMs;

        UrlHttp(int connectMs, int readMs) {
            this.connectMs = connectMs;
            this.readMs = readMs;
        }

        @Override
        public Reply send(String method, String url, Map<String, String> headers, byte[] body) throws IOException {
            URL u = new URL(url);
            if (!"https".equals(u.getProtocol()))
                throw new IOException("not https: " + url);
            HttpsURLConnection c = (HttpsURLConnection)u.openConnection();
            try {
                c.setInstanceFollowRedirects(false);
                c.setUseCaches(false);
                c.setConnectTimeout(connectMs);
                c.setReadTimeout(readMs);
                if ("PATCH".equals(method)) {   // HttpURLConnection has no PATCH; Google's APIs take the override
                    c.setRequestMethod("POST");
                    c.setRequestProperty("X-HTTP-Method-Override", "PATCH");
                } else {
                    c.setRequestMethod(method);
                }
                if (headers != null) {
                    for (Map.Entry<String, String> h : headers.entrySet())
                        c.setRequestProperty(h.getKey(), h.getValue());
                }
                if (body != null) {
                    c.setDoOutput(true);
                    c.setFixedLengthStreamingMode(body.length);
                    try (OutputStream out = c.getOutputStream()) {
                        out.write(body);
                    }
                }
                int code = c.getResponseCode();
                InputStream in = code >= 400 ? c.getErrorStream() : c.getInputStream();
                return new Reply(code, in == null ? null : readAll(in, 512L << 20));
            } finally {
                c.disconnect();
            }
        }
    }

    static byte[] readAll(InputStream in, long max) throws IOException {
        try (InputStream s = in) {
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = s.read(buf)) > 0) {
                b.write(buf, 0, n);
                if (b.size() > max)
                    throw new IOException("reply too large");
            }
            return b.toByteArray();
        }
    }

    private static String form(Map<String, String> m) {
        StringBuilder b = new StringBuilder();
        try {
            for (Map.Entry<String, String> e : m.entrySet()) {
                if (b.length() > 0)
                    b.append('&');
                b.append(URLEncoder.encode(e.getKey(), "UTF-8")).append('=')
                 .append(URLEncoder.encode(e.getValue(), "UTF-8"));
            }
        } catch (java.io.UnsupportedEncodingException e) {
            throw new IllegalStateException(e);
        }
        return b.toString();
    }

    private static Map<String, String> formHeaders() {
        Map<String, String> h = new HashMap<>();
        h.put("Content-Type", "application/x-www-form-urlencoded");
        return h;
    }

    // ---- Errors ------------------------------------------------------------------------------------------------

    /** Google said no: the account must connect again (revoked, expired refresh token, deleted client). */
    static final class AuthException extends IOException {
        AuthException(String m) {
            super(m);
        }
    }

    // ---- Sign-in (device flow) ---------------------------------------------------------------------------------

    final Http http;
    final String clientId, clientSecret;

    GoogleDrive(Http http, String clientId, String clientSecret) {
        this.http = http;
        this.clientId = clientId == null ? "" : clientId;
        this.clientSecret = clientSecret == null ? "" : clientSecret;
    }

    /** True when the build carries a client. */
    boolean configured() {
        return !clientId.isEmpty();
    }

    static final class DeviceCode {
        String deviceCode, userCode, verificationUrl;
        long expiresAtMs;
        int intervalS;
    }

    /** Step 1: the code the player types at verificationUrl. */
    DeviceCode deviceCode(long nowMs) throws IOException {
        Map<String, String> f = new LinkedHashMap<>();
        f.put("client_id", clientId);
        f.put("scope", SCOPES);
        Reply r = http.send("POST", DEVICE_CODE_URL, formHeaders(), form(f).getBytes(StandardCharsets.UTF_8));
        JSONObject o = r.json();
        if (r.code != 200)
            throw new IOException("device code: HTTP " + r.code + " " + o.optString("error", o.optString("error_code")));
        DeviceCode d = new DeviceCode();
        d.deviceCode = o.optString("device_code");
        d.userCode = o.optString("user_code");
        d.verificationUrl = o.optString("verification_url", o.optString("verification_uri"));
        d.expiresAtMs = nowMs + o.optLong("expires_in", 1800) * 1000;
        d.intervalS = Math.max(1, o.optInt("interval", 5));
        if (d.deviceCode.isEmpty() || d.userCode.isEmpty() || d.verificationUrl.isEmpty())
            throw new IOException("device code: incomplete reply");
        return d;
    }

    static final class Tokens {
        String access, refresh, email = "";
        long expiresAtMs;
    }

    enum Poll { PENDING, SLOW_DOWN, DENIED, EXPIRED, OK }

    static final class PollResult {
        Poll state;
        Tokens tokens;
    }

    /** Step 2, every interval seconds: pending until the player said yes (or no), then the tokens. */
    PollResult poll(DeviceCode d, long nowMs) throws IOException {
        PollResult p = new PollResult();
        if (nowMs >= d.expiresAtMs) {
            p.state = Poll.EXPIRED;
            return p;
        }
        Map<String, String> f = new LinkedHashMap<>();
        f.put("client_id", clientId);
        f.put("client_secret", clientSecret);
        f.put("device_code", d.deviceCode);
        f.put("grant_type", DEVICE_GRANT);
        Reply r = http.send("POST", TOKEN_URL, formHeaders(), form(f).getBytes(StandardCharsets.UTF_8));
        JSONObject o = r.json();
        if (r.code == 200) {
            p.state = Poll.OK;
            p.tokens = tokens(o, nowMs);
            if (p.tokens.refresh == null || p.tokens.refresh.isEmpty())
                throw new IOException("no refresh token");
            return p;
        }
        String err = o.optString("error");
        switch (err) {
        case "authorization_pending":
            p.state = Poll.PENDING;
            return p;
        case "slow_down":
            p.state = Poll.SLOW_DOWN;
            return p;
        case "access_denied":
            p.state = Poll.DENIED;
            return p;
        case "expired_token":
            p.state = Poll.EXPIRED;
            return p;
        default:
            throw new IOException("sign-in: HTTP " + r.code + " " + err);
        }
    }

    private static Tokens tokens(JSONObject o, long nowMs) {
        Tokens t = new Tokens();
        t.access = o.optString("access_token");
        t.refresh = o.optString("refresh_token", null);
        t.expiresAtMs = nowMs + Math.max(60, o.optLong("expires_in", 3600)) * 1000;
        t.email = emailOf(o.optString("id_token"));
        return t;
    }

    /** The email claim of an ID token (not verified: only shown to the player), or "". */
    static String emailOf(String idToken) {
        if (idToken == null)
            return "";
        String[] p = idToken.split("\\.");
        if (p.length < 2)
            return "";
        try {
            byte[] json = java.util.Base64.getUrlDecoder().decode(p[1]);
            return new JSONObject(new String(json, StandardCharsets.UTF_8)).optString("email");
        } catch (IllegalArgumentException | JSONException e) {
            return "";
        }
    }

    /** A new access token from the refresh token; AuthException when Google no longer takes it. */
    Tokens refresh(String refreshToken, long nowMs) throws IOException {
        Map<String, String> f = new LinkedHashMap<>();
        f.put("client_id", clientId);
        if (!clientSecret.isEmpty())
            f.put("client_secret", clientSecret);
        f.put("grant_type", "refresh_token");
        f.put("refresh_token", refreshToken);
        Reply r = http.send("POST", TOKEN_URL, formHeaders(), form(f).getBytes(StandardCharsets.UTF_8));
        JSONObject o = r.json();
        if (r.code == 200) {
            Tokens t = tokens(o, nowMs);
            t.refresh = refreshToken;
            return t;
        }
        String err = o.optString("error");
        if ("invalid_grant".equals(err) || "invalid_client".equals(err) || "unauthorized_client".equals(err))
            throw new AuthException(err);
        throw new IOException("refresh: HTTP " + r.code + " " + err);
    }

    /** Disconnect: Google forgets the grant (best effort: the account is forgotten here either way). */
    void revoke(String token) {
        try {
            Map<String, String> f = new LinkedHashMap<>();
            f.put("token", token);
            http.send("POST", REVOKE_URL, formHeaders(), form(f).getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    // ---- The app's folder ----------------------------------------------------------------------------------

    static final class RemoteFile {
        String id, name;
        final Map<String, String> props = new HashMap<>();

        String prop(String k) {
            return props.get(k);
        }
    }

    private static Map<String, String> auth(String access) {
        Map<String, String> h = new HashMap<>();
        h.put("Authorization", "Bearer " + access);
        return h;
    }

    private static IOException failure(String what, Reply r) {
        if (r.code == 401)
            return new TokenExpired(what + ": HTTP 401");
        return new IOException(what + ": HTTP " + r.code + " " + r.text().replaceAll("\\s+", " ")
                                                                      .substring(0, Math.min(200, r.text().length())));
    }

    /** The access token was refused (expired early): refresh and try once more. */
    static final class TokenExpired extends IOException {
        TokenExpired(String m) {
            super(m);
        }
    }

    /** Every file of the app's hidden folder. */
    List<RemoteFile> list(String access) throws IOException {
        List<RemoteFile> out = new ArrayList<>();
        String page = null;
        do {
            String url = FILES_URL + "?spaces=appDataFolder&pageSize=1000"
                         + "&fields=nextPageToken,files(id,name,appProperties)"
                         + (page == null ? "" : "&pageToken=" + URLEncoder.encode(page, "UTF-8"));
            Reply r = http.send("GET", url, auth(access), null);
            if (r.code != 200)
                throw failure("list", r);
            JSONObject o = r.json();
            JSONArray files = o.optJSONArray("files");
            for (int i = 0; files != null && i < files.length(); i++) {
                JSONObject f = files.optJSONObject(i);
                if (f == null)
                    continue;
                RemoteFile rf = new RemoteFile();
                rf.id = f.optString("id");
                rf.name = f.optString("name");
                JSONObject p = f.optJSONObject("appProperties");
                if (p != null) {
                    for (java.util.Iterator<String> k = p.keys(); k.hasNext();) {
                        String key = k.next();
                        rf.props.put(key, p.optString(key));
                    }
                }
                out.add(rf);
            }
            page = o.optString("nextPageToken", null);
            if (page != null && page.isEmpty())
                page = null;
        } while (page != null);
        return out;
    }

    /** Creates (id null) or replaces a file of the app's folder; returns its id. */
    String upload(String access, String id, String name, Map<String, String> props, byte[] content)
        throws IOException {
        JSONObject meta = new JSONObject();
        try {
            meta.put("name", name);
            meta.put("mimeType", "application/zip");
            if (id == null)
                meta.put("parents", new JSONArray().put("appDataFolder"));
            JSONObject p = new JSONObject();
            for (Map.Entry<String, String> e : props.entrySet())
                p.put(e.getKey(), e.getValue());
            meta.put("appProperties", p);
        } catch (JSONException e) {
            throw new IOException(e);
        }
        String boundary = "xrecomp" + Long.toHexString(System.nanoTime());
        ByteArrayOutputStream b = new ByteArrayOutputStream(content.length + 1024);
        b.write(("--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + meta + "\r\n--"
                 + boundary + "\r\nContent-Type: application/zip\r\n\r\n").getBytes(StandardCharsets.UTF_8));
        b.write(content);
        b.write(("\r\n--" + boundary + "--\r\n").getBytes(StandardCharsets.UTF_8));
        Map<String, String> h = auth(access);
        h.put("Content-Type", "multipart/related; boundary=" + boundary);
        String url = (id == null ? UPLOAD_URL : UPLOAD_URL + "/" + id) + "?uploadType=multipart&fields=id";
        Reply r = http.send(id == null ? "POST" : "PATCH", url, h, b.toByteArray());
        if (r.code != 200)
            throw failure("upload", r);
        String got = r.json().optString("id");
        if (got.isEmpty())
            throw new IOException("upload: no id");
        return got;
    }

    byte[] download(String access, String id) throws IOException {
        Reply r = http.send("GET", FILES_URL + "/" + id + "?alt=media", auth(access), null);
        if (r.code != 200)
            throw failure("download", r);
        return r.body;
    }
}
