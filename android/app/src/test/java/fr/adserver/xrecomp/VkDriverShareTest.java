package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.os.Bundle;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * VkDriverShare: a game's own app taking XRecomp's driver choice. XRecomp's answer (VkDriverShare.answer) and export
 * (VkDrivers.exportZip) on one store, the game app's apply on another: same id, Origin.inherited, made active;
 * nothing written when unchanged (the native trial marker survives); system clears it; old inherited drivers go.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class VkDriverShareTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    private static final String TID = "4D530004";
    private VkDrivers parent, game;
    private final List<String> opened = new ArrayList<>();

    @Before
    public void setUp() throws IOException {
        parent = new VkDrivers(tmp.newFolder("parent"), 33);
        game = new VkDrivers(tmp.newFolder("game"), 33);
    }

    /** The game app's opener: XRecomp's export of that id (VkDriverProvider.openFile). */
    private VkDriverShare.Opener opener() {
        return id -> {
            opened.add(id);
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            if (!id.equals(parent.activeId()) || !parent.exportZip(id, o))
                throw new java.io.FileNotFoundException(id);
            return new ByteArrayInputStream(o.toByteArray());
        };
    }

    private VkDrivers.Driver installParent(String name) throws IOException {
        Map<String, byte[]> m = VkDriversTest.good();
        m.put("meta.json", VkDriversTest.meta("vulkan.test.so", 30).replace("Turnip Test T1", name)
                                         .getBytes(StandardCharsets.UTF_8));
        return parent.install(new ByteArrayInputStream(VkDriversTest.zip(m)),
                              VkDrivers.Origin.github("MrPurple666/purple-turnip", name, name + ".zip", null, -1));
    }

    @Test
    public void callerTitleId() {
        assertEquals("4D530004", VkDriverShare.callerTitleId("fr.adserver.xrecomp.t4d530004"));
        assertNull(VkDriverShare.callerTitleId("fr.adserver.xrecomp.shell"));
        assertNull(VkDriverShare.callerTitleId("com.example"));
        assertNull(VkDriverShare.callerTitleId(null));
        assertEquals("content://fr.adserver.xrecomp.shell.vkdrivers/0123456789abcdef",
                     VkDriverShare.driverUri("fr.adserver.xrecomp.shell", "0123456789abcdef").toString());
    }

    @Test
    public void answerNamesTheActiveDriver() throws IOException {
        assertEquals("", VkDriverShare.answer(parent, TID).getString(VkDriverShare.KEY_ID));
        VkDrivers.Driver d = installParent("T30");
        assertEquals("", VkDriverShare.answer(parent, TID).getString(VkDriverShare.KEY_ID));   // installed, not chosen
        parent.setActive(d.id);
        Bundle b = VkDriverShare.answer(parent, TID);
        assertEquals(d.id, b.getString(VkDriverShare.KEY_ID));
        assertEquals("MrPurple666/purple-turnip", b.getString(VkDriverShare.KEY_REPO));
        assertEquals("T30", b.getString(VkDriverShare.KEY_TAG));
    }

    @Test
    public void gameAppFollowsXRecomp() throws IOException {
        // XRecomp on the system driver: nothing to do.
        assertEquals(VkDriverShare.Outcome.UNCHANGED, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        assertTrue(opened.isEmpty());

        // A driver chosen in XRecomp: copied once, same id, inherited, active.
        VkDrivers.Driver d = installParent("T30");
        parent.setActive(d.id);
        assertEquals(VkDriverShare.Outcome.INSTALLED, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        VkDrivers.Driver g = game.active();
        assertEquals(d.id, g.id);
        assertEquals(VkDrivers.Origin.INHERITED, g.originKind);
        assertEquals("T30", g.tag);
        assertEquals("T30", g.name);
        assertTrue(Files.isRegularFile(g.library().toPath()));

        // Unchanged: no copy, nothing written; the native trial marker stays (the runtime skips a crashing driver).
        File marker = new File(game.root(), VkDrivers.TRIAL_MARKER);
        Files.write(marker.toPath(), (d.id + "\n").getBytes(StandardCharsets.UTF_8));
        assertEquals(VkDriverShare.Outcome.UNCHANGED, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        assertEquals(1, opened.size());
        assertTrue(marker.isFile());

        // Another driver chosen: the new one copied and active, the old inherited one deleted.
        VkDrivers.Driver d2 = installParent("T31");
        parent.setActive(d2.id);
        assertEquals(VkDriverShare.Outcome.INSTALLED, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        assertEquals(d2.id, game.activeId());
        assertNull(game.read(d.id));
        assertEquals(1, game.list().size());

        // Back to the system driver in XRecomp: cleared, the inherited driver gone.
        parent.clearActive();
        assertEquals(VkDriverShare.Outcome.SYSTEM, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        assertNull(game.activeId());
        assertTrue(game.list().isEmpty());
    }

    @Test
    public void aDriverOfItsOwnIsKeptWhenAlreadyThere() throws IOException {
        VkDrivers.Driver d = installParent("T30");
        parent.setActive(d.id);
        VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener());
        game.clearActive();   // say the setting went missing: selected again without a copy
        assertEquals(VkDriverShare.Outcome.SELECTED, VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener()));
        assertEquals(d.id, game.activeId());
        assertEquals(1, opened.size());
    }

    @Test
    public void noAnswerOrAFailedCopyChangesNothing() throws IOException {
        VkDrivers.Driver d = installParent("T30");
        parent.setActive(d.id);
        VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), opener());
        assertEquals(VkDriverShare.Outcome.NO_ANSWER, VkDriverShare.apply(game, null, opener()));
        assertEquals(VkDriverShare.Outcome.NO_ANSWER,
                     VkDriverShare.apply(game, VkDriverShare.refusal(PackHandover.Refusal.WRONG_CERT), opener()));
        Bundle bad = new Bundle();
        bad.putString(VkDriverShare.KEY_ID, "../x");
        assertEquals(VkDriverShare.Outcome.NO_ANSWER, VkDriverShare.apply(game, bad, opener()));
        assertEquals(d.id, game.activeId());

        Bundle gone = new Bundle();
        gone.putString(VkDriverShare.KEY_ID, "0123456789abcdef");
        assertEquals(VkDriverShare.Outcome.FAILED, VkDriverShare.apply(game, gone, opener()));
        assertEquals("the previous choice kept", d.id, game.activeId());
        assertFalse(new File(game.root(), "0123456789abcdef").exists());
    }

    @Test
    public void gameAppTakesItsTitlesOwnChoice() throws IOException {
        VkDrivers.Driver global = installParent("T30"), own = installParent("R8");
        parent.setActive(global.id);
        // Its own choice in XRecomp wins over the global one, for that title only.
        assertTrue(parent.setGameChoice(TID, own.id));
        assertEquals(own.id, VkDriverShare.answer(parent, TID).getString(VkDriverShare.KEY_ID));
        assertEquals(global.id, VkDriverShare.answer(parent, "4D530005").getString(VkDriverShare.KEY_ID));
        assertEquals(global.id, VkDriverShare.answer(parent, null).getString(VkDriverShare.KEY_ID));
        VkDriverShare.Opener exporter = id -> {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            if (!parent.exportZip(id, o))
                throw new java.io.FileNotFoundException(id);
            return new ByteArrayInputStream(o.toByteArray());
        };
        assertEquals(VkDriverShare.Outcome.INSTALLED,
                     VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), exporter));
        assertEquals(own.id, game.activeId());   // the game app has one choice: its title's
        // « System » for that title: the game app goes back to the system driver.
        assertTrue(parent.setGameChoice(TID, VkDrivers.SYSTEM));
        assertEquals("", VkDriverShare.answer(parent, TID).getString(VkDriverShare.KEY_ID));
        assertEquals(VkDriverShare.Outcome.SYSTEM,
                     VkDriverShare.apply(game, VkDriverShare.answer(parent, TID), exporter));
        assertNull(game.activeId());
    }
}
