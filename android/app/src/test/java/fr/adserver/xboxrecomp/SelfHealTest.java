package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** XBA-74, XBA-129: what a run found since the last Prepare, the offer made of it, and the rounds. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class SelfHealTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final String RUN = "{\"t\":\"run\",\"time\":\"2026-11-12T10:00:00Z\"}\n";
    private static final String ICALL = "{\"t\":\"icall\",\"va\":\"0x0012ABCD\",\"ret\":\"0x00123456\"}\n";
    private static final String THREAD = "{\"t\":\"thread\",\"va\":\"0x00012345\",\"ctx1\":\"0x00045678\"}\n";
    private static final String UNIMPL = "{\"t\":\"unimpl\",\"va\":\"0x00012345\",\"insn\":\"bsf eax, ecx\"}\n";
    private static final String D3D = "{\"t\":\"d3d\",\"va\":\"0x0085FDE0\",\"slot\":\"0x00862288\"}\n";
    private static final String STACK_SWITCH = "{\"t\":\"stack_switch\",\"va\":\"0x00023454\",\"to\":\"0x00023481\","
            + "\"why\":\"resumes where no context is parked and no function starts\"}\n";

    private void append(File work, String text) throws Exception {
        assertTrue(work.mkdirs() || work.isDirectory());
        try (FileOutputStream out = new FileOutputStream(SelfHeal.discoveryFile(work), true)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static SelfHeal.Exit crash(long t) {
        return new SelfHeal.Exit(t, true, "signal 11 (SIGSEGV)");
    }

    @Test
    public void scanCountsOnlyWhatThePrepareCanSeed() throws Exception {
        File work = tmp.newFolder("w");
        append(work, RUN + ICALL + D3D + THREAD + UNIMPL + "not json\n" + "{\"t\":\"thread_ctx\"");   // torn last line
        SelfHeal.Finds f = SelfHeal.scan(SelfHeal.discoveryFile(work), 0);
        assertEquals(2, f.seeds);
        assertEquals(1, f.unimpl);
        assertEquals("bsf eax, ecx at 0x00012345", f.firstUnimpl);
    }

    @Test
    public void taintedEntriesAreNoSeeds() throws Exception {
        // What the Prepare's gate holds back (tools/seed_from_log, profile-free WP1) is no progress for it.
        File work = tmp.newFolder("w");
        append(work, RUN + "{\"t\":\"icall\",\"va\":\"0x0012ABCD\",\"ret\":\"0x00123456\",\"tainted\":true}\n"
                     + "{\"t\":\"icall\",\"va\":\"0x0012ABD0\",\"ret\":\"0x00123456\",\"tainted\":false}\n");
        assertEquals(1, SelfHeal.scan(SelfHeal.discoveryFile(work), 0).seeds);
        SelfHeal.prepareStarted(work);
        append(work, RUN + "{\"t\":\"icall\",\"va\":\"0x0012ABE0\",\"ret\":\"0x00123456\",\"tainted\":true}\n");
        SelfHeal.launched(work, 1000);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        assertEquals(SelfHeal.Offer.NEEDS_FIX, SelfHeal.read(work).offer);
    }

    @Test
    public void onlyWhatFollowsThePrepareIsNew() throws Exception {
        File work = tmp.newFolder("w");
        append(work, RUN + ICALL);
        SelfHeal.prepareStarted(work);
        SelfHeal.launched(work, 1000);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertTrue(s.blocker, s.blocker.contains("nothing new found"));
        assertTrue(s.blocker, s.blocker.contains("SIGSEGV"));

        append(work, RUN + THREAD);
        SelfHeal.launched(work, 3000);
        assertTrue(SelfHeal.onExit(work, crash(4000)));
        s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.PREPARE_AGAIN, s.offer);
        assertEquals(1, s.seeds);
    }

    /** tools.prepare kept n seeds from discovered.jsonl (discovered-seeds.json). */
    private static void seeds(File work, int n) throws Exception {
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < n; i++)
            b.append(i == 0 ? "" : ",").append("{\"start\":\"0x0010").append(String.format("%04X", i)).append("\"}");
        TestSupport.write(new File(work, SelfHeal.SEEDS), b.append("]").toString());
    }

    /** XBA-129: no cap on rounds as such: every round that takes in new code may be followed by another. */
    @Test
    public void roundsWithProgressAreNotCapped() throws Exception {
        File work = tmp.newFolder("w");
        long t = 1000;
        for (int round = 0; round < 6; round++) {
            SelfHeal.launched(work, t++);
            append(work, RUN + "{\"t\":\"icall\",\"va\":\"0x0010000" + round + "\",\"ret\":\"0x00123456\"}\n");
            assertTrue(SelfHeal.onExit(work, crash(t++)));
            assertEquals("round " + round, SelfHeal.Offer.PREPARE_AGAIN, SelfHeal.read(work).offer);
            SelfHeal.prepareAgain(work);
            SelfHeal.prepareStarted(work);
            seeds(work, round + 1);   // the Prepare took the new address
            assertTrue("relaunch after a round with progress", SelfHeal.roundEnded(work, true));
            assertFalse("a round ends once", SelfHeal.roundEnded(work, true));
            assertEquals(round + 1, SelfHeal.read(work).rounds);
            assertEquals(0, SelfHeal.read(work).idle);
        }
    }

    /** XBA-129: the cap is on rounds without progress: the Prepare took none of the new addresses. */
    @Test
    public void aRoundWithoutProgressStopsTheLoop() throws Exception {
        File work = tmp.newFolder("w");
        seeds(work, 2);
        SelfHeal.launched(work, 1000);
        append(work, RUN + ICALL);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.prepareAgain(work);
        SelfHeal.prepareStarted(work);
        // tools.prepare's gates refused the address: discovered-seeds.json is as it was.
        assertFalse("no relaunch: the same pack would crash the same way", SelfHeal.roundEnded(work, true));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertEquals(1, s.idle);
        assertTrue(s.blocker, s.blocker.contains("none of them code"));
        assertFalse(s.relaunch);
    }

    /** XBA-129: the player's Stop (a cancelled Prepare) ends the round quietly, nothing starts. */
    @Test
    public void aStoppedRoundStartsNothing() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + ICALL);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.prepareAgain(work);
        assertFalse(SelfHeal.roundEnded(work, false));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NONE, s.offer);
        assertFalse(s.relaunch || s.roundOpen);
    }

    /** XBA-129: a run that ended well but skipped code (an indirect call, a thread): a background round. */
    @Test
    public void aGoodRunWithNewCodeImprovesInTheBackground() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + ICALL + THREAD);
        assertTrue(SelfHeal.onExit(work, new SelfHeal.Exit(2000, false, "")));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.IMPROVE, s.offer);
        assertEquals(2, s.seeds);
        SelfHeal.startRound(work, true);
        s = SelfHeal.read(work);
        assertTrue(s.roundOpen && s.background);
        assertFalse("nothing starts by itself after a background round", s.relaunch);
        seeds(work, 2);
        assertFalse(SelfHeal.roundEnded(work, true));
        s = SelfHeal.read(work);
        assertEquals(2, s.lastAccepted);
        assertFalse(s.roundOpen || s.background);
        // The next good run met nothing new: the rounds end.
        SelfHeal.prepareStarted(work);
        SelfHeal.launched(work, 3000);
        assertTrue(SelfHeal.onExit(work, new SelfHeal.Exit(4000, false, "")));
        s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NONE, s.offer);
        assertEquals(0, s.rounds);
    }

    /** XBA-129: never a round on what a Prepare cannot fix, even with new code in the same run. */
    @Test
    public void aGuestExceptionIsTheBlocker() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + ICALL + "{\"t\":\"exception\",\"va\":\"0x00011020\",\"code\":\"0xC0000094\","
                + "\"why\":\"divide by zero\"}\n");
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertEquals("guest exception 0xC0000094 at 0x00011020: divide by zero", s.blocker);
    }

    @Test
    public void aStubIsNamedWhenNothingElseIs() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + "{\"t\":\"stub\",\"ord\":136,\"name\":\"KeRemoveQueue\",\"ret\":\"0x00012345\"}\n");
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertEquals("crash (signal 11 (SIGSEGV)), nothing new found; calls stub ordinal 136 (KeRemoveQueue)",
                     s.blocker);
    }

    @Test
    public void theSettingIsOnByDefault() {
        android.app.Application app = org.robolectric.RuntimeEnvironment.getApplication();
        assertTrue(SelfHeal.auto(app));
        SelfHeal.setAuto(app, false);
        assertFalse(SelfHeal.auto(app));
    }

    @Test
    public void aStackSwitchTheRuntimeCannotCarryIsTheBlocker() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + STACK_SWITCH);
        SelfHeal.Finds f = SelfHeal.scan(SelfHeal.discoveryFile(work), 0);
        assertEquals(0, f.seeds);
        assertEquals(1, f.stackSwitch);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertEquals("stack switch at 0x00023454 to 0x00023481: resumes where no context is parked and no function "
                + "starts", s.blocker);
    }

    @Test
    public void anUntranslatedInstructionIsTheBlocker() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.launched(work, 1000);
        append(work, RUN + UNIMPL);
        assertTrue(SelfHeal.onExit(work, crash(2000)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertEquals("untranslated instruction: bsf eax, ecx at 0x00012345", s.blocker);
    }

    @Test
    public void aCleanExitEndsTheRoundsAndOldExitsAreIgnored() throws Exception {
        File work = tmp.newFolder("w");
        SelfHeal.prepareAgain(work);
        SelfHeal.roundEnded(work, true);
        SelfHeal.prepareAgain(work);
        SelfHeal.roundEnded(work, true);
        SelfHeal.launched(work, 5000);
        assertFalse("an exit before the launch is another run's", SelfHeal.onExit(work, crash(4000)));
        assertFalse(SelfHeal.onExit(work, null));
        assertTrue(SelfHeal.onExit(work, new SelfHeal.Exit(6000, false, "")));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(0, s.rounds);
        assertEquals(SelfHeal.Offer.NONE, s.offer);
        assertFalse("handled once", SelfHeal.onExit(work, new SelfHeal.Exit(6000, true, "")));
    }

    @Test
    public void aReplacedFileIsReadWhole() throws Exception {
        File work = tmp.newFolder("w");
        append(work, RUN + ICALL + THREAD + ICALL);
        SelfHeal.prepareStarted(work);
        assertTrue(SelfHeal.discoveryFile(work).delete());
        append(work, RUN + ICALL);
        assertEquals(1, SelfHeal.scan(SelfHeal.discoveryFile(work), SelfHeal.read(work).offset).seeds);
    }

    /** Wave 3: the same words as tools/compat/blockers.py (tools/compat/test_blockers.py pins the same strings). */
    private static final String BLOCKER_FIXTURE = RUN
            + "{\"t\":\"smc\",\"va\":\"0x00400000\",\"kind\":\"exec_outside_image\",\"ret\":\"0x00011234\","
            + "\"where\":\"heap\",\"bytes\":\"5589E583EC10C390\"}\n"
            + "{\"t\":\"smc\",\"va\":\"0x00012000\",\"kind\":\"code_write\",\"at\":\"0x00012345\","
            + "\"section\":\".text\",\"by\":\"a store\",\"ret\":\"0x00011000\"}\n"
            + "{\"t\":\"stub\",\"ord\":136,\"name\":\"KeRemoveQueue\",\"ret\":\"0x00012345\"}\n"
            + "{\"t\":\"stub\",\"ord\":206,\"name\":\"NtQueueApcThread\",\"ret\":\"0x00012399\"}\n"
            + "{\"t\":\"exception\",\"va\":\"0x00040000\",\"code\":\"0xC0000005\","
            + "\"why\":\"host fault on a guest address no guest handler took\"}\n"
            + "{\"t\":\"exception\",\"va\":\"0x00011020\",\"code\":\"0xC0000094\",\"why\":\"divide by zero\"}\n";

    @Test
    public void theBlockerWordsAreTheDesktops() throws Exception {
        File work = tmp.newFolder("w");
        append(work, BLOCKER_FIXTURE);
        SelfHeal.Finds f = SelfHeal.scan(SelfHeal.discoveryFile(work), 0);
        assertEquals("guest exception 0xC0000005 at 0x00040000: host fault on a guest address no guest handler took (+1)",
                     SelfHeal.fatal(f));
        assertEquals("code written or run at run time x2: exec_outside_image 0x00400000 (heap, from 0x00011234), "
                     + "code_write 0x00012345 (.text)", SelfHeal.smcWords(f));
        assertEquals("calls stub ordinal 136 (KeRemoveQueue), ordinal 206 (NtQueueApcThread)", SelfHeal.stubWords(f));
        // Code written at run time alone is « needs a fix » too: a Prepare cannot make the lifted C run it.
        File w2 = tmp.newFolder("w2");
        SelfHeal.launched(w2, 1000);
        append(w2, RUN + ICALL + BLOCKER_FIXTURE.split("\n")[2] + "\n");
        assertTrue(SelfHeal.onExit(w2, crash(2000)));
        assertEquals(SelfHeal.Offer.NEEDS_FIX, SelfHeal.read(w2).offer);
        assertEquals("code written or run at run time x1: code_write 0x00012345 (.text)", SelfHeal.read(w2).blocker);
    }
}
