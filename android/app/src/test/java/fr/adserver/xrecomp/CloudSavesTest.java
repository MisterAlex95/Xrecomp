package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Cloud saves against a Google that lives in the test (FakeGoogle: the device flow, the token endpoint, the app's
 * hidden folder): the sync decisions, two devices taking turns, a conflict and its settling, a running game, a
 * refused refresh token.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class CloudSavesTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = "4D530004", GBA = "gba-AXVE";

    /** Google's endpoints, in memory. */
    static final class FakeGoogle implements GoogleDrive.Http {
        final Map<String, JSONObject> meta = new LinkedHashMap<>();
        final Map<String, byte[]> content = new LinkedHashMap<>();
        final List<String> calls = new ArrayList<>();
        int polls, nextId, interval = 5;
        boolean revoked, refuseRefresh;
        /** False: the player has not said yes yet (pending at every poll). */
        volatile boolean allow = true;
        String pendingUntilPoll = "2";

        @Override
        public GoogleDrive.Reply send(String method, String url, Map<String, String> h, byte[] body)
            throws IOException {
            calls.add(method + " " + url.replaceAll("\\?.*", ""));
            String b = body == null ? "" : new String(body, StandardCharsets.ISO_8859_1);
            try {
                if (url.equals(GoogleDrive.DEVICE_CODE_URL))
                    return json(200, new JSONObject().put("device_code", "dc").put("user_code", "ABCD-EFGH")
                                         .put("verification_url", "https://www.google.com/device")
                                         .put("expires_in", 1800).put("interval", interval));
                if (url.equals(GoogleDrive.TOKEN_URL)) {
                    if (b.contains("device_code")) {
                        if (++polls < Integer.parseInt(pendingUntilPoll) || !allow)
                            return json(428, new JSONObject().put("error", "authorization_pending"));
                        String claims = java.util.Base64.getUrlEncoder().encodeToString(
                            "{\"email\":\"player@example.com\"}".getBytes(StandardCharsets.UTF_8));
                        return json(200, new JSONObject().put("access_token", "at").put("refresh_token", "rt")
                                             .put("expires_in", 3600).put("id_token", "h." + claims + ".s"));
                    }
                    if (refuseRefresh)
                        return json(400, new JSONObject().put("error", "invalid_grant"));
                    return json(200, new JSONObject().put("access_token", "at").put("expires_in", 3600));
                }
                if (url.startsWith(GoogleDrive.REVOKE_URL)) {
                    revoked = true;
                    return json(200, new JSONObject());
                }
                if (!"Bearer at".equals(h.get("Authorization")))
                    return json(401, new JSONObject());
                if (method.equals("GET") && url.startsWith(GoogleDrive.FILES_URL + "?")) {
                    JSONArray files = new JSONArray();
                    for (JSONObject m : meta.values())
                        files.put(m);
                    return json(200, new JSONObject().put("files", files));
                }
                if (method.equals("GET") && url.contains("alt=media")) {
                    String id = url.substring(GoogleDrive.FILES_URL.length() + 1, url.indexOf('?'));
                    return new GoogleDrive.Reply(200, content.get(id));
                }
                if (url.startsWith(GoogleDrive.UPLOAD_URL)) {
                    String boundary = h.get("Content-Type").replaceAll(".*boundary=", "");
                    String[] parts = b.split("--" + boundary);
                    String metaJson = parts[1].substring(parts[1].indexOf("\r\n\r\n") + 4).trim();
                    String zip = parts[2].substring(parts[2].indexOf("\r\n\r\n") + 4);
                    zip = zip.substring(0, zip.length() - 2);
                    JSONObject m = new JSONObject(metaJson);
                    String id;
                    if (method.equals("POST")) {
                        assertEquals("appDataFolder", m.getJSONArray("parents").getString(0));
                        id = "f" + (++nextId);
                    } else {
                        id = url.substring(GoogleDrive.UPLOAD_URL.length() + 1, url.indexOf('?'));
                    }
                    meta.put(id, new JSONObject().put("id", id).put("name", m.getString("name"))
                                     .put("appProperties", m.getJSONObject("appProperties")));
                    content.put(id, zip.getBytes(StandardCharsets.ISO_8859_1));
                    return json(200, new JSONObject().put("id", id));
                }
            } catch (org.json.JSONException e) {
                throw new IOException(e);
            }
            return json(404, new JSONObject());
        }

        static GoogleDrive.Reply json(int code, JSONObject o) {
            return new GoogleDrive.Reply(code, o.toString().getBytes(StandardCharsets.UTF_8));
        }
    }

    private FakeGoogle google;
    private GoogleDrive drive;
    private Storage.Dirs a, b;

    @Before
    public void setUp() throws Exception {
        google = new FakeGoogle();
        drive = new GoogleDrive(google, "client", "secret");
        a = new Storage.Dirs(tmp.newFolder("a"), tmp.newFolder("ae"));
        b = new Storage.Dirs(tmp.newFolder("b"), tmp.newFolder("be"));
    }

    private static void save(Storage.Dirs d, String rel, String text) throws IOException {
        TestSupport.write(new File(d.saveDir(), rel), text.getBytes(StandardCharsets.UTF_8));
    }

    private static String read(Storage.Dirs d, String rel) throws IOException {
        return new String(Files.readAllBytes(new File(d.saveDir(), rel).toPath()), StandardCharsets.UTF_8);
    }

    /** The device flow to the end, and the account kept for d. */
    private void signIn(Storage.Dirs d) throws IOException {
        long now = 1000;
        GoogleDrive.DeviceCode dc = drive.deviceCode(now);
        GoogleDrive.PollResult p;
        do {
            p = drive.poll(dc, now += 5000);
        } while (p.state == GoogleDrive.Poll.PENDING);
        assertEquals(GoogleDrive.Poll.OK, p.state);
        CloudSaves.connect(d.filesDir, p.tokens);
    }

    private CloudSaves.Report sync(Storage.Dirs d, String... busy) {
        java.util.List<String> l = java.util.Arrays.asList(busy);
        return new CloudSaves(drive, d, "test", l).syncAll(System.currentTimeMillis());
    }

    @Test
    public void decisions() {
        assertEquals(CloudSaves.Action.NONE, CloudSaves.decide(null, null, null));
        assertEquals(CloudSaves.Action.NONE, CloudSaves.decide("x", "x", null));
        assertEquals(CloudSaves.Action.UPLOAD, CloudSaves.decide("x", null, null));
        assertEquals(CloudSaves.Action.UPLOAD, CloudSaves.decide("y", "x", "x"));
        assertEquals(CloudSaves.Action.DOWNLOAD, CloudSaves.decide(null, "x", null));
        assertEquals(CloudSaves.Action.DOWNLOAD, CloudSaves.decide("x", "y", "x"));
        assertEquals("deleted here: the cloud keeps it", CloudSaves.Action.NONE, CloudSaves.decide(null, "x", "x"));
        assertEquals(CloudSaves.Action.CONFLICT, CloudSaves.decide("y", "z", "x"));
        assertEquals("never synced, both have saves", CloudSaves.Action.CONFLICT, CloudSaves.decide("y", "z", null));
    }

    @Test
    public void deviceFlow() throws Exception {
        GoogleDrive.DeviceCode dc = drive.deviceCode(0);
        assertEquals("ABCD-EFGH", dc.userCode);
        assertEquals("https://www.google.com/device", dc.verificationUrl);
        assertEquals(5, dc.intervalS);
        assertEquals(GoogleDrive.Poll.PENDING, drive.poll(dc, 5000).state);
        GoogleDrive.PollResult p = drive.poll(dc, 10000);
        assertEquals(GoogleDrive.Poll.OK, p.state);
        assertEquals("rt", p.tokens.refresh);
        assertEquals("player@example.com", p.tokens.email);
        assertEquals(GoogleDrive.Poll.EXPIRED, drive.poll(dc, 1800_001).state);
    }

    /** Device A plays and uploads, B gets it, B plays on and uploads, A gets B's. */
    @Test
    public void twoDevicesTakeTurns() throws Exception {
        signIn(a);
        signIn(b);
        save(a, "UDATA/4d530004/save.dat", "level 1");
        save(a, GBA + "/game.sav", "gba 1");
        CloudSaves.Report r = sync(a);
        assertEquals(java.util.Arrays.asList(TID, GBA), r.uploaded);
        assertEquals(2, google.meta.size());

        r = sync(b);
        assertEquals(java.util.Arrays.asList(TID, GBA), r.downloaded);
        assertEquals("level 1", read(b, "UDATA/4d530004/save.dat"));
        assertEquals(0, sync(b).moved());
        assertEquals("nothing changed", 0, sync(a).moved());

        save(b, "UDATA/4d530004/save.dat", "level 2");
        r = sync(b);
        assertEquals(Collections.singletonList(TID), r.uploaded);
        assertEquals("replaced in place, not a second file", 2, google.meta.size());
        r = sync(a);
        assertEquals(Collections.singletonList(TID), r.downloaded);
        assertEquals("level 2", read(a, "UDATA/4d530004/save.dat"));
        assertEquals("the device's own kept aside", "level 1",
                     read(a, SaveArchive.BACKUP_DIR + "/" + TID + "/UDATA/4d530004/save.dat"));
    }

    /** Both changed: nothing moves until the player picks; either side then wins. */
    @Test
    public void conflictWaitsForThePlayer() throws Exception {
        signIn(a);
        signIn(b);
        save(a, "UDATA/4d530004/save.dat", "a");
        sync(a);
        sync(b);
        save(a, "UDATA/4d530004/save.dat", "a2");
        save(b, "UDATA/4d530004/save.dat", "b2");
        sync(a);
        CloudSaves.Report r = sync(b);
        assertEquals(Collections.singletonList(TID), r.conflicts);
        assertEquals("b2", read(b, "UDATA/4d530004/save.dat"));
        assertTrue(CloudSaves.load(b.filesDir).conflicts.contains(TID));

        r = new CloudSaves(drive, b, "test", null).syncOne(TID, CloudSaves.Side.KEEP_CLOUD, 0);
        assertEquals(Collections.singletonList(TID), r.downloaded);
        assertEquals("a2", read(b, "UDATA/4d530004/save.dat"));
        assertFalse(CloudSaves.load(b.filesDir).conflicts.contains(TID));

        save(b, "UDATA/4d530004/save.dat", "b3");
        save(a, "UDATA/4d530004/save.dat", "a3");
        sync(b);
        assertEquals(Collections.singletonList(TID), sync(a).conflicts);
        r = new CloudSaves(drive, a, "test", null).syncOne(TID, CloudSaves.Side.KEEP_DEVICE, 0);
        assertEquals(Collections.singletonList(TID), r.uploaded);
        assertEquals(Collections.singletonList(TID), sync(b).downloaded);
        assertEquals("a3", read(b, "UDATA/4d530004/save.dat"));
    }

    /** First sync on a second device that has saves of its own: a conflict, never an overwrite. */
    @Test
    public void bothHaveSavesAtFirstSync() throws Exception {
        signIn(a);
        signIn(b);
        save(a, "UDATA/4d530004/save.dat", "a");
        save(b, "UDATA/4d530004/save.dat", "b");
        sync(a);
        assertEquals(Collections.singletonList(TID), sync(b).conflicts);
        assertEquals("b", read(b, "UDATA/4d530004/save.dat"));
    }

    @Test
    public void runningGameIsNotReplaced() throws Exception {
        signIn(a);
        signIn(b);
        save(a, "UDATA/4d530004/save.dat", "a");
        sync(a);
        CloudSaves.Report r = sync(b, TID);
        assertEquals(Collections.singletonList(TID), r.pending);
        assertFalse(new File(b.saveDir(), "UDATA/4d530004").exists());
        assertEquals(Collections.singletonList(TID), sync(b).downloaded);
    }

    @Test
    public void deletedHereStaysInTheCloud() throws Exception {
        signIn(a);
        save(a, "UDATA/4d530004/save.dat", "a");
        sync(a);
        GameStore.deleteTree(new File(a.saveDir(), "UDATA/4d530004"));
        assertEquals(0, sync(a).moved());
        assertEquals(1, google.meta.size());
        assertFalse(new File(a.saveDir(), "UDATA/4d530004").exists());
    }

    @Test
    public void refusedRefreshSignsOut() throws Exception {
        signIn(a);
        save(a, "UDATA/4d530004/save.dat", "a");
        google.refuseRefresh = true;
        CloudSaves.Report r = sync(a);
        assertTrue(r.signedOut);
        assertFalse(CloudSaves.load(a.filesDir).connected());
    }

    @Test
    public void disconnectRevokesAndKeepsEverything() throws Exception {
        signIn(a);
        save(a, "UDATA/4d530004/save.dat", "a");
        sync(a);
        CloudSaves.disconnect(a.filesDir, drive);
        assertTrue(google.revoked);
        CloudSaves.State s = CloudSaves.load(a.filesDir);
        assertFalse(s.connected());
        assertEquals("player@example.com", s.email);
        assertEquals("a", read(a, "UDATA/4d530004/save.dat"));
        assertEquals(1, google.meta.size());
        assertEquals("not connected: nothing", 0, sync(a).moved());
    }

    @Test
    public void notConfiguredDoesNothing() throws Exception {
        signIn(a);
        save(a, "UDATA/4d530004/save.dat", "a");
        CloudSaves.Report r = new CloudSaves(new GoogleDrive(google, "", ""), a, "x", null).syncAll(0);
        assertEquals(0, r.moved());
        assertNull(google.meta.isEmpty() ? null : "uploaded");
    }
}
