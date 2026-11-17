package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A PS1 .cue among documents (XBA-168): its FILE lines renamed to the open track descriptors (/proc/self/fd/N) in a
 * .cue of the game activity's own, the rest of the sheet kept; a missing or unopenable track fails it.
 */
@RunWith(RobolectricTestRunner.class)
public class Ps1CueTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    static final String CUE = "REM made on Windows\r\n"
                              + "FILE \"Game (Track 1).bin\" BINARY\r\n"
                              + "  TRACK 01 MODE2/2352\r\n"
                              + "    INDEX 01 00:00:00\r\n"
                              + "FILE \"sub\\GAME (TRACK 2).BIN\" BINARY\r\n"
                              + "  TRACK 02 AUDIO\r\n"
                              + "    INDEX 00 10:00:00\r\n"
                              + "    INDEX 01 10:02:00\r\n";

    @Test
    public void rewrite() {
        Map<String, String> paths = new HashMap<>();
        paths.put("game (track 1).bin", "/proc/self/fd/41");
        paths.put("game (track 2).bin", "/proc/self/fd/42");
        assertEquals("REM made on Windows\n"
                     + "FILE \"/proc/self/fd/41\" BINARY\n"
                     + "  TRACK 01 MODE2/2352\n"
                     + "    INDEX 01 00:00:00\n"
                     + "FILE \"/proc/self/fd/42\" BINARY\n"
                     + "  TRACK 02 AUDIO\n"
                     + "    INDEX 00 10:00:00\n"
                     + "    INDEX 01 10:02:00\n", Ps1Cue.rewrite(CUE, paths));
        // Unquoted names too.
        assertEquals("FILE \"/proc/self/fd/7\" BINARY\n",
                     Ps1Cue.rewrite("FILE game.bin BINARY", one("game.bin", "/proc/self/fd/7")));
        paths.remove("game (track 2).bin");
        assertNull("a track it cannot name", Ps1Cue.rewrite(CUE, paths));
        assertEquals("game.bin", Ps1Cue.baseName("C:\\Games\\GAME.BIN"));
        assertEquals("game.bin", Ps1Cue.baseName("discs/game.bin"));
    }

    private static Map<String, String> one(String k, String v) {
        Map<String, String> m = new HashMap<>();
        m.put(k, v);
        return m;
    }

    @Test
    public void runtimeCue() throws IOException {
        Map<String, String> names = new HashMap<>();
        names.put("content://t/document/1", "Game (Track 1).bin");
        names.put("content://t/document/2", "Game (Track 2).bin");
        List<String> parts = Arrays.asList("content://t/document/2", "content://t/document/1");
        Map<String, Integer> fds = new HashMap<>();
        fds.put("content://t/document/1", 41);
        fds.put("content://t/document/2", 42);
        File out = new File(tmp.getRoot(), "ps1");
        Ps1Cue.Reader reader = u -> {
            if (!u.equals("content://t/document/cue"))
                throw new IOException("no " + u);
            return new ByteArrayInputStream(CUE.getBytes(StandardCharsets.ISO_8859_1));
        };
        // The fds here are fake numbers, not descriptors this JVM owns: the closer only records them.
        List<Integer> closed = new ArrayList<>();
        String path = Ps1Cue.runtimeCue("content://t/document/cue", parts, reader, names::get, fds::get, closed::add,
                                        out);
        assertNotNull(path);
        assertEquals(new File(out, "disc.cue").getAbsolutePath(), path);
        assertEquals("a disc that opens keeps its tracks open", Collections.emptyList(), closed);
        String text = new String(Files.readAllBytes(new File(path).toPath()), StandardCharsets.ISO_8859_1);
        assertEquals(2, text.split("/proc/self/fd/4", -1).length - 1);
        assertEquals(true, text.contains("FILE \"/proc/self/fd/41\" BINARY\n  TRACK 01 MODE2/2352"));
        assertEquals(true, text.contains("FILE \"/proc/self/fd/42\" BINARY\n  TRACK 02 AUDIO"));

        // A track that is not beside it, one that cannot be opened, a .cue that cannot be read: no disc, and the
        // track already opened (track 1, opened first) is given back.
        assertNull(Ps1Cue.runtimeCue("content://t/document/cue", parts.subList(1, 2), reader, names::get, fds::get,
                                     closed::add, out));
        assertEquals("track 2 missing: track 1 closed", Collections.singletonList(41), closed);
        closed.clear();
        assertNull(Ps1Cue.runtimeCue("content://t/document/cue", parts, reader, names::get, u -> {
            if (u.endsWith("/2"))
                throw new IOException("gone");
            return fds.get(u);
        }, closed::add, out));
        assertEquals("track 2 unopenable: track 1 closed", Collections.singletonList(41), closed);
        closed.clear();
        assertNull(Ps1Cue.runtimeCue("content://t/document/cue", parts, reader, names::get,
                                     u -> u.endsWith("/2") ? -1 : fds.get(u), closed::add, out));
        assertEquals("track 2 refused (-1): track 1 closed", Collections.singletonList(41), closed);
        closed.clear();
        assertNull(Ps1Cue.runtimeCue("content://t/document/cue", parts, reader, names::get, u -> {
            throw new IOException("gone");
        }, closed::add, out));
        assertEquals("nothing opened, nothing closed", Collections.emptyList(), closed);
        assertNull(Ps1Cue.runtimeCue("content://t/document/other", parts, reader, names::get, fds::get, closed::add,
                                     out));
        assertEquals(Collections.emptyList(), closed);

        // The sheet cannot be written (its folder is a file): both tracks are given back.
        File blocked = new File(tmp.getRoot(), "blocked");
        Files.write(blocked.toPath(), new byte[] {1});
        assertNull(Ps1Cue.runtimeCue("content://t/document/cue", parts, reader, names::get, fds::get, closed::add,
                                     blocked));
        Collections.sort(closed);
        assertEquals(Arrays.asList(41, 42), closed);
    }
}
