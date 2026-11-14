package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Ps1Discs (XBA-166): the discs of a multi-disc PlayStation game beside the one playing: disc markers in names, one
 * file per disc (a .cue's tracks are not discs), a .m3u that names it, the library index's documents of a folder,
 * and nothing for a single disc.
 */
public class Ps1DiscsTest {
    private static Map<String, String> folder(String... names) {
        Map<String, String> m = new LinkedHashMap<>();
        for (String n : names)
            m.put(n, "/games/ps1/" + n);
        return m;
    }

    private static List<String> names(List<Ps1Discs.Disc> ds) {
        List<String> out = new ArrayList<>();
        for (Ps1Discs.Disc d : ds)
            out.add(d.name + (d.current ? "*" : ""));
        return out;
    }

    @Test
    public void markers() {
        assertEquals(1, Ps1Discs.number("Final Game (USA) (Disc 1).cue"));
        assertEquals(2, Ps1Discs.number("Final Game Disc 2.chd"));
        assertEquals(3, Ps1Discs.number("Final Game [Disc 3].bin"));
        assertEquals(1, Ps1Discs.number("FinalGame_CD1.iso"));
        assertEquals(2, Ps1Discs.number("Final Game (CD 2).cue"));
        assertEquals(4, Ps1Discs.number("Final Game - Disk 4.chd"));
        assertEquals(2, Ps1Discs.number("Final Game (Disc 2 of 3).cue"));
        assertEquals(0, Ps1Discs.number("ABCD1 Racing.cue"));        // CD inside a word
        assertEquals(0, Ps1Discs.number("Final Game.cue"));
        assertEquals("final game (usa)", Ps1Discs.gameKey("Final Game (USA) (Disc 1).cue"));
        assertEquals(Ps1Discs.gameKey("Final Game (USA) (Disc 1).cue"), Ps1Discs.gameKey("final game (usa) (disc 2).chd"));
        assertEquals(Ps1Discs.gameKey("FinalGame_CD1.iso"), Ps1Discs.gameKey("FinalGame_CD2.iso"));
    }

    @Test
    public void sameNameButTheDisc() {
        Map<String, String> f = folder("Game (USA) (Disc 2).cue", "Game (USA) (Disc 2).bin",
                                       "Game (USA) (Disc 1) (Track 1).bin", "Game (USA) (Disc 1) (Track 2).bin",
                                       "Game (USA) (Disc 1).cue", "Game (USA) (Disc 3).chd", "Game (USA) (Disc 3).cue",
                                       "Game (USA) (Disc 3) (Track 1).bin", "Other (Disc 1).cue", "Other (Disc 2).cue",
                                       "readme.txt", "Game (USA) (Disc 1).sav");
        List<Ps1Discs.Disc> ds = Ps1Discs.find("Game (USA) (Disc 1).cue", f, null);
        assertEquals(List.of("Game (USA) (Disc 1).cue*", "Game (USA) (Disc 2).cue", "Game (USA) (Disc 3).cue"),
                     names(ds));
        assertEquals("/games/ps1/Game (USA) (Disc 2).cue", ds.get(1).spec);
        assertEquals(2, ds.get(1).number);
        // Playing the .chd of disc 3: that one stands for disc 3.
        ds = Ps1Discs.find("Game (USA) (Disc 3).chd", f, null);
        assertEquals(List.of("Game (USA) (Disc 1).cue", "Game (USA) (Disc 2).cue", "Game (USA) (Disc 3).chd*"),
                     names(ds));
        // .chd only, numbers out of order on disk.
        ds = Ps1Discs.find("G CD2.chd", folder("G CD3.chd", "G CD2.chd", "G CD1.chd"), null);
        assertEquals(List.of("G CD1.chd", "G CD2.chd*", "G CD3.chd"), names(ds));
    }

    @Test
    public void nothingForOneDisc() {
        assertTrue(Ps1Discs.find("Game.cue", folder("Game.cue", "Game.bin", "Game 2.cue"), null).isEmpty());
        assertTrue(Ps1Discs.find("Game (Disc 1).cue", folder("Game (Disc 1).cue", "Game (Disc 1).bin"), null)
                           .isEmpty());
        assertTrue(Ps1Discs.find("Game (Disc 1).cue", new HashMap<>(), null).isEmpty());
        assertTrue(Ps1Discs.find(null, folder("A.cue"), null).isEmpty());
    }

    @Test
    public void playlist() {
        Map<String, String> f = folder("Saga.m3u", "Saga A.chd", "saga b.chd", "Saga C.chd", "Unrelated.m3u");
        Map<String, String> texts = new HashMap<>();
        texts.put("/games/ps1/Saga.m3u", "﻿# the saga\r\nSaga A.chd\r\n\r\nsub/Saga B.chd\r\nC:\\x\\Saga C.chd\r\n"
                                         + "Missing.chd\r\n");
        texts.put("/games/ps1/Unrelated.m3u", "Other.cue\n");
        Ps1Discs.Reader r = spec -> {
            String t = texts.get(spec);
            if (t == null)
                throw new IOException("no " + spec);
            return t;
        };
        List<Ps1Discs.Disc> ds = Ps1Discs.find("saga b.chd", f, r);
        assertEquals(List.of("Saga A.chd", "saga b.chd*", "Saga C.chd"), names(ds));
        assertEquals(3, ds.get(2).number);
        // A playlist that does not name the disc: not this game's.
        assertTrue(Ps1Discs.find("Other.cue", folder("Other.cue", "Unrelated.m3u"), r).isEmpty());
        // An unreadable playlist: the names decide.
        ds = Ps1Discs.find("X (Disc 1).cue", folder("Bad.m3u", "X (Disc 1).cue", "X (Disc 2).cue"), r);
        assertEquals(2, ds.size());
        assertEquals(List.of("a.cue", "b.cue"), Ps1Discs.m3uNames("a.cue\n# no\n  \n/x/y/b.cue"));
    }

    @Test
    public void siblingsFromTheLibraryIndex() {
        LibraryIndex x = new LibraryIndex();
        LibraryIndex.Item cue = new LibraryIndex.Item();
        cue.loc = "doc:primary:ps1/Game (Disc 1).cue";
        cue.name = "Game (Disc 1).cue";
        cue.parts.add("doc:primary:ps1/Game (Disc 1).bin");
        x.items.add(cue);
        LibraryIndex.Item chd = new LibraryIndex.Item();
        chd.loc = "doc:primary:ps1/Game (Disc 2).chd";
        x.items.add(chd);
        LibraryIndex.Item deeper = new LibraryIndex.Item();
        deeper.loc = "doc:primary:ps1/more/Game (Disc 3).chd";
        x.items.add(deeper);
        LibraryIndex.Item path = new LibraryIndex.Item();
        path.loc = "/sdcard/ps1/Game (Disc 4).chd";
        x.items.add(path);
        Map<String, String> s = Ps1Discs.siblingsFromIndex(x, "primary:ps1/Game (Disc 1).cue");
        assertEquals(3, s.size());
        assertEquals("primary:ps1/Game (Disc 2).chd", s.get("Game (Disc 2).chd"));
        assertTrue(s.containsKey("Game (Disc 1).bin"));
        assertFalse(s.containsKey("Game (Disc 3).chd"));
        assertEquals(List.of("Game (Disc 1).cue*", "Game (Disc 2).chd"),
                     names(Ps1Discs.find("Game (Disc 1).cue", s, null)));
        assertTrue(Ps1Discs.siblingsFromIndex(null, "primary:x.cue").isEmpty());
    }
}
