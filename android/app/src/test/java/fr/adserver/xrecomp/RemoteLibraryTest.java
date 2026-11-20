package fr.adserver.xrecomp;

import static fr.adserver.xrecomp.TorrentMetaTest.file;
import static fr.adserver.xrecomp.TorrentMetaTest.files;
import static fr.adserver.xrecomp.TorrentMetaTest.torrent;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * RemoteLibrary (Robolectric for org.json): a torrent's files as games (images, split sets, .cue with its tracks, ROMs,
 * the console by name or by source), cleaned titles, keys, the sources file, magnets and their metadata, and the
 * games that are on the device left out.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class RemoteLibraryTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private File files;

    @Before
    public void setUp() {
        files = new File(tmp.getRoot(), "files");
    }

    private static RemoteLibrary.Source source(Console c) {
        RemoteLibrary.Source s = new RemoteLibrary.Source();
        s.id = "0123456789abcdef0123456789abcdef01234567";
        s.console = c;
        return s;
    }

    private static List<String> titles(List<RemoteLibrary.Game> l) {
        List<String> t = new ArrayList<>();
        for (RemoteLibrary.Game g : l)
            t.add(g.console.id + ":" + g.title + ":" + g.files.size() + ":" + g.size());
        return t;
    }

    @Test
    public void aTorrentsFilesBecomeGames() throws IOException {
        TorrentMeta m = TorrentMeta.parse(torrent("Mixed", false, files(
                file(100, "Halo (USA) (Rev 1).iso"),
                file(10, "Big.1.iso"), file(20, "Big.2.iso"),
                file(5, "ps1", "Crash (Europe).cue"), file(50, "ps1", "Crash (Europe) (Track 1).bin"),
                file(30, "ps1", "Crash (Europe) (Track 2).bin"),
                file(40, "Lone.bin"),
                file(8, "gba", "Golden_Sun [!].gba"),
                file(9, "n64", "Mario.z64"),
                file(3, "readme.txt"), file(2, ".hidden.iso"),
                file(7, "Fable.xiso"))));
        List<RemoteLibrary.Game> g = RemoteLibrary.games(source(null), m);
        assertEquals(java.util.Arrays.asList(
                "xbox:Halo:1:100", "xbox:Big:2:30", "ps1:Lone:1:40", "xbox:Fable:1:7",
                "ps1:Crash:3:85", "gba:Golden Sun:1:8", "n64:Mario:1:9"), titles(g));
    }

    @Test
    public void thePlainIsoTakesTheSourcesConsoleOtherNamesKeepTheirs() throws IOException {
        TorrentMeta m = TorrentMeta.parse(torrent("PS1", false, files(file(1, "Disc.iso"), file(2, "X.xiso"),
                                                                      file(3, "G.gba"))));
        assertEquals(java.util.Arrays.asList("ps1:Disc:1:1", "xbox:X:1:2", "gba:G:1:3"),
                     titles(RemoteLibrary.games(source(Console.PS1), m)));
        assertEquals("xbox", RemoteLibrary.games(source(null), m).get(0).console.id);
    }

    @Test
    public void cleanTitles() {
        assertEquals("Halo", RemoteLibrary.cleanTitle("Halo (USA) (Rev 1).iso"));
        assertEquals("Jet Set Radio Future", RemoteLibrary.cleanTitle("Jet_Set_Radio_Future [!].xiso"));
        assertEquals("Big", RemoteLibrary.cleanTitle("Big.part1.iso"));
        assertEquals("Big", RemoteLibrary.cleanTitle("Big.iso.001"));
        assertEquals("Big", RemoteLibrary.cleanTitle("Big.2.iso"));
        assertEquals("Mario Kart", RemoteLibrary.cleanTitle("Mario Kart.z64"));
        assertEquals("(USA)", RemoteLibrary.cleanTitle("(USA).iso"));   // nothing left: the bare name
    }

    @Test
    public void keysAreStableNeverTitleIdsAndCarryTheConsole() throws IOException {
        TorrentMeta m = TorrentMeta.parse(torrent("T", false, files(file(1, "A.iso"), file(2, "B.gba"))));
        List<RemoteLibrary.Game> g = RemoteLibrary.games(source(null), m);
        assertTrue(g.get(0).key, g.get(0).key.matches("net-0123456789ab-[0-9a-f]{8}"));
        assertTrue(g.get(1).key, g.get(1).key.matches("gba-net-0123456789ab-[0-9a-f]{8}"));
        assertEquals(Console.GBA, Console.ofKey(g.get(1).key));
        assertEquals(Console.XBOX, Console.ofKey(g.get(0).key));
        assertTrue(RemoteLibrary.isRemoteKey(g.get(0).key));
        assertTrue(RemoteLibrary.isRemoteKey(g.get(1).key));
        assertFalse(RemoteLibrary.isRemoteKey("4D530004"));
        assertFalse(RemoteLibrary.isRemoteKey("gba-AXVE-1A2B3C4D"));
        assertFalse(RemoteLibrary.isRemoteKey("local-abc"));
        assertNotEquals(g.get(0).key, RemoteLibrary.games(source(null), m).get(1).key);
        assertEquals(g.get(0).key, RemoteLibrary.games(source(null), m).get(0).key);
    }

    @Test
    public void sourcesFromATorrentAndAMagnet() throws IOException {
        byte[] t = torrent("My Xbox", true, files(file(100, "Halo.iso"), file(10, "Fable.iso")));
        RemoteLibrary.Source s = RemoteLibrary.addTorrent(files, t, Console.XBOX, 5);
        assertEquals("My Xbox", s.name);
        String other = "fedcba9876543210fedcba9876543210fedcba98";
        RemoteLibrary.Source mag = RemoteLibrary.addMagnet(files, "magnet:?xt=urn:btih:" + other + "&dn=GBA+games",
                                                          null, 6);
        assertEquals("GBA games", mag.name);
        assertNull(RemoteLibrary.addMagnet(files, "http://not.a.magnet", null, 7));

        List<RemoteLibrary.Source> l = RemoteLibrary.sources(files);
        assertEquals(2, l.size());
        assertEquals(s.id, l.get(0).id);
        assertEquals(Console.XBOX, l.get(0).console);
        assertNull(l.get(1).console);
        assertNotNull(RemoteLibrary.meta(files, s.id));
        assertNull(RemoteLibrary.meta(files, other));   // still fetching

        // The same torrent again: one source, its console updated.
        RemoteLibrary.addTorrent(files, t, Console.PS1, 9);
        l = RemoteLibrary.sources(files);
        assertEquals(2, l.size());
        assertEquals(Console.PS1, l.get(0).console);
        assertEquals(5, l.get(0).addedAt);

        // Metadata for the magnet: only the right hash is kept.
        assertFalse(RemoteLibrary.saveMetadata(files, other, t));
        assertNull(RemoteLibrary.meta(files, other));

        RemoteLibrary.remove(files, s.id);
        assertEquals(1, RemoteLibrary.sources(files).size());
        assertFalse(RemoteLibrary.torrentFile(files, s.id).exists());
    }

    @Test
    public void gamesOnTheDeviceAreLeftOut() throws IOException {
        RemoteLibrary.addTorrent(files, torrent("X", false, files(file(100, "Halo.iso"), file(10, "Fable.iso"),
                                                                  file(10, "Big.1.iso"), file(20, "Big.2.iso"))),
                                 null, 1);
        LibraryIndex local = new LibraryIndex();
        LibraryIndex.Item halo = new LibraryIndex.Item();
        halo.name = "Halo.iso";
        halo.size = 100;
        LibraryIndex.Item fable = new LibraryIndex.Item();
        fable.name = "Fable.iso";
        fable.size = 11;   // another size: another file, the remote one stays
        LibraryIndex.Item big = new LibraryIndex.Item();
        big.name = "Big.1.iso";
        big.size = 30;     // a split set: its parts summed
        local.items.add(halo);
        local.items.add(fable);
        local.items.add(big);
        Map<String, RemoteLibrary.Game> m = RemoteLibrary.notLocal(files, local);
        assertEquals(1, m.size());
        assertEquals("Fable", m.values().iterator().next().title);
        assertEquals(3, RemoteLibrary.notLocal(files, null).size());
    }

    @Test
    public void aDamagedSourcesFileIsNoSources() throws IOException {
        RemoteLibrary.dir(files).mkdirs();
        java.nio.file.Files.write(RemoteLibrary.sourcesFile(files).toPath(), "{nope".getBytes());
        assertTrue(RemoteLibrary.sources(files).isEmpty());
    }

    @Test
    public void filesDeepInTheTorrentAreListed() throws IOException {
        // the download folder is walked as deep as any path TorrentMeta takes (XBA-320: 5–6 levels of folders seen)
        TorrentMeta m = TorrentMeta.parse(torrent("T", false, files(file(1, "A.iso"), file(2, "d", "B.iso"),
                                                                    file(3, "a", "b", "c", "d", "e", "f", "C.iso"))));
        assertEquals(TorrentMeta.MAX_DEPTH, RemoteLibrary.maxPathDepth(m));
        assertEquals(java.util.Arrays.asList("xbox:A:1:1", "xbox:B:1:2", "xbox:C:1:3"),
                     titles(RemoteLibrary.games(source(null), m)));
    }

    @Test
    public void zipsAreGamesOfTheSourcesConsole() throws IOException {
        // XBA-331: a Redump set, one .zip per disc
        TorrentMeta m = TorrentMeta.parse(torrent("Microsoft - Xbox 360", false, files(
                file(70, "A", "Adidas miCoach (Europe) (Disc 1).zip"), file(80, "A", "Adidas miCoach (Europe) (Disc 2).zip"),
                file(1, "readme.txt"))));
        assertEquals(java.util.Arrays.asList("x360:Adidas miCoach:1:70", "x360:Adidas miCoach:1:80"),
                     titles(RemoteLibrary.games(source(Console.X360), m)));
        // no console chosen: the name inside, else the Xbox
        TorrentMeta r = TorrentMeta.parse(torrent("R", false, files(file(5, "Pocket.gba.zip"), file(6, "Disc.zip"))));
        assertEquals(java.util.Arrays.asList("gba:Pocket:1:5", "xbox:Disc:1:6"), titles(RemoteLibrary.games(source(null), r)));
        assertEquals("Game v1.2", RemoteLibrary.cleanTitle("Game v1.2 (USA).zip"));
        assertEquals("Halo", RemoteLibrary.cleanTitle("Halo (USA).iso.zip"));
    }

    @Test
    public void aTorrentWithNoGameSaysWhatItHolds() throws IOException {
        TorrentMeta m = TorrentMeta.parse(torrent("T", false, files(file(1, "a.7z"), file(1, "b.7z"), file(1, "c.7z"),
                                                                    file(1, "x.txt"), file(1, "noext"))));
        assertTrue(RemoteLibrary.games(source(null), m).isEmpty());
        assertEquals("3 .7z, 1 .txt, 1 —", RemoteLibrary.fileSummary(m));
    }

    @Test
    public void anUnpackedZipIsLocalNow() throws IOException {
        RemoteLibrary.Source s = RemoteLibrary.addTorrent(files, torrent("Set", false, files(
                file(70, "d", "Game (Disc 1).zip"), file(80, "d", "Game (Disc 2).zip"))), Console.X360, 1);
        LibraryIndex local = new LibraryIndex();
        LibraryIndex.Item i = new LibraryIndex.Item();
        i.folder = LibraryScanner.NET_FOLDER;
        i.loc = "file:/x/net/" + s.shortId() + "/Set/d/Game (Disc 1)/Game (Disc 1).iso";
        i.name = "Game (Disc 1).iso";
        i.size = 1000;
        local.items.add(i);
        Map<String, RemoteLibrary.Game> m = RemoteLibrary.notLocal(files, local);
        assertEquals(1, m.size());
        assertEquals("d/Game (Disc 2).zip", m.values().iterator().next().files.get(0).path);
        i.folder = "content://elsewhere";   // the same path in a library folder of the player's is not that download
        assertEquals(2, RemoteLibrary.notLocal(files, local).size());
    }
}
