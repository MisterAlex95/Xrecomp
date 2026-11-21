package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

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
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * The « game » performance transaction: the run summary read (numbers and known words only), its tags and
 * measurements, the gate (the option off: nothing; a crash: nothing; another process's summary: kept), never queued,
 * and the Benchmark's consented send.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class GamePerfTest {
    /** A summary as host_perf_summary_json writes it (an Xbox 360 run: its gpu figures and words). */
    static final String X360 = "{\"v\":1,\"pid\":4242,\"console\":\"x360\",\"final\":0,\"bench\":0,\"seconds\":312.5,"
        + "\"guest_frames\":9000,\"host_frames\":9100,\"speed_pct\":48.000,\"guest_fps\":28.800,\"full_hz\":60.000,"
        + "\"shown_fps\":29.100,\"fps_speed_median\":29.000,\"fps_speed_p5\":21.500,\"fps_shown_median\":29.500,"
        + "\"fps_shown_p5\":22.000,\"samples\":312,\"frame_mean_ms\":34.300,\"frame_p99_ms\":52.000,"
        + "\"worst_ms\":120.000,\"over_1\":4550,\"over_2\":120,\"budget_ms\":16.667,\"busy_ms\":33.000,"
        + "\"emu_ms\":28.000,\"render_ms\":2.500,\"idle_ms\":0.300,\"stages\":{\"emu\":28.000,\"audio\":0.200,"
        + "\"present\":2.500,\"other\":3.000},\"nums\":{\"gpu_ms\":9.500,\"gpu_cpu_ms\":6.000,\"gpu_wait_ms\":3.500,"
        + "\"resolve_read_ms\":1.250,\"resolve_write_ms\":0.500,\"guest_cpu_ms\":17.000,\"Bad Key\":1,"
        + "\"inf_ms\":\"x\"},\"words\":{\"gpu_thread\":\"on\",\"resolve_mode\":\"lazy\",\"title\":\"/storage/Halo.iso\"}}";

    private Context c;
    private final List<byte[]> posted = new ArrayList<>();
    private boolean netOk = true;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        CrashReport.transport = (url, h, body) -> {
            posted.add(body);
            return 200;
        };
        GamePerf.work = Runnable::run;
        GamePerf.network = ctx -> netOk;
        GameStore.sessionStart(c, 1_700_000_000_000L, "x360-4D5307E6");
    }

    @After
    public void tearDown() {
        CrashReport.transport = CrashReport.HTTP;
        GamePerf.network = GamePerf.SYSTEM_NETWORK;
        Telemetry.setAuto(c, false);
        GamePerf.file(c).delete();
        File[] f = Telemetry.dir(c).listFiles();
        if (f != null)
            for (File x : f)
                x.delete();
    }

    private void summary(String json) throws Exception {
        File f = GamePerf.file(c);
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), json.getBytes(StandardCharsets.UTF_8));
    }

    private static JSONObject tx(byte[] body) throws Exception {
        List<Object[]> it = CrashReportTest.items(body);
        assertEquals("transaction", ((JSONObject)it.get(1)[0]).getString("type"));
        return new JSONObject(new String((byte[])it.get(1)[1], StandardCharsets.UTF_8));
    }

    @Test
    public void parsesNumbersAndKnownWordsOnly() {
        GamePerf.Summary s = GamePerf.parse(X360);
        assertNotNull(s);
        assertEquals(4242, s.pid);
        assertEquals("x360", s.console);
        assertEquals(29.5, s.fpsShownMedian, 1e-9);
        assertEquals(22.0, s.fpsShownP5, 1e-9);
        assertEquals(52.0, s.frameP99, 1e-9);
        assertEquals(9.5, s.nums.get("gpu_ms"), 1e-9);
        assertFalse(s.nums.containsKey("Bad Key"));
        assertFalse(s.nums.containsKey("inf_ms"));
        assertEquals("on", s.words.get("gpu_thread"));
        assertEquals("lazy", s.words.get("resolve_mode"));
        assertFalse(s.words.containsKey("title"));   // not a word: dropped
        assertNull(GamePerf.parse("{\"v\":2}"));
        assertNull(GamePerf.parse("not json"));
        assertNull(GamePerf.parse(null));
    }

    @Test
    public void theTransactionCarriesMeasurementsAndNoName() throws Exception {
        GamePerf.Summary s = GamePerf.parse(X360);
        Map<String, String> tags = GamePerf.tags(c, s, "x360-4D5307E6", AutoReports.EXITED, null);
        byte[] body = GamePerf.envelope(s, tags, 1000, 313_500, "xrecomp@test", "debug");
        JSONObject t = tx(body);
        assertEquals("game", t.getString("transaction"));
        assertEquals("game.run", t.getJSONObject("contexts").getJSONObject("trace").getString("op"));
        JSONObject tg = t.getJSONObject("tags");
        assertEquals("x360", tg.getString("console"));
        assertEquals("x360-4D5307E6", tg.getString("game_key"));
        assertEquals("0", tg.getString("benchmark"));
        assertEquals("exited", tg.getString("end"));
        assertEquals("on", tg.getString("gpu_thread"));
        assertEquals("lazy", tg.getString("resolve_mode"));
        assertFalse(tg.has("title"));
        assertFalse(tg.has("game"));
        JSONObject m = t.getJSONObject("measurements");
        assertEquals(29.5, m.getJSONObject("fps_shown_median").getDouble("value"), 1e-9);
        assertEquals(22.0, m.getJSONObject("fps_shown_p5").getDouble("value"), 1e-9);
        assertEquals(21.5, m.getJSONObject("fps_speed_p5").getDouble("value"), 1e-9);
        assertEquals("millisecond", m.getJSONObject("frame_time_p99").getString("unit"));
        assertEquals(34.3, m.getJSONObject("frame_time_mean").getDouble("value"), 1e-9);
        assertEquals(28.0, m.getJSONObject("guest_ms").getDouble("value"), 1e-9);
        assertEquals(2.5, m.getJSONObject("present_ms").getDouble("value"), 1e-9);
        assertEquals(9.5, m.getJSONObject("gpu_ms").getDouble("value"), 1e-9);
        assertEquals(3.5, m.getJSONObject("gpu_wait_ms").getDouble("value"), 1e-9);
        assertEquals(1.25, m.getJSONObject("resolve_read_ms").getDouble("value"), 1e-9);
        assertEquals("percent", m.getJSONObject("speed_pct").getString("unit"));
        assertEquals(312.5, m.getJSONObject("run_seconds").getDouble("value"), 1e-9);
        String text = new String(body, StandardCharsets.UTF_8);
        assertFalse(text.contains("storage"));
        assertFalse(text.contains("Halo"));
        assertFalse(text.contains("4242"));   // the pid stays on the device
        writeProbe(s);
    }

    /**
     * XBA_PROBE_OUT set (a host run, never CI): the envelope of a probe transaction (release xrecomp@probe,
     * environment debug) is written there, for one post to the project's Rustrak by hand.
     */
    private void writeProbe(GamePerf.Summary s) throws Exception {
        String out = System.getenv("XBA_PROBE_OUT");
        if (out == null || out.isEmpty())
            return;
        Map<String, String> tags = GamePerf.tags(c, s, "x360-PROBE", AutoReports.EXITED, null);
        long now = System.currentTimeMillis();
        Files.write(new File(out).toPath(), GamePerf.envelope(s, tags, now - 312_500, now, "xrecomp@probe", "debug"));
    }

    @Test
    public void nothingLeavesWithTheOptionOff() throws Exception {
        summary(X360);
        assertFalse(GamePerf.runEnded(c, 4242, AutoReports.EXITED, "exited", 1000, 2000));
        assertTrue(posted.isEmpty());
        assertFalse(GamePerf.file(c).exists());
    }

    @Test
    public void anEndedRunLeavesOnceWithTheOptionOn() throws Exception {
        Telemetry.setAuto(c, true);
        summary(X360);
        assertTrue(GamePerf.runEnded(c, 4242, AutoReports.EXITED, "exited", 1000, 2000));
        assertEquals(1, posted.size());
        assertEquals("game", tx(posted.get(0)).getString("transaction"));
        assertFalse(GamePerf.file(c).exists());
        assertFalse(GamePerf.runEnded(c, 4242, AutoReports.EXITED, "exited", 1000, 2000));   // gone
        assertEquals(1, posted.size());
    }

    @Test
    public void aCrashOrAnotherProcessSendsNothing() throws Exception {
        Telemetry.setAuto(c, true);
        summary(X360);
        assertFalse(GamePerf.runEnded(c, 99, AutoReports.EXITED, "exited", 1000, 2000));
        assertTrue("another process's summary stays", GamePerf.file(c).exists());
        assertFalse(GamePerf.runEnded(c, 4242, AutoReports.CRASHED, "native crash", 1000, 2000));
        assertFalse(GamePerf.file(c).exists());
        summary(X360);
        assertFalse(GamePerf.runEnded(c, 4242, AutoReports.ABNORMAL, "not responding", 1000, 2000));
        assertTrue(posted.isEmpty());
    }

    @Test
    public void neverQueued() throws Exception {
        Telemetry.setAuto(c, true);
        summary(X360);
        netOk = false;   // a metered network without « Also on mobile data », or none
        assertFalse(GamePerf.runEnded(c, 4242, AutoReports.EXITED, "exited", 1000, 2000));
        assertTrue(posted.isEmpty());
        assertFalse(GamePerf.file(c).exists());
        netOk = true;
        CrashReport.transport = (url, h, body) -> {
            throw new java.io.IOException("offline");
        };
        summary(X360);
        assertTrue(GamePerf.runEnded(c, 4242, AutoReports.EXITED, "exited", 1000, 2000));
        File[] q = CrashReport.queueDir(c).listFiles();
        assertTrue("nothing kept for later", q == null || q.length == 0);
        assertEquals(0, AutoReports.pending(c));
    }

    @Test
    public void theRunOfCollectSendsItsSummary() throws Exception {
        List<AutoReports.Exit> exits = new ArrayList<>();
        AutoReports.exits = ctx -> new ArrayList<>(exits);
        try {
            AutoReports.collect(c, false);   // the first look only starts counting
            Telemetry.setAuto(c, true);
            long t = System.currentTimeMillis();
            GameStore.sessionStart(c, t + 1000, "x360-4D5307E6");
            exits.add(new AutoReports.Exit(4242, t + 5000, "exited", 0));
            summary(X360);
            AutoReports.collect(c, false);
            assertEquals(1, posted.size());
            assertEquals("exited", tx(posted.get(0)).getJSONObject("tags").getString("end"));
        } finally {
            AutoReports.exits = AutoReports.SYSTEM_EXITS;
        }
    }

    @Test
    public void theBenchmarkIsTaggedAndSentOnlyOnSend() throws Exception {
        GamePerf.Summary s = GamePerf.parse(X360.replace("\"bench\":0", "\"bench\":1"));
        String h = GamePerf.stateHash("x360-4D5307E6", 2, 1_700_000_000_000L, new int[] { 4, 4, 1, 2 });
        assertEquals(12, h.length());
        assertEquals(h, GamePerf.stateHash("x360-4D5307E6", 2, 1_700_000_000_000L, new int[] { 4, 4, 1, 2 }));
        assertTrue(!h.equals(GamePerf.stateHash("x360-4D5307E6", 3, 1_700_000_000_000L, null)));
        assertEquals("none", GamePerf.stateHash("x360-4D5307E6", 0, 0, null));
        byte[] body = GamePerf.benchEnvelope(c, s, "x360-4D5307E6", h, 1_700_000_100_000L);
        JSONObject tg = tx(body).getJSONObject("tags");
        assertEquals("1", tg.getString("benchmark"));
        assertEquals(h, tg.getString("state"));
        assertFalse(tg.has("end"));
        assertTrue(posted.isEmpty());   // built, not sent
        assertFalse(Telemetry.auto(c));
        assertTrue("the tap is the consent: sent with the option off", CrashReport.sendOnce(c, body));
        assertEquals(1, posted.size());
        assertEquals("29.5 fps (5 %: 22.0) · p99 52.0 ms", GamePerf.resultLine(s));
    }
}
