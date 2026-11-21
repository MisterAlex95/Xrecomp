package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

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
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * « Send report » (XBA-335): the DSN, what the scrubbing removes, the log's trim, the Sentry envelope's layout (checked
 * against the live Rustrak server once with the bytes this class makes), the last game's header, and the queue of
 * reports that could not leave.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class CrashReportTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private Context c;
    private final List<byte[]> posted = new ArrayList<>();
    private final List<Map<String, String>> headers = new ArrayList<>();
    private int answer = 200;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        CrashReport.transport = (url, h, body) -> {
            if (answer < 0)
                throw new IOException("offline");
            assertEquals("https://rustrak.adserver.fr/api/1/envelope/", url);
            headers.add(h);
            posted.add(body);
            return answer;
        };
    }

    @After
    public void tearDown() {
        CrashReport.transport = CrashReport.HTTP;
    }

    @Test
    public void parsesTheDsn() {
        CrashReport.Dsn d = CrashReport.Dsn.parse("https://abc123@rustrak.adserver.fr/1");
        assertEquals("https://rustrak.adserver.fr/api/1/envelope/", d.endpoint);
        assertEquals("abc123", d.key);
        d = CrashReport.Dsn.parse("http://k:secret@host:9000/sub/42/");
        assertEquals("http://host:9000/sub/api/42/envelope/", d.endpoint);
        assertEquals("k", d.key);
        assertNull(CrashReport.Dsn.parse(""));
        assertNull(CrashReport.Dsn.parse("not a dsn"));
        assertNull(CrashReport.Dsn.parse("https://rustrak.adserver.fr/1"));
        // The build's default (no -PxbaReportDsn): the project's server.
        assertTrue(CrashReport.available(c));
        assertEquals("https://rustrak.adserver.fr/api/1/envelope/", CrashReport.dsn(c).endpoint);
    }

    @Test
    public void scrubsPathsAddressesAndLinks() {
        assertEquals("open <path>/Halo (USA).iso failed",
                     CrashReport.scrub("open /storage/emulated/0/Games/Xbox/Halo (USA).iso failed"));
        assertEquals("<path>/default.xbe", CrashReport.scrub("/sdcard/Alex's stuff/xbox/default.xbe"));
        assertEquals("<path>/disc.iso", CrashReport.scrub("/storage/1A2B-3C4D/roms/disc.iso"));
        assertEquals("<path>/x.bin", CrashReport.scrub("/mnt/media_rw/1A2B-3C4D/x.bin"));
        // The app's own folders keep their names (title IDs, our files), the user id goes.
        assertEquals("<app:fr.adserver.xrecomp.shell>/files/work/4D530004/prepare.log",
                     CrashReport.scrub("/data/user/0/fr.adserver.xrecomp.shell/files/work/4D530004/prepare.log"));
        assertEquals("<app:fr.adserver.xrecomp.shell>/files/x",
                     CrashReport.scrub("/data/data/fr.adserver.xrecomp.shell/files/x"));
        // The network library: logins, NAS hosts, magnet links, addresses.
        assertEquals("smb://<host>/share/Halo.iso", CrashReport.scrub("smb://alex:hunter2@nas.local/share/Halo.iso"));
        assertEquals("GET https://<host>/a?b=1", CrashReport.scrub("GET https://user:pw@192.168.1.20:8080/a?b=1"));
        assertEquals("added magnet:<link> ok", CrashReport.scrub("added magnet:?xt=urn:btih:ABCDEF&dn=Halo&tr=udp ok"));
        assertEquals("peer <ip>:6881", CrashReport.scrub("peer 10.0.0.12:6881"));
        assertEquals("peer <ip>", CrashReport.scrub("peer fe80:0:0:0:1c2b:3a4d:5e6f:7a8b"));
        assertEquals("by <email>", CrashReport.scrub("by someone.name+x@example.co.uk"));
        assertEquals("content://<host>/document/1", CrashReport.scrub("content://com.android.externalstorage.documents/document/1"));
        // What the crash needs stays.
        String bt = "#00 pc 000000000012a40c  /data/app/~~x==/fr.adserver.xrecomp.shell-1/lib/arm64/libtitle.so (func_00012a40+12)";
        assertTrue(CrashReport.scrub(bt), CrashReport.scrub(bt).contains("libtitle.so (func_00012a40+12)"));
        assertEquals("app 0.15.0, Android 14", CrashReport.scrub("app 0.15.0, Android 14"));
        assertEquals("", CrashReport.scrub(null));
    }

    /** XBA-342: desktop home folders, the same vectors as macos/Sources/xrecomp-check/ReportChecks.swift. */
    @Test
    public void scrubsHomeFolders() {
        assertEquals("# RECOMP_SAVE_DIR=<app:xboxrecomp>/titles/58410001/save",
                     CrashReport.scrub("# RECOMP_SAVE_DIR=/Users/you/Library/Application Support/xboxrecomp/titles/58410001/save"));
        assertEquals("<app:xboxrecomp>/settings.json",
                     CrashReport.scrub("~/Library/Application Support/xboxrecomp/settings.json"));
        assertEquals("open <path>/Halo (USA).iso failed",
                     CrashReport.scrub("open /Users/you/Games/Xbox/Halo (USA).iso failed"));
        assertEquals("<path>/x.py:12: error", CrashReport.scrub("/Users/you/src/Xrecomp/tools/prepare/x.py:12: error"));
        assertEquals("HOME=<path>", CrashReport.scrub("HOME=/Users/you"));
        assertEquals("cd <path>/ ok", CrashReport.scrub("cd /Users/you/ ok"));
        assertEquals("<path>/disc.iso", CrashReport.scrub("/home/you/roms/disc.iso"));
        assertEquals("<path>/x.iso", CrashReport.scrub("C:\\Users\\John Smith\\Documents\\x.iso"));
        assertEquals("log <path>/run.log", CrashReport.scrub("log c:/users/you/AppData/Local/xrecomp/run.log"));
        assertEquals("cwd <path>", CrashReport.scrub("cwd C:\\Users\\you"));
        for (String in : new String[] {"/Users/you/a", "/home/you/a b/c", "C:\\Users\\you\\x"})
            assertFalse(CrashReport.scrub(in), CrashReport.scrub(in).contains("you"));
        // Not a home folder: left alone.
        assertEquals("/usr/lib/libSystem.B.dylib", CrashReport.scrub("/usr/lib/libSystem.B.dylib"));
    }

    @Test
    public void trimsTheLogKeepingHeadAndTail() {
        byte[] small = "short".getBytes(StandardCharsets.UTF_8);
        assertArrayEquals(small, CrashReport.trim(small, 1000));
        byte[] big = new byte[1_000_000];
        Arrays.fill(big, (byte)'x');
        System.arraycopy("HEAD".getBytes(StandardCharsets.UTF_8), 0, big, 0, 4);
        System.arraycopy("TAIL".getBytes(StandardCharsets.UTF_8), 0, big, big.length - 4, 4);
        byte[] t = CrashReport.trim(big, CrashReport.LOG_MAX);
        assertEquals(CrashReport.LOG_MAX, t.length);
        String s = new String(t, StandardCharsets.US_ASCII);
        assertTrue(s.startsWith("HEAD"));
        assertTrue(s.endsWith("TAIL"));
        assertTrue(s.contains("# ---- " + (big.length - CrashReport.LOG_MAX) + " bytes cut ----"));
    }

    /** Header line, item header, payload, as many bytes as the item says: what the server reads. */
    static List<Object[]> items(byte[] env) throws Exception {
        List<Object[]> out = new ArrayList<>();
        int p = indexOf(env, 0);
        JSONObject head = new JSONObject(new String(env, 0, p, StandardCharsets.UTF_8));
        out.add(new Object[] { head, null });
        p++;
        while (p < env.length) {
            int e = indexOf(env, p);
            JSONObject ih = new JSONObject(new String(env, p, e - p, StandardCharsets.UTF_8));
            int len = ih.getInt("length");
            byte[] payload = Arrays.copyOfRange(env, e + 1, e + 1 + len);
            out.add(new Object[] { ih, payload });
            p = e + 1 + len;
            assertEquals('\n', env[p]);
            p++;
        }
        return out;
    }

    private static int indexOf(byte[] b, int from) {
        for (int i = from; i < b.length; i++)
            if (b[i] == '\n')
                return i;
        return b.length;
    }

    @Test
    public void buildsAnEnvelopeOfAnEventAndTheLog() throws Exception {
        File log = tmp.newFile("prepare.log");
        TestSupport.write(log, "step compile\nreading /storage/emulated/0/xbox/Halo/default.xbe\nclang: error\n");
        CrashReport r = CrashReport.prepare(c, "4D530004", "Halo \"CE\"", "CC_FAILED", "3 files failed", log);
        byte[] env = r.envelope("0123456789abcdef0123456789abcdef", 1_700_000_000_000L);
        List<Object[]> it = items(env);
        assertEquals(3, it.size());
        JSONObject head = (JSONObject)it.get(0)[0];
        assertEquals("0123456789abcdef0123456789abcdef", head.getString("event_id"));
        assertEquals("2023-11-14T22:13:20Z", head.getString("sent_at"));
        assertEquals("event", ((JSONObject)it.get(1)[0]).getString("type"));
        JSONObject ev = new JSONObject(new String((byte[])it.get(1)[1], StandardCharsets.UTF_8));
        assertEquals("PrepareFailed", ev.getJSONObject("exception").getJSONArray("values").getJSONObject(0)
                .getString("type"));
        assertEquals("CC_FAILED: 3 files failed", ev.getJSONObject("exception").getJSONArray("values")
                .getJSONObject(0).getString("value"));
        assertTrue(ev.getString("release").startsWith("xrecomp@"));
        JSONObject tags = ev.getJSONObject("tags");
        assertEquals("xbox", tags.getString("console"));
        assertEquals("4D530004", tags.getString("game_key"));
        assertEquals("Halo \"CE\"", tags.getString("game"));
        assertEquals("CC_FAILED", tags.getString("prepare_code"));
        assertEquals("prepare", tags.getString("kind"));
        assertEquals("prepare", ev.getJSONArray("fingerprint").getString(0));
        JSONObject ah = (JSONObject)it.get(2)[0];
        assertEquals("attachment", ah.getString("type"));
        assertEquals("prepare.log", ah.getString("filename"));
        assertEquals("text/plain", ah.getString("content_type"));
        String att = new String((byte[])it.get(2)[1], StandardCharsets.UTF_8);
        assertTrue(att, att.contains("reading <path>/default.xbe"));
        assertFalse(att.contains("/storage/"));
        // The summary the sheet shows: the event, no log lines.
        String[] lines = r.summary().split("\n");
        assertEquals(r.summary(), 4, lines.length);
        assertEquals("PrepareFailed: CC_FAILED: 3 files failed", lines[0]);
        assertEquals("Halo \"CE\" (4D530004) xbox", lines[1]);
        assertTrue(lines[2], lines[2].contains(" · Android "));
        assertEquals("+ prepare.log (1 KB)", lines[3]);
        assertFalse(r.summary().contains("clang"));
    }

    @Test
    public void readsHowTheLastGameEnded() throws Exception {
        File log = tmp.newFile("last-game.log");
        String head = "# XRecomp: the game's last run\n# app 0.15.0, Android 14, AYN Thor\n"
                      + LogProvider.exitHeader(4242, 1_700_000_000_000L, "native crash", 11, "signal 11 (SIGSEGV)");
        TestSupport.write(log, head + "\n# ---- crash buffer ----\n"
                               + "F DEBUG   :       #00 pc 000000000012a40c  /data/app/~~a==/fr.adserver.xrecomp.shell-2/"
                               + "lib/arm64/libtitle.so (func_00012a40+12) (BuildId: 1)\n"
                               + "F DEBUG   :       #01 pc 0000000000001000  /apex/libc.so (abort+8)\n");
        CrashReport r = CrashReport.game(c, "n64-NSME-12345678", "Some Game", log);
        assertEquals("Game: native crash", r.type);
        assertEquals("status 11 (signal 11 (SIGSEGV))", r.value);
        assertEquals("n64", r.tags.get("console"));
        assertEquals("native crash", r.tags.get("end"));
        assertEquals("top frame: libtitle.so func_00012a40", r.message);
        assertEquals(Arrays.asList("game", "n64", "n64-NSME-12345678", "native crash", "libtitle.so func_00012a40"),
                     r.fingerprint);
        assertEquals(LogProvider.LAST_GAME_LOG, r.logName);
        // No record of the end: still a report, said so.
        File none = tmp.newFile("none.log");
        TestSupport.write(none, "# XRecomp: the game's last run\n# no record of how the game's process ended\n");
        CrashReport n = CrashReport.game(c, null, null, none);
        assertEquals("Game: unknown end", n.type);
        assertEquals("unknown", n.tags.get("console"));
        assertEquals("", n.message);
    }

    @Test
    public void consoleOfKeys() {
        assertEquals("xbox", CrashReport.consoleOf("4D530004"));
        assertEquals("gc", CrashReport.consoleOf("gc-GALE01-0"));
        assertEquals("ps1", CrashReport.consoleOf("ps1-SLUS-00594"));
        assertEquals("unknown", CrashReport.consoleOf(null));
    }

    @Test
    public void sendsWithTheKeyAndQueuesWhatCouldNotLeave() throws Exception {
        File log = tmp.newFile("p.log");
        TestSupport.write(log, "x\n");
        CrashReport r = CrashReport.prepare(c, "4D530004", "Game", "KILLED", "", log);
        assertTrue(r.send(c));
        assertEquals(1, posted.size());
        String auth = headers.get(0).get("X-Sentry-Auth");
        assertTrue(auth, auth.contains("sentry_key=b3f3a378974546b0bd69b26fdfb24fa6"));
        assertEquals("application/x-sentry-envelope", headers.get(0).get("Content-Type"));
        assertEquals(0, CrashReport.queued(c).length);

        // Offline: the post and its retry fail, the report waits.
        answer = -1;
        assertFalse(r.send(c));
        assertFalse(r.send(c));
        assertEquals(2, CrashReport.queued(c).length);
        // A server error is not a success either.
        answer = 500;
        assertFalse(r.send(c));
        assertEquals(3, CrashReport.queued(c).length);
        // Never more than QUEUE_MAX waiting.
        for (int i = 0; i < 6; i++)
            r.send(c);
        assertEquals(CrashReport.QUEUE_MAX, CrashReport.queued(c).length);

        // Back online: this one and the waiting ones leave.
        answer = 200;
        posted.clear();
        assertTrue(r.send(c));
        assertEquals(1 + CrashReport.QUEUE_MAX, posted.size());
        assertEquals(0, CrashReport.queued(c).length);
        assertNotNull(posted.get(1));
    }
}
