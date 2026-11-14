package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.rules.Timeout;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;

/** PrepareRunner against scripted fake commands: progress -> done, an error, a hang -> cancel, a kill -> retry. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareRunnerTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();
    @Rule
    public Timeout timeout = new Timeout(60, TimeUnit.SECONDS);

    private final List<PrepareProtocol.Status> seen = new CopyOnWriteArrayList<>();

    private PrepareRunner runner(File script, int jobs, PrepareRunner.Validator v) {
        File cwd = tmp.getRoot();
        PrepareRunner r = new PrepareRunner(new File(tmp.getRoot(), "prepare/12345678"), "12345678", jobs,
                new PrepareRunner.CommandFactory() {
                    @Override
                    public List<String> command(int j) {
                        return TestSupport.sh(script, String.valueOf(j));
                    }

                    @Override
                    public Map<String, String> environment() {
                        return Collections.singletonMap("XBA_TEST_DIR", cwd.getAbsolutePath());
                    }

                    @Override
                    public File workingDir() {
                        return cwd;
                    }
                }, v, seen::add);
        r.statusIntervalMs = 0;
        r.killGraceMs = 500;
        return r;
    }

    private File script(String body) throws Exception {
        File f = tmp.newFile("fake_prepare_" + System.nanoTime() + ".sh");
        TestSupport.write(f, body);
        return f;
    }

    @Test
    public void progressThenDone() throws Exception {
        File s = script("echo 'python: starting'\n"
                        + "echo 'XBA_PROGRESS step=1/5 id=identify pct=100 msg=Halo (4D530004)'\n"
                        + "echo 'XBA_PROGRESS step=2/5 id=analyse pct=50 eta=600 msg=functions: 1200'\n"
                        + "echo 'something on stderr' >&2\n"
                        + "echo 'XBA_PROGRESS step=4/5 id=compile pct=42 eta=300 msg=jobs '$1\n"
                        + "echo 'XBA_PROGRESS step=5/5 id=pack pct=100 msg=libtitle.so'\n"
                        + "echo \"cwd=$(pwd) env=$XBA_TEST_DIR\"\n"
                        + "echo 'XBA_RESULT ok pack=/x/packs/12345678'\n"
                        + "exit 0\n");
        PrepareRunner r = runner(s, 4, () -> null);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.DONE, end.state);
        assertEquals("", end.code);
        assertEquals(1, end.attempt);
        PrepareProtocol.Status file = PrepareProtocol.readStatus(r.statusFile());
        assertNotNull(file);
        assertEquals(PrepareProtocol.DONE, file.state);
        assertEquals(100, file.pct);
        assertEquals(r.logFile().getAbsolutePath(), file.log);

        boolean sawCompile = false;
        for (PrepareProtocol.Status st : seen) {
            if ("compile".equals(st.id) && st.pct == 42 && st.eta == 300 && "jobs 4".equals(st.msg))
                sawCompile = true;
        }
        assertTrue("the compile progress reached the listener", sawCompile);

        String log = PrepareProtocol.readText(r.logFile(), 1 << 20);
        assertTrue(log, log.contains("python: starting"));
        assertTrue(log, log.contains("something on stderr"));
        assertTrue(log, log.contains("XBA_PROGRESS step=2/5"));
        assertTrue(log, log.contains("cwd=" + tmp.getRoot().getAbsolutePath() + " env=" + tmp.getRoot()));
        assertFalse(log, log.contains("XBA_PGID"));
    }

    @Test
    public void okButPackInvalid() throws Exception {
        File s = script("echo 'XBA_RESULT ok pack=/x'\nexit 0\n");
        PrepareRunner r = runner(s, 2, () -> PrepareRunner.PACK_INVALID);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.FAILED, end.state);
        assertEquals(PrepareRunner.PACK_INVALID, end.code);
    }

    @Test
    public void error() throws Exception {
        File s = script("echo 'XBA_PROGRESS step=4/5 id=compile pct=10 msg=x'\n"
                        + "echo 'XBA_RESULT error code=CC_FAILED msg=clang: 3 files failed'\nexit 1\n");
        PrepareRunner r = runner(s, 2, () -> null);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.FAILED, end.state);
        assertEquals("CC_FAILED", end.code);
        assertEquals("clang: 3 files failed", end.msg);
        assertEquals("compile", end.id);
        assertEquals(PrepareProtocol.FAILED, PrepareProtocol.readStatus(r.statusFile()).state);
    }

    @Test
    public void exitWithoutResult() throws Exception {
        PrepareRunner r = runner(script("echo garbage\nexit 3\n"), 2, () -> null);
        r.begin();
        assertEquals(PrepareRunner.NO_RESULT, r.run().code);
    }

    @Test
    public void spawnFailure() throws Exception {
        PrepareRunner r = new PrepareRunner(new File(tmp.getRoot(), "p"), "1", 1, new PrepareRunner.CommandFactory() {
            @Override
            public List<String> command(int jobs) {
                return Collections.singletonList("/nonexistent/libxbapython.so");
            }

            @Override
            public Map<String, String> environment() {
                return null;
            }

            @Override
            public File workingDir() {
                return new File("/nonexistent-dir");
            }
        }, null, null);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.FAILED, end.state);
        assertTrue(end.code, end.code.equals(PrepareRunner.SPAWN_FAILED) || end.code.equals(PrepareRunner.NO_RESULT));
    }

    /** A hang (a child that also ignores SIGTERM): cancel -> SIGTERM to the group, then SIGKILL; state cancelled. */
    @Test
    public void hangThenCancel() throws Exception {
        File marker = new File(tmp.getRoot(), "child.pid");
        File s = script("echo 'XBA_PROGRESS step=4/5 id=compile pct=5 msg=hanging'\n"
                        + "trap '' TERM\n"
                        + "sh -c 'echo $$ > " + marker.getAbsolutePath() + "; while :; do sleep 1; done' &\n"
                        + "while :; do sleep 1; done\n");
        PrepareRunner r = runner(s, 2, () -> null);
        r.begin();
        PrepareProtocol.Status[] end = new PrepareProtocol.Status[1];
        Thread t = new Thread(() -> end[0] = r.run());
        t.start();
        long until = System.currentTimeMillis() + 10000;
        while (!marker.isFile() && System.currentTimeMillis() < until)
            Thread.sleep(50);
        assertTrue("the fake compiler started", marker.isFile());
        Thread.sleep(200);
        long t0 = System.currentTimeMillis();
        r.cancel();
        t.join(15000);
        assertFalse("the run ended after cancel", t.isAlive());
        assertEquals(PrepareProtocol.CANCELLED, end[0].state);
        assertTrue("SIGKILL came after the grace period", System.currentTimeMillis() - t0 >= 400);
        // The grandchild (a « compiler ») died with the group.
        String pid = PrepareProtocol.readText(marker, 100).trim();
        assertFalse("grandchild " + pid + " still alive", new File("/proc/" + pid).exists()
                                                         && !isZombie(pid));
        assertEquals(PrepareProtocol.CANCELLED, PrepareProtocol.readStatus(r.statusFile()).state);
        String log = PrepareProtocol.readText(r.logFile(), 1 << 20);
        assertTrue(log, log.contains("SIGKILL"));
    }

    private static boolean isZombie(String pid) {
        try {
            String st = PrepareProtocol.readText(new File("/proc/" + pid + "/stat"), 4096);
            return st.substring(st.lastIndexOf(')') + 2).startsWith("Z");
        } catch (Exception e) {
            return true;
        }
    }

    @Test
    public void cancelBeforeStart() throws Exception {
        PrepareRunner r = runner(script("sleep 30\n"), 2, () -> null);
        r.begin();
        r.cancel();
        long t0 = System.currentTimeMillis();
        assertEquals(PrepareProtocol.CANCELLED, r.run().state);
        assertTrue(System.currentTimeMillis() - t0 < 10000);
    }

    /** Killed by a signal once (the phantom-process killer): one retry with half the jobs, then done. */
    @Test
    public void killThenRetryWithHalfTheJobs() throws Exception {
        File jobsLog = new File(tmp.getRoot(), "jobs.txt");
        File once = new File(tmp.getRoot(), "killed-once");
        File s = script("echo $1 >> " + jobsLog.getAbsolutePath() + "\n"
                        + "if [ ! -f " + once.getAbsolutePath() + " ]; then\n"
                        + "  touch " + once.getAbsolutePath() + "\n"
                        + "  echo 'XBA_PROGRESS step=4/5 id=compile pct=30 msg=about to die'\n"
                        + "  kill -9 $$\n"
                        + "fi\n"
                        + "echo 'XBA_PROGRESS step=5/5 id=pack pct=100 msg=ok'\n"
                        + "echo 'XBA_RESULT ok pack=/x'\n");
        PrepareRunner r = runner(s, 6, () -> null);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.DONE, end.state);
        assertEquals(2, end.attempt);
        assertEquals(3, end.jobs);
        assertEquals("6\n3\n", PrepareProtocol.readText(jobsLog, 100));
        String log = PrepareProtocol.readText(r.logFile(), 1 << 20);
        assertTrue(log, log.contains("killed by signal 9"));
    }

    @Test
    public void killedTwiceFails() throws Exception {
        File s = script("kill -9 $$\n");
        PrepareRunner r = runner(s, 1, () -> null);
        r.begin();
        PrepareProtocol.Status end = r.run();
        assertEquals(PrepareProtocol.FAILED, end.state);
        assertEquals(PrepareRunner.KILLED, end.code);
        assertEquals(2, end.attempt);
        assertEquals(1, end.jobs);
    }

    @Test
    public void signalDecoding() {
        assertEquals(9, PrepareRunner.signalOf(137));
        assertEquals(9, PrepareRunner.signalOf(9));
        assertEquals(15, PrepareRunner.signalOf(143));
        assertEquals(11, PrepareRunner.signalOf(139));
        assertEquals(0, PrepareRunner.signalOf(1));
        assertEquals(0, PrepareRunner.signalOf(0));
        assertEquals(0, PrepareRunner.signalOf(130));
        assertEquals(0, PrepareRunner.signalOf(6));
    }
}
