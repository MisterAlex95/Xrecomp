package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * XBA-338: breadcrumbs keep only allow-listed events, the consent gate, and the feedback, session and transaction
 * envelopes (their layout was posted to the live Rustrak v0.30.1 once and read back: feedback, release health,
 * performance).
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ReportPlumbingTest {
    private Context c;
    private final List<byte[]> posted = new ArrayList<>();
    private int answer = 200;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        CrashReport.transport = (url, h, body) -> {
            posted.add(body);
            return answer;
        };
    }

    @After
    public void tearDown() {
        CrashReport.transport = CrashReport.HTTP;
        wipe(Telemetry.dir(c));
        Breadcrumbs.file(c).delete();
    }

    private static void wipe(File d) {
        File[] f = d.listFiles();
        if (f != null)
            for (File x : f)
                x.delete();
        d.delete();
    }

    @Test
    public void breadcrumbsKeepOnlyAllowListedTokens() {
        assertNotNull(Breadcrumbs.line(1, "savestate_load", "slot", "3"));
        assertNull(Breadcrumbs.line(1, "free text"));
        assertNull(Breadcrumbs.line(1, "unknown_event"));
        assertNull(Breadcrumbs.line(1, "mod", "name", "My Mod/with path"));
        assertNull(Breadcrumbs.line(1, "mod", "name", "/storage/emulated/0/x"));
        assertNull(Breadcrumbs.line(1, "cheat", "on"));   // odd pairs
        assertNull(Breadcrumbs.parse("1\tgame_start\tfile=/sdcard/a.iso"));
        assertNull(Breadcrumbs.parse("1\tsomething_else"));

        Breadcrumbs.startRun(c, "renderer", "vulkan");
        Breadcrumbs.add(c, "savestate_save", "slot", "1");
        Breadcrumbs.add(c, "not_allowed", "x", "y");
        Breadcrumbs.add(c, "cheat", "state", "on");
        List<Breadcrumbs.Crumb> all = Breadcrumbs.read(Breadcrumbs.file(c), 50);
        assertEquals(3, all.size());
        assertEquals("game_start", all.get(0).name);
        assertEquals(Arrays.asList("renderer", "vulkan"), all.get(0).data);
        assertEquals("cheat", all.get(2).name);
        Breadcrumbs.startRun(c);
        assertEquals(1, Breadcrumbs.read(Breadcrumbs.file(c), 50).size());
    }

    @Test
    public void theRingIsCutBack() {
        Breadcrumbs.startRun(c);
        for (int i = 0; i < Breadcrumbs.CUT + 20; i++)
            Breadcrumbs.add(c, "rewind", "n", String.valueOf(i));
        List<Breadcrumbs.Crumb> all = Breadcrumbs.read(Breadcrumbs.file(c), Integer.MAX_VALUE);
        assertTrue(String.valueOf(all.size()), all.size() <= Breadcrumbs.CUT);
        assertEquals(String.valueOf(Breadcrumbs.CUT + 19), all.get(all.size() - 1).data.get(1));
    }

    @Test
    public void theGateIsOffByDefaultAndHoldsTheOnlyId() {
        assertFalse(Telemetry.auto(c));
        assertNull(Telemetry.installId(c));
        assertFalse(Telemetry.allowed(c));
        assertFalse(CrashReport.sendAuto(c, new byte[] { 1 }));
        assertTrue(posted.isEmpty());

        Telemetry.setAuto(c, true);
        String id = Telemetry.installId(c);
        assertNotNull(id);
        assertTrue(Telemetry.allowed(c));
        Telemetry.resetInstallId(c);
        assertNotEquals(id, Telemetry.installId(c));
        assertTrue(CrashReport.sendAuto(c, new byte[] { 1 }));
        assertEquals(1, posted.size());

        Telemetry.setAuto(c, false);
        assertNull(Telemetry.installId(c));
        assertFalse(Telemetry.allowed(c));
    }

    @Test
    public void theServerCanStopAutomaticSending() {
        Telemetry.setAuto(c, true);
        answer = 429;
        assertFalse(CrashReport.sendAuto(c, new byte[] { 1 }));
        assertFalse(Telemetry.allowed(c));   // backing off
        wipe(Telemetry.dir(c));
        Telemetry.setAuto(c, true);
        answer = 403;
        assertFalse(CrashReport.sendAuto(c, new byte[] { 1 }));
        assertFalse(Telemetry.allowed(c));   // until the next version
        assertEquals(2, posted.size());
    }

    @Test
    public void feedbackIsAFeedbackItemWithBreadcrumbsAndAnImage() throws Exception {
        Breadcrumbs.startRun(c);
        Breadcrumbs.add(c, "savestate_load", "slot", "2");
        File log = File.createTempFile("game", ".log");
        TestSupport.write(log, "opened /storage/emulated/0/Games/Halo.iso\n");
        CrashReport r = CrashReport.feedback(c, "4D530004", "Halo", "Black screen after /sdcard/x.iso loads",
                                             log, "game.log");
        r.image = new byte[] { (byte)0xFF, (byte)0xD8, 1, 2 };
        List<Object[]> it = CrashReportTest.items(r.envelope("0123456789abcdef0123456789abcdef", 1_700_000_000_000L));
        assertEquals(4, it.size());
        assertEquals("feedback", ((JSONObject)it.get(1)[0]).getString("type"));
        JSONObject ev = new JSONObject(new String((byte[])it.get(1)[1], StandardCharsets.UTF_8));
        assertEquals("info", ev.getString("level"));
        String msg = ev.getJSONObject("contexts").getJSONObject("feedback").getString("message");
        assertEquals("Black screen after <path>/x.iso loads", msg);
        assertFalse(ev.has("exception"));
        assertFalse(ev.has("user"));
        JSONArray bc = ev.getJSONObject("breadcrumbs").getJSONArray("values");
        assertEquals(2, bc.length());
        assertEquals("savestate_load", bc.getJSONObject(1).getString("message"));
        assertEquals("2", bc.getJSONObject(1).getJSONObject("data").getString("slot"));
        assertEquals("game.log", ((JSONObject)it.get(2)[0]).getString("filename"));
        assertFalse(new String((byte[])it.get(2)[1], StandardCharsets.UTF_8).contains("/storage/"));
        assertEquals("image/jpeg", ((JSONObject)it.get(3)[0]).getString("content_type"));
        assertEquals(4, ((byte[])it.get(3)[1]).length);
        assertTrue(r.summary(), r.summary().startsWith("Black screen after <path>/x.iso loads\n"));
        log.delete();
    }

    @Test
    public void sessionAndTransactionEnvelopes() throws Exception {
        List<Object[]> s = CrashReportTest.items(CrashReport.sessionEnvelope(
                "sid-1", "did-1", true, "crashed", 1_700_000_000_000L, 1_700_000_012_500L, 1, "xrecomp@1", "release"));
        assertEquals("session", ((JSONObject)s.get(1)[0]).getString("type"));
        JSONObject se = new JSONObject(new String((byte[])s.get(1)[1], StandardCharsets.UTF_8));
        assertEquals("crashed", se.getString("status"));
        assertTrue(se.getBoolean("init"));
        assertEquals(12.5, se.getDouble("duration"), 1e-9);
        assertEquals("xrecomp@1", se.getJSONObject("attrs").getString("release"));

        Map<String, String> tags = new LinkedHashMap<>();
        tags.put("console", "xbox");
        tags.put("device", "AYN Thor");
        List<Object[]> t = CrashReportTest.items(CrashReport.transactionEnvelope(
                "prepare", "prepare", "ok", 1_700_000_000_000L, 1_700_000_060_000L,
                Arrays.asList(new CrashReport.Span("prepare.recomp", 1_700_000_010_000L, 1_700_000_050_000L)),
                tags, "xrecomp@1", "release"));
        assertEquals("transaction", ((JSONObject)t.get(1)[0]).getString("type"));
        JSONObject tx = new JSONObject(new String((byte[])t.get(1)[1], StandardCharsets.UTF_8));
        assertEquals("prepare", tx.getString("transaction"));
        assertEquals(60.0, tx.getDouble("timestamp") - tx.getDouble("start_timestamp"), 1e-6);
        String root = tx.getJSONObject("contexts").getJSONObject("trace").getString("span_id");
        JSONObject sp = tx.getJSONArray("spans").getJSONObject(0);
        assertEquals(root, sp.getString("parent_span_id"));
        assertEquals("prepare.recomp", sp.getString("op"));
        assertEquals("AYN Thor", tx.getJSONObject("tags").getString("device"));
    }
}
