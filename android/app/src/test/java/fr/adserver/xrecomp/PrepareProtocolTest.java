package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileOutputStream;

/** The progress / result line grammar and status.json (docs/android/prepare.md). Robolectric only for org.json. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class PrepareProtocolTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void progressValid() {
        PrepareProtocol.Progress p =
                PrepareProtocol.parseProgress("XBA_PROGRESS step=4/5 id=compile pct=42 eta=730 msg=func_0001.c");
        assertNotNull(p);
        assertEquals(4, p.step);
        assertEquals(5, p.n);
        assertEquals("compile", p.id);
        assertEquals(42, p.pct);
        assertEquals(730, p.eta);
        assertEquals("func_0001.c", p.msg);
    }

    @Test
    public void progressWithoutEtaOrMsg() {
        PrepareProtocol.Progress p = PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=identify pct=0");
        assertNotNull(p);
        assertEquals(-1, p.eta);
        assertEquals("", p.msg);
    }

    @Test
    public void progressMsgIsGreedy() {
        PrepareProtocol.Progress p = PrepareProtocol.parseProgress(
                "XBA_PROGRESS step=3/5 id=lift pct=10 msg=lifting  sub_0001 eta=99 pct=100 id=x=y");
        assertNotNull(p);
        assertEquals("lifting  sub_0001 eta=99 pct=100 id=x=y", p.msg);
        assertEquals(10, p.pct);
        assertEquals(-1, p.eta);
        assertEquals("lift", p.id);
    }

    @Test
    public void progressUnknownKeysIgnoredAndValuesClamped() {
        PrepareProtocol.Progress p = PrepareProtocol.parseProgress(
                "XBA_PROGRESS future=1 step=2/5 id=analyse rate=3.5 pct=104.6 eta=12.4 bare msg=ok\r");
        assertNotNull(p);
        assertEquals(100, p.pct);
        assertEquals(12, p.eta);
        assertEquals("ok", p.msg);
        assertEquals(2, p.step);
    }

    @Test
    public void progressGarbage() {
        assertNull(PrepareProtocol.parseProgress(null));
        assertNull(PrepareProtocol.parseProgress(""));
        assertNull(PrepareProtocol.parseProgress("hello world"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESSstep=1/5 id=x pct=1"));
        assertNull(PrepareProtocol.parseProgress("  XBA_PROGRESS step=1/5 id=x pct=1"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=lift"));          // no pct
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 pct=3"));            // no id
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS id=lift pct=3"));             // no step
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=x/5 id=lift pct=3"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=6/5 id=lift pct=3"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=1 id=lift pct=3"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=lift pct=lots"));
        assertNull(PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=lift msg=pct=3"));  // pct inside msg
        // a bad eta only drops the eta
        PrepareProtocol.Progress p = PrepareProtocol.parseProgress("XBA_PROGRESS step=1/5 id=lift pct=3 eta=soon");
        assertNotNull(p);
        assertEquals(-1, p.eta);
    }

    @Test
    public void results() {
        PrepareProtocol.Result ok = PrepareProtocol.parseResult("XBA_RESULT ok pack=/data/x/packs/4D530004");
        assertNotNull(ok);
        assertTrue(ok.ok);
        assertEquals("/data/x/packs/4D530004", ok.pack);

        PrepareProtocol.Result err =
                PrepareProtocol.parseResult("XBA_RESULT error code=CC_FAILED msg=clang: error: 3 files failed");
        assertNotNull(err);
        assertFalse(err.ok);
        assertEquals("CC_FAILED", err.code);
        assertEquals("clang: error: 3 files failed", err.msg);

        PrepareProtocol.Result bare = PrepareProtocol.parseResult("XBA_RESULT error");
        assertNotNull(bare);
        assertEquals("UNKNOWN", bare.code);

        assertNull(PrepareProtocol.parseResult("XBA_RESULT maybe code=X"));
        assertNull(PrepareProtocol.parseResult("XBA_RESULTS ok"));
        assertNull(PrepareProtocol.parseResult("XBA_PROGRESS step=1/5 id=lift pct=3"));
    }

    @Test
    public void overall() {
        assertEquals(0f, PrepareProtocol.overall(0, 5, 50), 1e-6);
        assertEquals(0.1f, PrepareProtocol.overall(1, 5, 50), 1e-6);
        assertEquals(0.7f, PrepareProtocol.overall(4, 5, 50), 1e-6);
        assertEquals(1f, PrepareProtocol.overall(5, 5, 100), 1e-6);
    }

    @Test
    public void statusRoundTrip() throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.FAILED;
        s.step = 4;
        s.n = 5;
        s.id = "compile";
        s.pct = 37;
        s.eta = 600;
        s.msg = "clang: « error » in func.c\nsecond line";
        s.code = "CC_FAILED";
        s.startedAt = 1_700_000_000_000L;
        s.updatedAt = 1_700_000_123_456L;
        s.log = "/data/user/0/x/files/prepare/12345678/prepare.log";
        s.jobs = 4;
        s.attempt = 2;
        s.titleId = "12345678";
        File f = new File(tmp.getRoot(), "prepare/12345678/status.json");
        PrepareProtocol.writeStatus(f, s);
        assertTrue(f.isFile());
        assertFalse(new File(f.getPath() + ".tmp").exists());
        PrepareProtocol.Status r = PrepareProtocol.readStatus(f);
        assertNotNull(r);
        assertEquals(s.toJson().toString(), r.toJson().toString());
        assertEquals("compile", r.id);
        assertEquals(1_700_000_123_456L, r.updatedAt);

        // The keys of docs/android/prepare.md
        String text = PrepareProtocol.readText(f, 1 << 16);
        for (String k : new String[] { "state", "step", "n", "id", "pct", "eta", "msg", "code", "started_at",
                                       "updated_at", "log" })
            assertTrue(k, text.contains("\"" + k + "\""));

        // Overwrite (atomic rename over an existing file)
        s.state = PrepareProtocol.DONE;
        PrepareProtocol.writeStatus(f, s);
        assertEquals(PrepareProtocol.DONE, PrepareProtocol.readStatus(f).state);
    }

    @Test
    public void statusMissingOrBroken() throws Exception {
        assertNull(PrepareProtocol.readStatus(new File(tmp.getRoot(), "nope.json")));
        File f = tmp.newFile("broken.json");
        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write("{\"state\": \"runn".getBytes("UTF-8"));
        }
        assertNull(PrepareProtocol.readStatus(f));
    }

    @Test
    public void formatEta() {
        assertEquals("< 1 min", PrepareProtocol.formatEta(30));
        assertEquals("2 min", PrepareProtocol.formatEta(61));
        assertEquals("1 h 05", PrepareProtocol.formatEta(3900));
    }

    @Test
    public void queuedStatus() throws Exception {
        PrepareProtocol.Status s = new PrepareProtocol.Status();
        s.state = PrepareProtocol.QUEUED;
        s.titleId = "4541000D";
        File f = new File(tmp.getRoot(), "prepare/4541000D/status.json");
        PrepareProtocol.writeStatus(f, s);
        PrepareProtocol.Status back = PrepareProtocol.readStatus(f);
        assertTrue(back.queued());
        assertFalse(back.running());
        assertFalse(new PrepareProtocol.Status().queued());
    }

    @Test
    public void queue() throws Exception {
        PrepareProtocol.Queue q = new PrepareProtocol.Queue();
        assertNull(q.pop());
        assertTrue(q.add(new PrepareProtocol.Job("A", "/games/a", "Game A")));
        assertTrue(q.add(new PrepareProtocol.Job("B", "/games/b", "Game B")));
        assertFalse("one entry per title", q.add(new PrepareProtocol.Job("A", "/elsewhere", "again")));
        assertFalse(q.add(new PrepareProtocol.Job("", "/x", "no title ID")));
        assertTrue(q.add(new PrepareProtocol.Job("C", null, null)));
        assertEquals(1, q.position("A"));
        assertEquals(3, q.position("C"));
        assertEquals(0, q.position("Z"));

        File f = PrepareProtocol.queueFile(tmp.getRoot());
        assertEquals(new File(tmp.getRoot(), "prepare/queue.json"), f);
        assertTrue(PrepareProtocol.readQueue(f).jobs.isEmpty());
        PrepareProtocol.writeQueue(f, q);
        PrepareProtocol.Queue back = PrepareProtocol.readQueue(f);
        assertEquals(3, back.jobs.size());
        assertEquals("/games/b", back.jobs.get(1).gameDir);
        assertEquals("Game B", back.jobs.get(1).title);
        assertEquals("", back.jobs.get(2).gameDir);

        assertTrue(back.remove("B"));
        assertFalse(back.remove("B"));
        assertEquals("A", back.pop().titleId);
        assertEquals("C", back.pop().titleId);
        assertNull(back.pop());

        try (FileOutputStream o = new FileOutputStream(f)) {
            o.write("{\"jobs\": [".getBytes("UTF-8"));
        }
        assertTrue(PrepareProtocol.readQueue(f).jobs.isEmpty());
    }
}
