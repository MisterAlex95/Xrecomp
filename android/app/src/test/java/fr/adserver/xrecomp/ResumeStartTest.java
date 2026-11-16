package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** A GBA / PS1 / N64 game's Home: Resume when its automatic slot is saved, Start launches once from the boot. */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ResumeStartTest {
    private final Context c = RuntimeEnvironment.getApplication();

    @Test
    public void resumeOnlyWithASavedPointAndTheSettingOn() throws Exception {
        String tid = "gba-TEST1234";
        assertFalse(GameStore.canResume(c, tid));
        File st = GameStore.resumeState(c, tid);
        assertTrue(st.getParentFile().mkdirs() || st.getParentFile().isDirectory());
        new FileOutputStream(st).close();
        assertTrue(GameStore.canResume(c, tid));
        GameStore.setAutoResume(c, false);
        assertFalse(GameStore.canResume(c, tid));
        GameStore.setAutoResume(c, true);
        assertTrue(GameStore.canResume(c, tid));
        assertFalse(GameStore.canResume(c, null));
    }

    @Test
    public void freshStartIsOneShot() {
        assertFalse(GameStore.takeFreshStart(c));
        GameStore.requestFreshStart(c);
        assertTrue(GameStore.takeFreshStart(c));
        assertFalse(GameStore.takeFreshStart(c));   // the next process (Restart, another launch) resumes again
    }

    /** XBA-265: an Xbox title's runtime leaves why the automatic slot was not written (auto.why) for its Home. */
    @Test
    public void xboxWhyNotSaved() throws Exception {
        String tid = "xbox-4D530004";
        assertNull(GameStore.resumeWhyNot(c, tid));
        File why = new File(GameStore.resumeState(c, tid).getParentFile(), "auto.why");
        assertTrue(why.getParentFile().mkdirs() || why.getParentFile().isDirectory());
        Files.write(why.toPath(), "guest thread 3: a return address no call site names\n".getBytes(StandardCharsets.UTF_8));
        assertEquals("guest thread 3: a return address no call site names", GameStore.resumeWhyNot(c, tid));
        Files.write(why.toPath(), "  \n".getBytes(StandardCharsets.UTF_8));
        assertNull(GameStore.resumeWhyNot(c, tid));
        assertNull(GameStore.resumeWhyNot(c, null));
    }
}
