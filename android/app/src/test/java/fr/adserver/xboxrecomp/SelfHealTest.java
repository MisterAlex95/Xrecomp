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

/** XBA-74: what a crashed run found since the last Prepare, and the offer made of it. */
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

    @Test
    public void roundsAreCapped() throws Exception {
        File work = tmp.newFolder("w");
        long t = 1000;
        for (int round = 0; round < SelfHeal.MAX_ROUNDS; round++) {
            SelfHeal.launched(work, t++);
            append(work, RUN + "{\"t\":\"icall\",\"va\":\"0x0010000" + round + "\",\"ret\":\"0x00123456\"}\n");
            assertTrue(SelfHeal.onExit(work, crash(t++)));
            assertEquals(SelfHeal.Offer.PREPARE_AGAIN, SelfHeal.read(work).offer);
            SelfHeal.prepareAgain(work);
            assertTrue(SelfHeal.read(work).relaunch);
            SelfHeal.prepareStarted(work);
        }
        SelfHeal.launched(work, t++);
        append(work, RUN + ICALL);
        assertTrue(SelfHeal.onExit(work, crash(t++)));
        SelfHeal.State s = SelfHeal.read(work);
        assertEquals(SelfHeal.Offer.NEEDS_FIX, s.offer);
        assertTrue(s.blocker, s.blocker.contains("after 3 Prepare again rounds"));
        assertFalse(s.relaunch);
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
        SelfHeal.prepareAgain(work);
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
}
