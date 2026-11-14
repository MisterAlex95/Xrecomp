package fr.adserver.xrecomp;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Ps1Disc (XBA-168): the sectors of a PS1 disc document that lead to its boot EXE, copied at their place into a sparse
 * cooked image tools.prepare reads like the disc: SYSTEM.CNF's boot file (any case, a sub-directory, « cdrom » with or
 * without its colon), PSX.EXE without SYSTEM.CNF, raw and cooked tracks; a disc whose boot file is missing, or no disc.
 */
public class Ps1DiscTest {
    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static void put(byte[] b, int o, long v) {
        for (int i = 0; i < 4; i++) {
            b[o + i] = (byte)(v >> (8 * i));
            b[o + 7 - i] = (byte)(v >> (8 * i));
        }
    }

    /** A directory record: returns its length. */
    private static int rec(byte[] d, int o, String name, int lba, int size, boolean dir) {
        byte[] n = name.getBytes(StandardCharsets.ISO_8859_1);
        int len = 33 + n.length + ((33 + n.length) & 1);
        d[o] = (byte)len;
        put(d, o + 2, lba);
        put(d, o + 10, size);
        d[o + 25] = (byte)(dir ? 2 : 0);
        d[o + 32] = (byte)n.length;
        System.arraycopy(n, 0, d, o + 33, n.length);
        return len;
    }

    static byte[] exe(int size) {
        byte[] e = new byte[size];
        for (int i = 0; i < size; i++)
            e[i] = (byte)(i * 7 + 3);
        System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII), 0, e, 0, 8);
        return e;
    }

    /**
     * A disc: 16 PVD, 18 root, 19 the directory GAMES, 20 SYSTEM.CNF (unless null), 30.. the EXE named exeName in
     * GAMES (inSub) or the root; 60 sectors (raw: 2352-byte mode 2 form 1).
     */
    static byte[] disc(boolean raw, String cnf, String exeName, boolean inSub, byte[] exe) {
        byte[][] s = new byte[60][2048];
        byte[] pvd = s[16];
        pvd[0] = 1;
        System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII), 0, pvd, 1, 5);
        System.arraycopy("PLAYSTATION".getBytes(StandardCharsets.US_ASCII), 0, pvd, 8, 11);
        rec(pvd, 156, "\0", 18, 2048, true);
        byte[] root = s[18], sub = s[19];
        int o = rec(root, 0, "\0", 18, 2048, true);
        o += rec(root, o, "\1", 18, 2048, true);
        if (cnf != null) {
            o += rec(root, o, "SYSTEM.CNF;1", 20, cnf.length(), false);
            System.arraycopy(cnf.getBytes(StandardCharsets.ISO_8859_1), 0, s[20], 0, cnf.length());
        }
        o += rec(root, o, "GAMES", 19, 2048, true);
        if (!inSub)
            rec(root, o, exeName, 30, exe.length, false);
        int k = rec(sub, 0, "\0", 19, 2048, true);
        k += rec(sub, k, "\1", 18, 2048, true);
        if (inSub)
            rec(sub, k, exeName, 30, exe.length, false);
        for (int i = 0; i < exe.length; i += 2048)
            System.arraycopy(exe, i, s[30 + i / 2048], 0, Math.min(2048, exe.length - i));
        s[50][0] = 0x55;   // a sector the Prepare never needs
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] sec : s) {
            if (raw) {
                byte[] head = new byte[24];
                for (int i = 1; i <= 10; i++)
                    head[i] = (byte)0xff;
                head[15] = 2;
                out.write(head, 0, 24);
                out.write(sec, 0, 2048);
                out.write(new byte[2352 - 24 - 2048], 0, 2352 - 24 - 2048);
            } else {
                out.write(sec, 0, 2048);
            }
        }
        return out.toByteArray();
    }

    private static byte[] sector(byte[] img, int lba) {
        byte[] s = new byte[2048];
        System.arraycopy(img, lba * 2048, s, 0, 2048);
        return s;
    }

    @Test
    public void copiesWhatLeadsToTheBootExe() throws Exception {
        byte[] exe = exe(5000);
        for (boolean raw : new boolean[] { false, true }) {
            byte[] d = disc(raw, "boot = cdrom\\games\\game.exe;1\r\nTCB=4\r\n", "GAME.EXE;1", true, exe);
            File out = new File(tmp.getRoot(), "disc" + raw + ".iso");
            Ps1Disc.Result r = Ps1Disc.extract(Console.source(d), out);
            assertEquals("\\games\\game.exe;1", r.boot);
            assertEquals(5000, r.exeSize);
            byte[] img = Files.readAllBytes(out.toPath());
            assertEquals("up to the EXE's last sector", 33 * 2048, img.length);
            for (int lba : new int[] { 16, 18, 19, 20, 30, 31, 32 }) {
                byte[] want = new byte[2048];
                System.arraycopy(d, raw ? lba * 2352 + 24 : lba * 2048, want, 0, 2048);
                assertArrayEquals("sector " + lba, want, sector(img, lba));
            }
            assertArrayEquals(new byte[2048], sector(img, 0));
            assertArrayEquals(new byte[2048], sector(img, 25));
            // What tools.prepare finds in the copy: the same EXE.
            byte[] got = new byte[5000];
            System.arraycopy(img, 30 * 2048, got, 0, 5000);
            assertArrayEquals(exe, got);
            assertFalse(new File(out.getPath() + ".tmp").exists());
        }
    }

    /** A raw image served sector by sector, as the native reader serves a .chd (Ps1Native.Disc). */
    static Ps1Native.Disc nativeDisc(byte[] raw, boolean[] closed) {
        return new Ps1Native.Disc() {
            @Override
            public int sectors() {
                return raw.length / 2352;
            }

            @Override
            public boolean read(int lba, byte[] out) {
                if (lba < 0 || lba >= sectors())
                    return false;
                System.arraycopy(raw, lba * 2352, out, 0, 2352);
                return true;
            }

            @Override
            public void close() {
                closed[0] = true;
            }
        };
    }

    /** XBA-168: a .chd through the native reader (raw sectors): the same copy as from its raw image. */
    @Test
    public void nativeDiscAsRawImage() throws Exception {
        byte[] exe = exe(7000);
        byte[] d = disc(true, "BOOT=cdrom:\\GAME.EXE;1\n", "GAME.EXE;1", false, exe);
        Console.Source src = Ps1Disc.raw(nativeDisc(d, new boolean[1]));
        assertEquals(d.length, src.length());
        byte[] head = new byte[3000];
        assertEquals("one sector per read at most", 2352 - 100, src.read(100, head, 0, head.length));
        assertEquals(-1, src.read(d.length, head, 0, 10));
        File a = new File(tmp.getRoot(), "a.iso"), b = new File(tmp.getRoot(), "b.iso");
        Ps1Disc.Result r = Ps1Disc.extract(src, a);
        Ps1Disc.extract(Console.source(d), b);
        assertEquals("\\GAME.EXE;1", r.boot);
        assertArrayEquals(Files.readAllBytes(b.toPath()), Files.readAllBytes(a.toPath()));
    }

    @Test
    public void psxExeWithoutSystemCnf() throws Exception {
        byte[] exe = exe(2048);
        File out = new File(tmp.getRoot(), "psx.iso");
        Ps1Disc.Result r = Ps1Disc.extract(Console.source(disc(false, null, "PSX.EXE;1", false, exe)), out);
        assertEquals("PSX.EXE", r.boot);
        assertEquals(31 * 2048, out.length());
        // A SYSTEM.CNF without BOOT: PSX.EXE too.
        r = Ps1Disc.extract(Console.source(disc(false, "TCB=4\n", "PSX.EXE;1", false, exe)), out);
        assertEquals("PSX.EXE", r.boot);
    }

    @Test
    public void bootPath() {
        assertEquals("\\SLUS_005.94;1", Ps1Disc.bootPath("BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n"));
        assertEquals("\\X.EXE;1", Ps1Disc.bootPath("boot=CDROM\\X.EXE;1"));
        assertEquals("\\A\\B.EXE", Ps1Disc.bootPath("TCB=4\nBOOT =\n  cdrom:\\A\\B.EXE\n"));
        assertNull(Ps1Disc.bootPath("TCB=4\n"));
    }

    @Test
    public void refusesWhatItCannotRead() throws Exception {
        File out = new File(tmp.getRoot(), "x.iso");
        try {
            Ps1Disc.extract(Console.source(disc(false, "BOOT=cdrom:\\GONE.EXE;1\n", "GAME.EXE;1", false, exe(100))),
                            out);
            fail("no boot EXE");
        } catch (IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("GONE.EXE"));
        }
        try {
            Ps1Disc.extract(Console.source(new byte[100000]), out);
            fail("not a disc");
        } catch (IOException e) {
            assertTrue(e.getMessage(), e.getMessage().contains("ISO 9660"));
        }
        assertFalse(out.exists());
        assertFalse(new File(out.getPath() + ".tmp").exists());
    }
}
