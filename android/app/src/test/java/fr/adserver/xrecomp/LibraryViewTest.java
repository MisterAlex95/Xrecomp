package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The library's view (XBA-206, plain JUnit): the four sorts, the console filter, the search (case, accents, words),
 * favourites first and their storage, the « Recently played » row, the saved state.
 */
public class LibraryViewTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static LibraryScreen.Entry entry(String key, String title, long lastPlayed, long playMs) {
        LibraryScreen.Entry e = new LibraryScreen.Entry(key);
        Library.Record r = new Library.Record();
        r.titleId = key;
        r.title = title;
        r.lastPlayed = lastPlayed;
        r.playMs = playMs;
        e.record = r;
        return e;
    }

    private static List<LibraryScreen.Entry> library() {
        return new ArrayList<>(Arrays.asList(entry("58420001", "Night Drive", 300, 10), entry("gba-AXVE", "Pocket Quest", 0, 0),
                                             entry("58420002", "Ocean Trip", 100, 500), entry("ps1-SLUS00001", "Écran Total", 200, 40),
                                             entry("n64-NSME", "Ancient Hills", 0, 7)));
    }

    private static List<String> titles(List<LibraryScreen.Entry> l) {
        List<String> out = new ArrayList<>();
        for (LibraryScreen.Entry e : l)
            out.add(e.title());
        return out;
    }

    @Test
    public void sorts() {
        LibraryView.State s = new LibraryView.State();
        assertEquals(Arrays.asList("Ancient Hills", "Écran Total", "Night Drive", "Ocean Trip", "Pocket Quest"),
                     titles(LibraryView.apply(library(), s)));
        s.sort = LibraryView.Sort.LAST_PLAYED;   // never played last, by name
        assertEquals(Arrays.asList("Night Drive", "Écran Total", "Ocean Trip", "Ancient Hills", "Pocket Quest"),
                     titles(LibraryView.apply(library(), s)));
        s.sort = LibraryView.Sort.MOST_PLAYED;
        assertEquals(Arrays.asList("Ocean Trip", "Écran Total", "Night Drive", "Ancient Hills", "Pocket Quest"),
                     titles(LibraryView.apply(library(), s)));
        s.sort = LibraryView.Sort.CONSOLE;       // Xbox, GBA, PS1, N64
        assertEquals(Arrays.asList("Night Drive", "Ocean Trip", "Pocket Quest", "Écran Total", "Ancient Hills"),
                     titles(LibraryView.apply(library(), s)));
        assertEquals(LibraryView.Sort.NAME, LibraryView.Sort.MOST_PLAYED.next());
        assertEquals(LibraryView.Sort.NAME, LibraryView.Sort.parse("bogus"));
        assertEquals(LibraryView.Sort.MOST_PLAYED, LibraryView.Sort.parse("most_played"));
    }

    @Test
    public void gamesWithoutRecordSortAsNeverPlayed() {
        LibraryScreen.Entry bare = new LibraryScreen.Entry("58420009");   // seen in the folder only: its key as title
        List<LibraryScreen.Entry> l = library();
        l.add(bare);
        LibraryView.State s = new LibraryView.State();
        s.sort = LibraryView.Sort.LAST_PLAYED;
        List<LibraryScreen.Entry> out = LibraryView.apply(l, s);
        assertEquals(6, out.size());
        assertEquals("Night Drive", out.get(0).title());
    }

    @Test
    public void consoleFilterAndSearch() {
        LibraryView.State s = new LibraryView.State();
        s.console = Console.XBOX;
        assertEquals(Arrays.asList("Night Drive", "Ocean Trip"), titles(LibraryView.apply(library(), s)));
        s.console = Console.GBA;
        assertEquals(Arrays.asList("Pocket Quest"), titles(LibraryView.apply(library(), s)));
        assertTrue(s.narrowed());
        s.console = null;
        s.query = "ecran";   // no accent needed
        assertEquals(Arrays.asList("Écran Total"), titles(LibraryView.apply(library(), s)));
        s.query = "  TRIP  ocean ";   // any case, any order of the words
        assertEquals(Arrays.asList("Ocean Trip"), titles(LibraryView.apply(library(), s)));
        s.query = "zzz";
        assertTrue(LibraryView.apply(library(), s).isEmpty());
        s.query = "   ";
        assertFalse(s.narrowed());
        assertEquals(5, LibraryView.apply(library(), s).size());
        assertTrue(LibraryView.matches("Pokémon Ruby", "pokemon"));
        assertFalse(LibraryView.matches("Pokémon Ruby", "sapphire"));
    }

    @Test
    public void favouritesFirstInEverySort() {
        LibraryView.State s = new LibraryView.State();
        assertTrue(s.toggleFavourite("gba-AXVE"));
        assertTrue(s.toggleFavourite("58420002"));
        assertEquals(Arrays.asList("Ocean Trip", "Pocket Quest", "Ancient Hills", "Écran Total", "Night Drive"),
                     titles(LibraryView.apply(library(), s)));
        s.sort = LibraryView.Sort.MOST_PLAYED;
        assertEquals(Arrays.asList("Ocean Trip", "Pocket Quest", "Écran Total", "Night Drive", "Ancient Hills"),
                     titles(LibraryView.apply(library(), s)));
        assertFalse("a second press takes it out", s.toggleFavourite("gba-AXVE"));
        assertFalse(s.favourite("gba-AXVE"));
        assertFalse(s.toggleFavourite(""));
        assertFalse(s.favourite(null));
    }

    @Test
    public void recentlyPlayed() {
        List<LibraryScreen.Entry> r = LibraryView.recent(library(), 5);
        assertEquals(Arrays.asList("Night Drive", "Écran Total", "Ocean Trip"), titles(r));
        assertEquals(Arrays.asList("Night Drive", "Écran Total"), titles(LibraryView.recent(library(), 2)));
        assertTrue(LibraryView.recent(new ArrayList<>(), 5).isEmpty());
        assertEquals(Arrays.asList(Console.XBOX, Console.GBA, Console.PS1, Console.N64),
                     LibraryView.consoles(library()));
    }

    @Test
    public void stateKeptInItsFile() throws Exception {
        File files = tmp.newFolder("files");
        LibraryView.State s = LibraryView.load(files);   // none yet: the defaults
        assertEquals(LibraryView.Sort.NAME, s.sort);
        assertNull(s.console);
        assertTrue(s.favourites.isEmpty());
        s.sort = LibraryView.Sort.MOST_PLAYED;
        s.console = Console.PS1;
        s.query = "not kept";
        s.toggleFavourite("58420001");
        s.toggleFavourite("ps1-SLUS00001");
        assertTrue(LibraryView.save(files, s));
        LibraryView.State t = LibraryView.load(files);
        assertEquals(LibraryView.Sort.MOST_PLAYED, t.sort);
        assertEquals(Console.PS1, t.console);
        assertEquals("", t.query);
        assertEquals(Arrays.asList("58420001", "ps1-SLUS00001"), new ArrayList<>(t.favourites));
        t.console = null;
        t.toggleFavourite("58420001");
        assertTrue(LibraryView.save(files, t));
        LibraryView.State u = LibraryView.load(files);
        assertNull(u.console);
        assertEquals(Arrays.asList("ps1-SLUS00001"), new ArrayList<>(u.favourites));
        // A file it cannot read: the defaults, nothing thrown.
        TestSupport.write(LibraryView.file(files), "sort=\\u12\nconsole=sega\n".getBytes("UTF-8"));
        LibraryView.State v = LibraryView.load(files);
        assertEquals(LibraryView.Sort.NAME, v.sort);
        assertNull(v.console);
    }
}
