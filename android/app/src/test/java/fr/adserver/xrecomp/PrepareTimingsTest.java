package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.rules.Timeout;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/** XBA-341: the Prepare's stages from its protocol lines, the « prepare » transaction, and the gate. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareTimingsTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();
    @Rule
    public Timeout timeout = new Timeout(60, TimeUnit.SECONDS);

    private Context c;
    private final List<byte[]> posted = new ArrayList<>();
    private long now = 1_700_000_000_000L;
    private File cpuRootWas;

    @Before
    public void setUp() {
        c = RuntimeEnvironment.getApplication();
        CrashReport.transport = (url, h, body) -> {
            posted.add(body);
            return 200;
        };
        cpuRootWas = PrepareTimings.cpuRoot;
    }

    @After
    public void tearDown() {
        CrashReport.transport = CrashReport.HTTP;
        PrepareTimings.cpuRoot = cpuRootWas;
        File[] f = Telemetry.dir(c).listFiles();
        if (f != null)
            for (File x : f)
                x.delete();
        Telemetry.dir(c).delete();
    }

    private PrepareTimings timings() {
        return new PrepareTimings(() -> now);
    }

    private static List<String> ops(List<CrashReport.Span> spans) {
        List<String> o = new ArrayList<>();
        for (CrashReport.Span s : spans)
            o.add(s.op);
        return o;
    }

    @Test
    public void stagesFollowTheProtocolLines() {
        PrepareTimings t = timings();
        t.begin();                                   // install
        now += 3_000;
        t.stage(PrepareTimings.STAGE_LAUNCH);
        now += 1_000;
        t.line("python: starting");                  // not a progress line
        t.line("XBA_PROGRESS step=1/5 id=identify pct=0 msg=finding default.xbe");
        now += 2_000;
        t.line("XBA_PROGRESS step=1/5 id=identify pct=100 msg=Halo (4D530004)");   // same stage
        t.line("XBA_PROGRESS step=2/5 id=analyse pct=0 msg=analysing: disasm");
        now += 60_000;
        t.line("XBA_PROGRESS step=3/5 id=lift pct=0 msg=x");
        now += 30_000;
        t.line("XBA_PROGRESS step=4/5 id=compile pct=0 msg=x");
        now += 300_000;
        t.line("XBA_PROGRESS garbage");
        t.line("XBA_PROGRESS step=5/5 id=pack pct=0 msg=linking libtitle.so");
        now += 5_000;
        t.end();
        now += 9_000;
        t.stage("identify");                         // after the end: nothing
        t.end();
        List<CrashReport.Span> s = t.spans();
        assertEquals(Arrays.asList("prepare.install", "prepare.launch", "prepare.identify", "prepare.analyse",
                                   "prepare.lift", "prepare.compile", "prepare.pack"), ops(s));
        long[] secs = { 3, 1, 2, 60, 30, 300, 5 };
        for (int i = 0; i < secs.length; i++)
            assertEquals(s.get(i).op, secs[i] * 1000, s.get(i).endMs - s.get(i).startMs);
        for (int i = 1; i < s.size(); i++)
            assertEquals(s.get(i - 1).endMs, s.get(i).startMs);   // no gaps
        assertEquals(401_000, t.endedAt() - t.startedAt());
    }

    @Test
    public void aRetryRepeatsStagesAndOddIdsAreCleaned() {
        PrepareTimings t = timings();
        t.begin();
        t.stage("launch");
        now += 10;
        t.stage("compile");
        now += 1_000;
        t.stage("launch");                           // attempt 2 after a kill
        now += 10;
        t.stage("identify");
        now += 10;
        t.stage("Weird/Id with spaces");
        t.stage("");                                 // ignored
        now -= 500;                                  // the wall clock stepped back
        t.end();
        List<CrashReport.Span> s = t.spans();
        assertEquals(Arrays.asList("prepare.install", "prepare.launch", "prepare.compile", "prepare.launch",
                                   "prepare.identify", "prepare.weirdidwithspaces"), ops(s));
        for (CrashReport.Span x : s)
            assertTrue(x.op, x.endMs >= x.startMs);
        assertEquals("", PrepareTimings.clean(null));
    }

    @Test
    public void notBegunMakesNoTransaction() {
        PrepareProtocol.Status end = new PrepareProtocol.Status();
        assertNull(PrepareTimings.envelope(c, timings(), end, 8, 4));
    }

    @Test
    public void theTransactionHasTheStagesAndNoTitleOrPath() throws Exception {
        PrepareTimings t = timings();
        t.begin();
        now += 2_000;
        t.stage("identify");
        now += 8_000;
        t.stage("lift");
        now += 20_000;
        t.end();
        PrepareProtocol.Status end = new PrepareProtocol.Status();
        end.state = PrepareProtocol.FAILED;
        end.code = "LIFT_FAILED";
        end.msg = "Halo: tools.recomp failed, see /data/user/0/fr.adserver.xrecomp/files/work/4D530004/prepare.log";
        end.titleId = "4D530004";
        end.jobs = 6;
        end.opt = 2;
        end.background = true;
        byte[] body = PrepareTimings.envelope(c, t, end, 8, 3);
        assertNotNull(body);
        String all = new String(body, StandardCharsets.UTF_8);
        assertFalse(all, all.contains("Halo"));
        assertFalse(all, all.contains("/data/"));
        assertFalse(all, all.contains("prepare.log"));

        List<Object[]> it = CrashReportTest.items(body);
        assertEquals("transaction", ((JSONObject)it.get(1)[0]).getString("type"));
        JSONObject tx = new JSONObject(new String((byte[])it.get(1)[1], StandardCharsets.UTF_8));
        assertEquals("prepare", tx.getString("transaction"));
        JSONObject trace = tx.getJSONObject("contexts").getJSONObject("trace");
        assertEquals("prepare", trace.getString("op"));
        assertEquals("internal_error", trace.getString("status"));
        assertEquals(30.0, tx.getDouble("timestamp") - tx.getDouble("start_timestamp"), 1e-6);
        JSONArray sp = tx.getJSONArray("spans");
        assertEquals(3, sp.length());
        assertEquals("prepare.install", sp.getJSONObject(0).getString("op"));
        assertEquals("prepare.identify", sp.getJSONObject(1).getString("op"));
        assertEquals("prepare.lift", sp.getJSONObject(2).getString("op"));
        assertEquals(20.0, sp.getJSONObject(2).getDouble("timestamp")
                           - sp.getJSONObject(2).getDouble("start_timestamp"), 1e-6);
        for (int i = 0; i < sp.length(); i++)
            assertEquals(trace.getString("span_id"), sp.getJSONObject(i).getString("parent_span_id"));
        JSONObject tags = tx.getJSONObject("tags");
        assertEquals("xbox", tags.getString("console"));
        assertEquals("4D530004", tags.getString("game_key"));
        assertEquals("LIFT_FAILED", tags.getString("result"));
        assertEquals("8", tags.getString("cores"));
        assertEquals("3", tags.getString("big_cores"));
        assertEquals("background", tags.getString("prepare_kind"));
        assertEquals("O2", tags.getString("opt"));
        assertEquals("6", tags.getString("jobs"));
        assertTrue(tags.has("device"));
        assertTrue(tags.has("abi"));
        assertFalse(tags.has("game"));
        assertFalse(tags.has("kind"));
        assertTrue(tx.getString("release").startsWith("xrecomp@"));
    }

    @Test
    public void statusAndResultFollowTheEnd() {
        assertEquals("ok", PrepareTimings.txStatus(PrepareProtocol.DONE));
        assertEquals("cancelled", PrepareTimings.txStatus(PrepareProtocol.CANCELLED));
        assertEquals("internal_error", PrepareTimings.txStatus(PrepareProtocol.FAILED));
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.titleId = "n64-1a2b3c4d";
        s.state = PrepareProtocol.DONE;
        Map<String, String> t = PrepareTimings.tags(s, 8, -1);
        assertEquals("ok", t.get("result"));
        assertEquals("n64", t.get("console"));
        assertFalse(t.containsKey("big_cores"));
        s.state = PrepareProtocol.CANCELLED;
        s.code = PrepareRunner.CANCELLED_CODE;
        assertEquals("CANCELLED", PrepareTimings.tags(s, 8, -1).get("result"));
    }

    @Test
    public void bigCoresFromCapacityOrFrequency() throws Exception {
        File root = tmp.newFolder("cpu");
        PrepareTimings.cpuRoot = root;
        long[] cap = { 300, 300, 300, 300, 800, 800, 800, 1024 };
        for (int i = 0; i < cap.length; i++) {
            File d = new File(root, "cpu" + i);
            new File(d, "cpufreq").mkdirs();
            TestSupport.write(new File(d, "cpu_capacity"), cap[i] + "\n");
            TestSupport.write(new File(d, "cpufreq/cpuinfo_max_freq"), "2000000\n");
        }
        new File(root, "cpufreq").mkdirs();   // not a core
        assertEquals(4, PrepareTimings.bigCores());
        for (int i = 0; i < cap.length; i++)
            new File(root, "cpu" + i + "/cpu_capacity").delete();
        assertEquals(8, PrepareTimings.bigCores());   // all alike
        PrepareTimings.cpuRoot = new File(root, "missing");
        assertEquals(-1, PrepareTimings.bigCores());
    }

    @Test
    public void gateOffSendsNothingGateOnSendsOne() {
        PrepareTimings t = timings();
        t.begin();
        now += 1_000;
        t.end();
        PrepareProtocol.Status end = new PrepareProtocol.Status();
        end.state = PrepareProtocol.DONE;
        end.titleId = "4D530004";
        assertFalse(Telemetry.auto(c));
        assertFalse(PrepareTimings.send(c, t, end));
        assertTrue(posted.isEmpty());

        Telemetry.setAuto(c, true);
        assertTrue(PrepareTimings.send(c, t, end));
        assertEquals(1, posted.size());
        assertTrue(new String(posted.get(0), StandardCharsets.UTF_8).contains("\"prepare.install\""));
    }

    @Test
    public void theRunnerRecordsItsStages() throws Exception {
        File script = tmp.newFile("fake_prepare.sh");
        TestSupport.write(script, "echo 'python: starting'\n"
                                  + "echo 'XBA_PROGRESS step=1/5 id=identify pct=100 msg=Halo (4D530004)'\n"
                                  + "echo 'XBA_PROGRESS step=2/5 id=analyse pct=50 msg=functions: 1200'\n"
                                  + "echo 'XBA_PROGRESS step=3/5 id=lift pct=50 msg=x'\n"
                                  + "echo 'XBA_PROGRESS step=4/5 id=compile pct=42 msg=x'\n"
                                  + "echo 'XBA_PROGRESS step=5/5 id=pack pct=100 msg=libtitle.so'\n"
                                  + "echo 'XBA_RESULT ok pack=/x/packs/4D530004'\nexit 0\n");
        File cwd = tmp.getRoot();
        PrepareRunner r = new PrepareRunner(new File(cwd, "prepare/4D530004"), "4D530004", 2,
                new PrepareRunner.CommandFactory() {
                    @Override
                    public List<String> command(int j) {
                        return TestSupport.sh(script, String.valueOf(j));
                    }

                    @Override
                    public Map<String, String> environment() {
                        return Collections.emptyMap();
                    }

                    @Override
                    public File workingDir() {
                        return cwd;
                    }
                }, () -> null, null);
        r.begin();
        r.installProgress(100, "tools");
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.DONE, end.state);
        assertEquals(Arrays.asList("prepare.install", "prepare.launch", "prepare.identify", "prepare.analyse",
                                   "prepare.lift", "prepare.compile", "prepare.pack"), ops(r.timings().spans()));
        assertTrue(r.timings().endedAt() >= r.timings().startedAt());
    }
}
