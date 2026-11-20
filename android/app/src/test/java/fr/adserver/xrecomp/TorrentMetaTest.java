package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/** TorrentMeta (plain JUnit): bencode, the info hash over the raw bytes, files, pad files, bad paths, magnets. */
public class TorrentMetaTest {

    // ---- A small bencode writer (sorted keys, as BEP 3 wants) -------------------------------------------------------

    static byte[] encode(Object o) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        write(out, o);
        return out.toByteArray();
    }

    private static void write(ByteArrayOutputStream out, Object o) {
        try {
            if (o instanceof Long || o instanceof Integer) {
                out.write(("i" + o + "e").getBytes(StandardCharsets.US_ASCII));
            } else if (o instanceof String || o instanceof byte[]) {
                byte[] b = o instanceof String ? ((String)o).getBytes(StandardCharsets.UTF_8) : (byte[])o;
                out.write((b.length + ":").getBytes(StandardCharsets.US_ASCII));
                out.write(b);
            } else if (o instanceof List) {
                out.write('l');
                for (Object x : (List<?>)o)
                    write(out, x);
                out.write('e');
            } else if (o instanceof Map) {
                out.write('d');
                for (Map.Entry<?, ?> e : new TreeMap<>((Map<?, ?>)o).entrySet()) {
                    write(out, e.getKey());
                    write(out, e.getValue());
                }
                out.write('e');
            } else {
                throw new IllegalArgumentException(String.valueOf(o));
            }
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
    }

    static Map<String, Object> map(Object... kv) {
        Map<String, Object> m = new TreeMap<>();
        for (int i = 0; i < kv.length; i += 2)
            m.put((String)kv[i], kv[i + 1]);
        return m;
    }

    static Map<String, Object> file(long len, String... path) {
        return map("length", len, "path", Arrays.asList((Object[])path));
    }

    /** A multi-file torrent named name with these file entries. */
    static byte[] torrent(String name, boolean priv, List<Object> files) {
        Map<String, Object> info = map("name", name, "piece length", 16384L, "pieces", new byte[20], "files", files);
        if (priv)
            info.put("private", 1L);
        return encode(map("announce", "http://tracker.invalid/announce", "info", info));
    }

    static List<Object> files(Object... entries) {
        return new ArrayList<>(Arrays.asList(entries));
    }

    // ---- Tests ------------------------------------------------------------------------------------------------------

    @Test
    public void multiFileTorrentListsItsFilesAndHashesTheRawInfo() throws IOException {
        Map<String, Object> info = map("name", "Xbox", "piece length", 16384L, "pieces", new byte[20], "files",
                                       files(file(100, "Halo (USA).iso"), file(7, "sub", "Game.xiso")));
        byte[] t = encode(map("announce", "x", "info", info));
        TorrentMeta m = TorrentMeta.parse(t);
        assertEquals("Xbox", m.name);
        assertTrue(m.multiFile);
        assertFalse(m.isPrivate);
        assertEquals(2, m.files.size());
        assertEquals("Halo (USA).iso", m.files.get(0).path);
        assertEquals("sub/Game.xiso", m.files.get(1).path);
        assertEquals("Game.xiso", m.files.get(1).name());
        assertEquals("sub", m.files.get(1).dir());
        assertEquals(107, m.totalSize());
        assertEquals(TorrentMeta.sha1Hex(encode(info), 0, encode(info).length), m.infoHash);
        assertEquals(40, m.infoHash.length());
    }

    @Test
    public void infoHashIsOverTheBytesAsWrittenNotAReEncoding() throws IOException {
        // Keys out of order inside info: a re-encoding would sort them and change the hash.
        String info = "d4:name1:a6:lengthi5e12:piece lengthi16384e6:pieces20:" + "01234567890123456789" + "e";
        byte[] t = ("d4:info" + info + "e").getBytes(StandardCharsets.ISO_8859_1);
        TorrentMeta m = TorrentMeta.parse(t);
        byte[] raw = info.getBytes(StandardCharsets.ISO_8859_1);
        assertEquals(TorrentMeta.sha1Hex(raw, 0, raw.length), m.infoHash);
        assertFalse(m.multiFile);
        assertEquals("a", m.files.get(0).path);
        assertEquals(5, m.files.get(0).size);
        assertEquals(m.infoHash, TorrentMeta.parseInfo(raw).infoHash);
    }

    @Test
    public void padFilesAreLeftOutButKeepTheirNumber() throws IOException {
        Map<String, Object> pad = file(12, ".pad", "12");
        pad.put("attr", "p");
        TorrentMeta m = TorrentMeta.parse(torrent("T", true, files(file(4, "a.iso"), pad, file(9, "b.iso"))));
        assertTrue(m.isPrivate);
        assertEquals(2, m.files.size());
        assertEquals(0, m.files.get(0).index);
        assertEquals(2, m.files.get(1).index);
        assertEquals("b.iso", m.file("b.iso").path);
        assertNull(m.file(".pad/12"));
    }

    @Test
    public void pathsThatLeaveTheFolderRefuseTheTorrent() {
        for (String[] bad : new String[][] { { ".." , "x.iso" }, { "a", ".", "x.iso" }, { "" }, { "a/b.iso" },
                                             { "a\\b.iso" }, { "x\u0000.iso" } }) {
            try {
                TorrentMeta.parse(torrent("T", false, files(file(1, bad))));
                fail("taken: " + Arrays.toString(bad));
            } catch (IOException expected) {
                // refused
            }
        }
        try {
            TorrentMeta.parse(encode(map("info", map("name", "..", "length", 1L))));
            fail("a name of « .. » taken");
        } catch (IOException expected) {
            // refused
        }
    }

    @Test
    public void v2OnlyAndJunkAreRefused() {
        byte[][] bad = {
            encode(map("info", map("name", "T", "file tree", map("a", map("", map("length", 1L)))))),
            "not bencode".getBytes(StandardCharsets.US_ASCII),
            encode(map("announce", "x")),
            "d4:infod4:name1:a6:lengthi5e".getBytes(StandardCharsets.US_ASCII),   // truncated
            "d4:infod4:name99999:ae".getBytes(StandardCharsets.US_ASCII),          // string past the end
        };
        for (byte[] b : bad) {
            try {
                TorrentMeta.parse(b);
                fail("taken: " + new String(b, StandardCharsets.ISO_8859_1));
            } catch (IOException expected) {
                // refused
            }
        }
    }

    @Test
    public void magnetLinks() {
        String hex = "0123456789abcdef0123456789abcdef01234567";
        assertEquals(hex, TorrentMeta.magnetHash("magnet:?xt=urn:btih:" + hex.toUpperCase() + "&dn=Xbox"));
        assertEquals(hex, TorrentMeta.magnetHash("  MAGNET:?dn=x&xt=urn:btih:" + hex + "  "));
        // base32 of the same 20 bytes
        assertEquals(hex, TorrentMeta.magnetHash("magnet:?xt=urn:btih:AERUKZ4JVPG66AJDIVTYTK6N54ASGRLH"));
        assertNull(TorrentMeta.magnetHash("magnet:?xt=urn:btmh:1220" + hex + hex.substring(0, 24)));
        assertNull(TorrentMeta.magnetHash("http://example.invalid/a.torrent"));
        assertNull(TorrentMeta.magnetHash("magnet:?xt=urn:btih:123"));
        assertNull(TorrentMeta.magnetHash(null));
        assertEquals("My Xbox games", TorrentMeta.magnetName("magnet:?xt=urn:btih:" + hex + "&dn=My+Xbox%20games"));
        assertEquals("", TorrentMeta.magnetName("magnet:?xt=urn:btih:" + hex));
    }
}
