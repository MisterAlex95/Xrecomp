package fr.adserver.xboxrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;

/** The live save caches move aside when the launched title changes, and come back (plain JUnit). */
public class SaveCachesTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File save, owner;
    private static final String FABLE = "4D5300D1", OTHER = "4541000D";

    @Before
    public void setUp() {
        save = new File(tmp.getRoot(), "save");
        owner = new File(tmp.getRoot(), "library/cache_owner");
    }

    private void liveSet(String tag) throws Exception {
        TestSupport.write(new File(save, "Cache/unpacked.bin"), tag);
        TestSupport.write(new File(save, "TitleCache/LocalCache00.bin"), tag);
        TestSupport.write(new File(save, "Partition3.img"), tag);
        TestSupport.write(new File(save, "Partition4.img"), tag);
        TestSupport.write(new File(save, "Partition5.img"), tag);
    }

    private String read(File f) throws Exception {
        return PrepareProtocol.readText(f, 1 << 16);
    }

    @Test
    public void noOwnerAdoptsTheLiveSet() throws Exception {
        liveSet("fable");
        TestSupport.write(new File(save, "launch_data.bin"), "x");
        assertFalse(SaveCaches.switchTo(save, owner, FABLE));
        assertEquals(FABLE, SaveCaches.owner(owner));
        assertEquals("fable", read(new File(save, "Cache/unpacked.bin")));
        assertTrue("nothing changed hands", new File(save, "launch_data.bin").isFile());
    }

    @Test
    public void sameOwnerMovesNothing() throws Exception {
        liveSet("fable");
        SaveCaches.writeOwner(owner, FABLE);
        assertFalse(SaveCaches.switchTo(save, owner, FABLE));
        assertEquals("fable", read(new File(save, "Partition3.img")));
        assertFalse(new File(save, SaveCaches.STASH).exists());
    }

    @Test
    public void switchStashesAndRestores() throws Exception {
        liveSet("fable");
        TestSupport.write(new File(save, "launch_data.bin"), "fable reboot");
        TestSupport.write(new File(save, "UDATA/4d5300d1/save.dat"), "my save");
        SaveCaches.writeOwner(owner, FABLE);

        assertTrue(SaveCaches.switchTo(save, owner, OTHER));
        assertEquals(OTHER, SaveCaches.owner(owner));
        for (String n : SaveCaches.LIVE)
            assertFalse(n, new File(save, n).exists());
        assertFalse(new File(save, "launch_data.bin").exists());
        File stash = SaveCaches.stashDir(save, FABLE);
        assertEquals("fable", read(new File(stash, "Cache/unpacked.bin")));
        assertEquals("fable", read(new File(stash, "Partition5.img")));
        assertEquals("saves never move", "my save", read(new File(save, "UDATA/4d5300d1/save.dat")));

        // The other title runs and makes its own caches.
        liveSet("other");
        assertTrue(SaveCaches.switchTo(save, owner, FABLE));
        // Stashed before per-title Z: (K-9): back in Cache/<title id>.
        assertEquals("fable", read(new File(save, "Cache/4d5300d1/unpacked.bin")));
        assertFalse(new File(save, "Cache/unpacked.bin").exists());
        assertEquals("fable", read(new File(save, "TitleCache/LocalCache00.bin")));
        assertEquals("other", read(new File(SaveCaches.stashDir(save, OTHER), "Partition4.img")));
        assertFalse("an emptied stash is removed", stash.exists());
        assertEquals(FABLE, SaveCaches.owner(owner));
    }

    @Test
    public void halfDoneMoveIsFinishedWithoutOverwriting() throws Exception {
        // A previous switch FABLE -> OTHER was killed after stashing Cache only; Cache was then made again.
        SaveCaches.writeOwner(owner, FABLE);
        TestSupport.write(new File(SaveCaches.stashDir(save, FABLE), "Cache/unpacked.bin"), "fable old");
        TestSupport.write(new File(save, "Cache/unpacked.bin"), "fable new");
        TestSupport.write(new File(save, "Partition3.img"), "fable");

        assertTrue(SaveCaches.switchTo(save, owner, OTHER));
        // The stashed Cache is not overwritten: the live one stays where it was.
        assertEquals("fable old", read(new File(SaveCaches.stashDir(save, FABLE), "Cache/unpacked.bin")));
        assertEquals("fable new", read(new File(save, "Cache/unpacked.bin")));
        assertEquals("fable", read(new File(SaveCaches.stashDir(save, FABLE), "Partition3.img")));
        assertEquals(OTHER, SaveCaches.owner(owner));
    }

    @Test
    public void restoreNeverReplacesALiveEntry() throws Exception {
        SaveCaches.writeOwner(owner, OTHER);
        TestSupport.write(new File(SaveCaches.stashDir(save, FABLE), "Partition3.img"), "fable");
        // An entry the owner cannot stash (its stash already holds one) stays live.
        TestSupport.write(new File(SaveCaches.stashDir(save, OTHER), "Partition3.img"), "other old");
        TestSupport.write(new File(save, "Partition3.img"), "other new");
        assertTrue(SaveCaches.switchTo(save, owner, FABLE));
        assertEquals("other new", read(new File(save, "Partition3.img")));
        assertEquals("fable", read(new File(SaveCaches.stashDir(save, FABLE), "Partition3.img")));
    }

    /** The update to per-title Z: (K-9): the owner's old Z: moves into Cache/<owner> and is not rebuilt. */
    @Test
    public void ownerKeepsItsOldZ() throws Exception {
        TestSupport.write(new File(save, "Cache/data/misc/xbox/effects.big"), "unpacked");
        TestSupport.write(new File(save, "Cache/PageFile.bin"), "page");
        TestSupport.write(new File(save, "Cache/4c410003/currentgame/x"), "kotor");   // another title's folder
        TestSupport.write(new File(save, "TitleCache/LocalCache00.bin"), "title cache");
        SaveCaches.writeOwner(owner, FABLE);

        assertFalse(SaveCaches.switchTo(save, owner, FABLE));
        assertEquals("unpacked", read(new File(save, "Cache/4d5300d1/data/misc/xbox/effects.big")));
        assertEquals("page", read(new File(save, "Cache/4d5300d1/PageFile.bin")));
        assertFalse(new File(save, "Cache/data").exists());
        assertFalse(new File(save, "Cache/PageFile.bin").exists());
        assertEquals("a title folder stays", "kotor", read(new File(save, "Cache/4c410003/currentgame/x")));
        assertEquals("only Cache moves", "title cache", read(new File(save, "TitleCache/LocalCache00.bin")));

        // Idempotent: nothing left to move, nothing moves twice.
        assertEquals(0, SaveCaches.migrateLegacyCache(save, FABLE));
        assertFalse(SaveCaches.switchTo(save, owner, FABLE));
        assertEquals("unpacked", read(new File(save, "Cache/4d5300d1/data/misc/xbox/effects.big")));
        // Storage counts the moved files under the owner.
        assertEquals("unpacked".length() + "page".length() + "kotor".length() + "title cache".length(),
                     SaveCaches.bytes(save, owner, FABLE));
    }

    @Test
    public void migrationNeverReplaces() throws Exception {
        TestSupport.write(new File(save, "Cache/PageFile.bin"), "old");
        TestSupport.write(new File(save, "Cache/4d5300d1/PageFile.bin"), "new");
        TestSupport.write(new File(save, "Cache/Saves/a"), "old saves");
        assertEquals(1, SaveCaches.migrateLegacyCache(save, FABLE));
        assertEquals("new", read(new File(save, "Cache/4d5300d1/PageFile.bin")));
        assertEquals("old", read(new File(save, "Cache/PageFile.bin")));
        assertEquals("old saves", read(new File(save, "Cache/4d5300d1/Saves/a")));
    }

    @Test
    public void noMigrationWithoutAKnownOwner() throws Exception {
        TestSupport.write(new File(save, "Cache/data/a.big"), "x");
        assertFalse(SaveCaches.switchTo(save, owner, FABLE));            // no owner: adopted, not migrated
        assertEquals("x", read(new File(save, "Cache/data/a.big")));
        assertEquals(0, SaveCaches.migrateLegacyCache(save, "local-0a1b2c3d"));   // not a title ID folder
        assertEquals("x", read(new File(save, "Cache/data/a.big")));
        assertEquals(0, SaveCaches.migrateLegacyCache(new File(tmp.getRoot(), "none"), FABLE));
    }

    @Test
    public void ownerFile() throws Exception {
        assertNull(SaveCaches.owner(owner));
        TestSupport.write(owner, "../evil\n");
        assertNull(SaveCaches.owner(owner));
        TestSupport.write(owner, "");
        assertNull(SaveCaches.owner(owner));
        SaveCaches.writeOwner(owner, "local-0a1b2c3d");
        assertEquals("local-0a1b2c3d", SaveCaches.owner(owner));
        try {
            SaveCaches.switchTo(save, owner, "a/b");
            throw new AssertionError("a path is not a title ID");
        } catch (java.io.IOException expected) {
        }
    }

    @Test
    public void bytes() throws Exception {
        liveSet("12345");
        SaveCaches.writeOwner(owner, FABLE);
        assertEquals(25, SaveCaches.bytes(save, owner, FABLE));
        assertEquals(0, SaveCaches.bytes(save, owner, OTHER));
        SaveCaches.switchTo(save, owner, OTHER);
        assertEquals(25, SaveCaches.bytes(save, owner, FABLE));
        assertEquals(0, SaveCaches.bytes(save, owner, OTHER));
    }
}
