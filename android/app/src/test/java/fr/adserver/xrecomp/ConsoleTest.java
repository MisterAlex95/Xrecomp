package fr.adserver.xrecomp;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.After;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.zip.CRC32;

/**
 * Other consoles (XBA-147): the import filter, the console and title key read from a game's content (synthetic
 * headers and volumes built here, no game data), the console field of pack.json, and the route a game takes.
 */
@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ConsoleTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    /** XBA-182 / XBA-183: the consoles whose runtimes save states (the in-game menu's state rows). */
    @Test
    public void saveStatesPerConsole() {
        assertTrue(Console.GBA.hasSaveStates());
        assertTrue(Console.PS1.hasSaveStates());
        assertTrue(Console.N64.hasSaveStates());
        assertFalse(Console.XBOX.hasSaveStates());
    }

    private final Console.DiscKeyReader nativeChd = Console.chdReader;

    @After
    public void restoreChdReader() {
        Console.chdReader = nativeChd;
    }

    // ---- Synthetic games ------------------------------------------------------------------------------------------

    /** A homebrew-shaped GBA ROM: game code, 0x96 at 0xB2, the complement check at 0xBD. */
    static byte[] gba(String code) {
        byte[] b = new byte[0x400];
        for (int i = 0xC0; i < b.length; i++)
            b[i] = (byte)(i * 7);
        System.arraycopy("XRECOMPTEST ".getBytes(StandardCharsets.US_ASCII), 0, b, 0xA0, 12);
        System.arraycopy(code.getBytes(StandardCharsets.ISO_8859_1), 0, b, 0xAC, 4);
        b[0xB2] = (byte)0x96;
        int sum = 0;
        for (int i = 0xA0; i <= 0xBC; i++)
            sum -= b[i] & 0xff;
        b[0xBD] = (byte)(sum - 0x19);
        return b;
    }

    /** An N64 ROM header in byte order 0 (.z64), 1 (.v64) or 2 (.n64): CRC1 635A2BFF, game code NSME. */
    static byte[] n64(int order) {
        byte[] z = new byte[0x1000];
        z[0] = (byte)0x80; z[1] = 0x37; z[2] = 0x12; z[3] = 0x40;
        z[0x10] = 0x63; z[0x11] = 0x5A; z[0x12] = 0x2B; z[0x13] = (byte)0xFF;
        System.arraycopy("NSME".getBytes(StandardCharsets.US_ASCII), 0, z, 0x3B, 4);
        byte[] v = new byte[z.length];
        for (int i = 0; i < z.length; i++)
            v[i] = order == 0 ? z[i] : order == 1 ? z[i ^ 1] : z[(i & ~3) + 3 - (i & 3)];
        return v;
    }

    private static void put32(byte[] b, int o, long v) {
        b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24);
    }

    /** A PS1 data track: ISO 9660, system ID PLAYSTATION (or another), SYSTEM.CNF with cnf (none when null). */
    static byte[] ps1(boolean raw, String cnf, String systemId) {
        byte[][] s = new byte[24][2048];
        byte[] pvd = s[16];
        pvd[0] = 1;
        System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII), 0, pvd, 1, 5);
        pvd[6] = 1;
        for (int i = 8; i < 40; i++)
            pvd[i] = ' ';
        System.arraycopy(systemId.getBytes(StandardCharsets.US_ASCII), 0, pvd, 8, systemId.length());
        int r = 156;
        pvd[r] = 34; put32(pvd, r + 2, 20); put32(pvd, r + 10, 2048); pvd[r + 25] = 2; pvd[r + 32] = 1;
        s[17][0] = (byte)0xff;
        System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII), 0, s[17], 1, 5);
        byte[] root = s[20];
        root[0] = 34; put32(root, 2, 20); put32(root, 10, 2048); root[25] = 2; root[32] = 1; root[33] = 0;
        root[34] = 34; put32(root, 36, 20); put32(root, 44, 2048); root[59] = 2; root[66] = 1; root[67] = 1;
        int o = 68;
        root[o] = 46; put32(root, o + 2, 21); put32(root, o + 10, cnf == null ? 0 : cnf.length()); root[o + 32] = 12;
        System.arraycopy((cnf != null ? "SYSTEM.CNF;1" : "OTHER.TXT;1 ").getBytes(StandardCharsets.US_ASCII), 0, root,
                         o + 33, 12);
        if (cnf != null)
            System.arraycopy(cnf.getBytes(StandardCharsets.US_ASCII), 0, s[21], 0, cnf.length());
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] sec : s) {
            if (raw) {
                byte[] head = new byte[24];
                for (int i = 1; i <= 10; i++)
                    head[i] = (byte)0xff;
                head[15] = 2;
                out.write(head, 0, head.length);
                out.write(sec, 0, sec.length);
                out.write(new byte[2352 - 24 - 2048], 0, 2352 - 24 - 2048);
            } else {
                out.write(sec, 0, sec.length);
            }
        }
        return out.toByteArray();
    }

    private static void be32(byte[] b, int o, long v) {
        b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v;
    }

    private static long rbe32(byte[] b, int o) {
        return (b[o] & 0xffL) << 24 | (b[o + 1] & 0xffL) << 16 | (b[o + 2] & 0xffL) << 8 | (b[o + 3] & 0xffL);
    }

    /** CHD frames: 2352 bytes of sector + 96 of subcode; 8 per hunk (chdman's CD hunk). */
    static final int CHD_FRAME = 2448, CHD_HUNK = 8 * CHD_FRAME;

    /**
     * An uncompressed CHD v5 of a raw (2352-byte sector) data track, as consoles/ps1/tests/test_disc.c writes one:
     * 124-byte header, a map of 4-byte hunk numbers (hunk h at h * hunk bytes), one CHT2 track metadata entry, hunks
     * of 8 frames from hunk 1.
     */
    static byte[] chd(byte[] rawTrack) {
        int frames = rawTrack.length / 2352, hunks = (frames + 7) / 8;
        byte[] f = new byte[(hunks + 1) * CHD_HUNK];
        System.arraycopy("MComprHD".getBytes(StandardCharsets.US_ASCII), 0, f, 0, 8);
        be32(f, 8, 124);
        be32(f, 12, 5);
        be32(f, 36, (long)hunks * CHD_HUNK);   // logical bytes (low word)
        be32(f, 44, 124);                       // map offset (low word)
        int meta = 124 + hunks * 4;
        be32(f, 52, meta);                      // metadata offset (low word)
        be32(f, 56, CHD_HUNK);
        be32(f, 60, CHD_FRAME);
        for (int h = 0; h < hunks; h++)
            be32(f, 124 + h * 4, h + 1);
        String m = "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:" + frames
                   + " PREGAP:0 PGTYPE:MODE2_RAW PGSUB:NONE POSTGAP:0";
        System.arraycopy("CHT2".getBytes(StandardCharsets.US_ASCII), 0, f, meta, 4);
        be32(f, meta + 4, m.length() + 1);
        System.arraycopy(m.getBytes(StandardCharsets.US_ASCII), 0, f, meta + 16, m.length());
        for (int i = 0; i < frames; i++)
            System.arraycopy(rawTrack, i * 2352, f, CHD_HUNK + i * CHD_FRAME, 2352);
        return f;
    }

    /**
     * What the native reader (Ps1Native: recomp_pack_rom_identify + ps1_disc + libchdr) answers, for the uncompressed
     * CHDs above: the frames' sectors read as a .bin. The real reader is tested natively (consoles/ps1/tests
     * test_identify.c: uncompressed and cdzl CHDs); here only the app's side is: which files go to it, with what path.
     */
    static String chdKey(byte[] chd) {
        if (chd == null || chd.length < 124 || !new String(chd, 0, 8, StandardCharsets.US_ASCII).equals("MComprHD")
            || rbe32(chd, 12) != 5 || rbe32(chd, 16) != 0)
            return null;
        int hunkBytes = (int)rbe32(chd, 56), hunks = (int)(rbe32(chd, 36) / hunkBytes), map = (int)rbe32(chd, 44);
        ByteArrayOutputStream raw = new ByteArrayOutputStream();
        for (int h = 0; h < hunks; h++) {
            long at = rbe32(chd, map + h * 4) * hunkBytes;
            for (int i = 0; i < hunkBytes / CHD_FRAME && at + (i + 1) * (long)CHD_FRAME <= chd.length; i++)
                raw.write(chd, (int)(at + i * CHD_FRAME), 2352);
        }
        Console.Identity id = Console.identify(Console.source(raw.toByteArray()));
        return id != null && id.console == Console.PS1 ? id.key : null;
    }

    /** An Xbox XISO: "MICROSOFT*XBOX*MEDIA" at sector 32. */
    static byte[] xiso() {
        byte[] b = new byte[0x10800];
        System.arraycopy("MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII), 0, b, 0x10000, 20);
        return b;
    }

    private File file(String name, byte[] content) throws Exception {
        File f = new File(tmp.getRoot(), name);
        TestSupport.write(f, content);
        return f;
    }

    private static String crc(byte[] b) {
        CRC32 c = new CRC32();
        c.update(b);
        return String.format(Locale.ROOT, "%08X", c.getValue());
    }

    // ---- Import filter -----------------------------------------------------------------------------------------

    @Test
    public void importFilterByName() {
        assertEquals(Console.GBA, Console.byName("Game.gba"));
        assertEquals(Console.GBA, Console.byName("GAME.GBA"));
        assertEquals(Console.PS1, Console.byName("Game (Track 1).bin"));
        assertEquals(Console.PS1, Console.byName("Game.cue"));
        assertEquals(Console.PS1, Console.byName("Game.chd"));
        assertEquals(Console.N64, Console.byName("Game.z64"));
        assertEquals(Console.N64, Console.byName("Game.n64"));
        assertEquals(Console.N64, Console.byName("Game.V64"));
        assertEquals(Console.XBOX, Console.byName("Halo.xiso"));
        assertEquals(Console.XBOX, Console.byName("Halo.cso"));
        assertEquals(Console.XBOX, Console.byName("Halo.1.cci"));
        assertNull(Console.byName("Game.iso"));               // Xbox or PS1: the content says
        assertNull(Console.byName("Game.iso.1"));
        assertNull(Console.byName("notes.txt"));
        assertNull(Console.byName("default.xbe"));
        assertNull(Console.byName("gba"));
        assertNull(Console.byName(null));

        for (String n : new String[] { "a.gba", "a.bin", "a.cue", "a.chd", "a.iso", "a.z64", "a.n64", "a.v64",
                                       "a.xiso", "a.iso.1", "a.part1.iso" })
            assertTrue(n, Console.importable(n));
        for (String n : new String[] { "a.txt", "a.zip", "a.sav", "a.xbe", "a", "" })
            assertFalse(n, Console.importable(n));
    }

    @Test
    public void isoIsToldApartByContent() throws Exception {
        assertEquals(Console.XBOX, Console.detect(file("Halo.iso", xiso())));
        assertEquals(Console.PS1, Console.detect(file("Game.iso", ps1(false, "BOOT = cdrom:\\SLUS_005.94;1\n",
                                                                    "PLAYSTATION"))));
        assertEquals(Console.PS1, Console.detect(file("Raw.iso", ps1(true, null, "PLAYSTATION"))));
        assertNull(Console.detect(file("Other.iso", ps1(false, null, "WIN32"))));   // another ISO 9660 disc
        assertNull(Console.detect(file("Empty.iso", new byte[100])));
        assertEquals(Console.GBA, Console.detect(new File(tmp.getRoot(), "missing.gba")));   // by name alone
    }

    // ---- Title key ---------------------------------------------------------------------------------------------

    @Test
    public void gbaKeyIsGameCodeAndCrc() throws Exception {
        byte[] rom = gba("AXVE");
        Console.Identity id = Console.identify(file("Game.gba", rom));
        assertNotNull(id);
        assertEquals(Console.GBA, id.console);
        assertEquals("AXVE-" + crc(rom), id.key);
        assertEquals("gba-AXVE-" + crc(rom), id.packName());

        Console.Identity odd = Console.identify(file("Homebrew.gba", gba("a\0 1")));
        assertTrue(odd.key, odd.key.startsWith("___1-"));

        byte[] bad = gba("AXVE");
        bad[0xBD] ^= 1;
        assertNull(Console.identify(file("Bad.gba", bad)));
    }

    @Test
    public void n64KeyIsTheSameInEveryByteOrder() throws Exception {
        String[] ext = { "z64", "v64", "n64" };
        for (int order = 0; order < 3; order++) {
            Console.Identity id = Console.identify(file("Game." + ext[order], n64(order)));
            assertNotNull(ext[order], id);
            assertEquals(Console.N64, id.console);
            assertEquals("NSME-635A2BFF", id.key);
        }
    }

    @Test
    public void ps1KeyIsTheSerial() throws Exception {
        Console.Identity id = Console.identify(file("Game.iso", ps1(false, "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n",
                                                                     "PLAYSTATION")));
        assertEquals(Console.PS1, id.console);
        assertEquals("SLUS-00594", id.key);

        file("Game (Track 1).bin", ps1(true, "BOOT=cdrom:\\scus_944.26;1\n", "PLAYSTATION"));
        File cue = file("Game.cue", "FILE \"Game (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n"
                                    .getBytes(StandardCharsets.US_ASCII));
        assertEquals("SCUS-94426", Console.identify(new File(tmp.getRoot(), "Game (Track 1).bin")).key);
        Console.Identity viaCue = Console.identify(cue);
        assertEquals(Console.PS1, viaCue.console);
        assertEquals("SCUS-94426", viaCue.key);

        String exe = Console.identify(file("Homebrew.bin", ps1(true, "BOOT = cdrom:\\PSX.EXE;1\n", "PLAYSTATION"))).key;
        assertTrue(exe, exe.matches("PSX-[0-9A-F]{8}"));
        assertTrue(Console.identify(file("NoCnf.iso", ps1(false, null, "PLAYSTATION"))).key.startsWith("PSX-"));

        assertEquals("SLES-12345", Console.ps1Serial("BOOT = cdrom0:\\SLES_123.45;1"));
        assertNull(Console.ps1Serial("BOOT = cdrom:\\MAIN.EXE;1"));
        assertNull(Console.ps1Serial("VMODE = NTSC"));
    }

    /**
     * A .chd (XBA-159) goes to the native CHD reader with its path, by content (the « MComprHD » magic, whatever its
     * name), and its key is the one the same disc has as a .bin.
     */
    @Test
    public void chdKeyComesFromTheNativeReader() throws Exception {
        byte[] bin = ps1(true, "BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n", "PLAYSTATION");
        File disc = file("Game.chd", chd(bin));
        File renamed = file("Game.img", chd(bin));
        java.util.List<String> asked = new java.util.ArrayList<>();
        Console.chdReader = path -> {
            asked.add(path);
            try {
                return chdKey(java.nio.file.Files.readAllBytes(new File(path).toPath()));
            } catch (java.io.IOException e) {
                return null;
            }
        };
        Console.Identity id = Console.identify(disc);
        assertNotNull(id);
        assertEquals(Console.PS1, id.console);
        assertEquals("SLUS-00594", id.key);
        assertEquals(Console.identify(file("Game.bin", bin)).key, id.key);
        assertEquals("ps1-SLUS-00594", id.packName());
        assertEquals(java.util.Collections.singletonList(disc.getPath()), asked);
        assertEquals("SLUS-00594", Console.identify(renamed).key);

        // Another CD's CHD (no PLAYSTATION volume): no key.
        assertNull(Console.identify(file("Other.chd", chd(ps1(true, "BOOT = cdrom:\\SLUS_005.94;1\r\n", "WIN32")))));
        // Bytes with no path for native code (a test's Source): not read.
        asked.clear();
        assertNull(Console.identify(Console.source(chd(bin))));
        assertTrue(asked.isEmpty());
        // A key that cannot name a pack folder is refused.
        Console.chdReader = path -> "../x";
        assertNull(Console.identify(disc));
        // Without the native library (a JVM, an APK built without it): an « other file », as before.
        Console.chdReader = path -> null;
        assertNull(Console.identify(disc));
        assertNull(Ps1Native.titleKey(disc.getPath()));   // no libxba_import.so on the JVM
    }

    @Test
    public void xboxGamesAndOthersHaveNoKey() throws Exception {
        assertNull(Console.identify(file("Halo.iso", xiso())));
        assertNull(Console.identify(file("default.xbe", TestSupport.xbe(0x4D530004, "Halo"))));
        Console.chdReader = path -> chdKey("MComprHD".getBytes(StandardCharsets.US_ASCII));
        assertNull(Console.identify(file("Game.chd", "MComprHD".getBytes(StandardCharsets.US_ASCII))));
        assertNull(Console.identify(file("Loop.cue", "FILE \"Loop.cue\" BINARY\n".getBytes(StandardCharsets.US_ASCII))));
        assertNull(Console.identify(new File(tmp.getRoot(), "missing.gba")));
        assertNull(Console.identify(tmp.getRoot()));
    }

    @Test
    public void packDirNames() {
        assertEquals("4D530004", Console.packDirName(Console.XBOX, "0x4d530004"));
        assertEquals("ps1-SLUS-00594", Console.packDirName(Console.PS1, "SLUS-00594"));
        assertNull(Console.packDirName(Console.GBA, "../x"));
        assertNull(Console.packDirName(Console.GBA, ""));
        assertNull(Console.packDirName(null, "A"));
    }

    // ---- pack.json and dispatch --------------------------------------------------------------------------------

    @Test
    public void consoleField() {
        assertEquals(Console.XBOX, Console.fromPack(null));
        assertEquals(Console.XBOX, Console.fromPack(""));
        assertEquals(Console.XBOX, Console.fromPack("xbox"));
        assertEquals(Console.GBA, Console.fromPack("gba"));
        assertEquals(Console.PS1, Console.fromPack("ps1"));
        assertEquals(Console.N64, Console.fromPack("n64"));
        assertNull(Console.fromPack("GBA"));
        assertNull(Console.fromPack("snes"));
    }

    /** A pack for a game of another console, as a future Prepare writes it. */
    private File writeConsolePack(Console c, String key, String console, int libSize) throws Exception {
        File dir = Pack.packDir(tmp.getRoot(), c, key);
        TestSupport.write(new File(dir, "libtitle.so"), new byte[libSize]);
        JSONObject o = new JSONObject();
        o.put("format", 1);
        o.put("console", console);
        o.put("title_key", key);
        o.put("abi", "arm64-v8a");
        o.put("shell_abi", Pack.SHELL_ABI);
        o.put("lib", "libtitle.so");
        o.put("lib_size", libSize);
        TestSupport.write(new File(dir, "pack.json"), o.toString());
        return dir;
    }

    @Test
    public void oldXboxPackStillValid() throws Exception {
        File files = tmp.getRoot();
        TestSupport.writePack(files, "4D530004", 10);         // no console field
        assertEquals(Pack.Check.VALID, Pack.check(files, "4D530004"));
        assertEquals(Pack.Check.VALID, Pack.check(files, Console.XBOX, "4D530004"));

        File json = new File(Pack.packDir(files, "4D530004"), "pack.json");
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("console", "xbox");
        TestSupport.write(json, o.toString());
        assertEquals(Pack.Check.VALID, Pack.check(files, "4D530004"));
        o.put("console", "gba");
        TestSupport.write(json, o.toString());
        assertEquals(Pack.Check.WRONG_CONSOLE, Pack.check(files, "4D530004"));
    }

    @Test
    public void consolePackCheck() throws Exception {
        File files = tmp.getRoot();
        assertEquals(Pack.Check.NO_PACK, Pack.check(files, Console.GBA, "AXVE-00000001"));
        File dir = writeConsolePack(Console.GBA, "AXVE-00000001", "gba", 64);
        assertEquals("gba-AXVE-00000001", dir.getName());
        assertEquals(Pack.Check.VALID, Pack.check(files, Console.GBA, "AXVE-00000001"));
        assertEquals(Pack.Check.NO_TITLE_ID, Pack.check(files, Console.GBA, "../.."));

        writeConsolePack(Console.GBA, "AXVE-00000001", "n64", 64);         // the pack says another console
        assertEquals(Pack.Check.WRONG_CONSOLE, Pack.check(files, Console.GBA, "AXVE-00000001"));
        writeConsolePack(Console.GBA, "AXVE-00000001", "", 64);            // no console: an Xbox pack's
        assertEquals(Pack.Check.WRONG_CONSOLE, Pack.check(files, Console.GBA, "AXVE-00000001"));

        File json = new File(writeConsolePack(Console.N64, "NSME-635A2BFF", "n64", 8), "pack.json");
        JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 16));
        o.put("title_key", "NSME-00000000");
        TestSupport.write(json, o.toString());
        assertEquals(Pack.Check.WRONG_TITLE, Pack.check(files, Console.N64, "NSME-635A2BFF"));
        o.put("title_key", "NSME-635A2BFF");
        o.put("lib_size", 9);
        TestSupport.write(json, o.toString());
        assertEquals(Pack.Check.LIB_SIZE, Pack.check(files, Console.N64, "NSME-635A2BFF"));
    }

    @Test
    public void routeDispatchesByConsole() throws Exception {
        File files = new File(tmp.getRoot(), "files");
        byte[] rom = gba("XRCT");
        File game = file("Game.gba", rom);
        String key = "XRCT-" + crc(rom);

        Pack.Route r = Pack.route(files, game);
        assertEquals(Console.GBA, r.console);
        assertEquals(key, r.key);
        assertEquals(Pack.Check.NO_PACK, r.check);                  // the launcher offers Prepare

        File dir = Pack.packDir(files, Console.GBA, key);
        TestSupport.write(new File(dir, "libtitle.so"), new byte[64]);
        TestSupport.write(new File(dir, "pack.json"),
                          "{\"format\":1,\"console\":\"gba\",\"title_key\":\"" + key + "\",\"abi\":\"arm64-v8a\","
                          + "\"shell_abi\":" + Pack.SHELL_ABI + ",\"lib\":\"libtitle.so\",\"lib_size\":64}");
        assertEquals(Pack.Check.VALID, Pack.route(files, game).check);

        Pack.Route p = Pack.route(files, file("Game.iso", ps1(false, "BOOT = cdrom:\\SLUS_005.94;1\n", "PLAYSTATION")));
        assertEquals(Console.PS1, p.console);
        assertEquals("SLUS-00594", p.key);
        assertEquals(Console.N64, Pack.route(files, file("Game.v64", n64(1))).console);

        // The Xbox path, unchanged: a game folder by its default.xbe's title ID, an image by name or content.
        File folder = new File(tmp.getRoot(), "halo");
        TestSupport.write(new File(folder, "default.xbe"), TestSupport.xbe(0x4D530004, "Halo"));
        TestSupport.writePack(files, "4D530004", 10);
        Pack.Route x = Pack.route(files, folder);
        assertEquals(Console.XBOX, x.console);
        assertEquals("4D530004", x.key);
        assertEquals(Pack.Check.VALID, x.check);
        Pack.Route xi = Pack.route(files, file("Halo.iso", xiso()));
        assertEquals(Console.XBOX, xi.console);
        assertNull(xi.key);

        assertNull(Pack.route(files, file("notes.txt", new byte[10])));
        assertNull(Pack.route(files, new File(tmp.getRoot(), "empty-folder")));
        assertNull(Pack.route(files, null));
    }
}
