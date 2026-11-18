package fr.adserver.xrecomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.CRC32;

/**
 * The consoles XRecomp runs (XBA-147; Plane « Spec · Other consoles », consoles/README.md): Xbox, and Game Boy Advance,
 * PlayStation, Nintendo 64 and GameCube (homebrew DOLs for now, XBA-259) statically recompiled the same way. pack.json's "console" says which one a title pack is
 * for (absent: xbox, every pack made before the field); the native loader (pack_loader.c) hands a pack to that
 * console's runtime.
 *
 * Mirrors src/platform/recomp_pack.c: the import filter (file names the app takes as games of each console), and
 * {@link #identify}, the console and generic title key read from a game file's content, which names its pack folder
 * (packs/&lt;console&gt;-&lt;key&gt;; an Xbox pack stays packs/&lt;TITLEID&gt;).
 */
enum Console {
    XBOX("xbox"), GBA("gba"), PS1("ps1"), N64("n64"), GC("gc"), X360("x360");

    /** pack.json's spelling. */
    final String id;

    Console(String id) {
        this.id = id;
    }

    /** pack.json's "console": absent or empty is XBOX (a pack from before the field); an unknown name is null. */
    static Console fromPack(String v) {
        if (v == null || v.isEmpty())
            return XBOX;
        for (Console c : values())
            if (c.id.equals(v))
                return c;
        return null;
    }

    /**
     * XBA-182 / XBA-183: the console's runtime saves states and fast-forwards (recomp_states.h): GBA, PS1, N64 and
     * GameCube (XBA-264: gc_state.c, rewind and the automatic slot too). An Xbox game goes through its own path
     * (XBA-249: the runtime says whether states are available).
     */
    boolean hasSaveStates() {
        return this == GBA || this == PS1 || this == N64 || this == GC || this == X360;
    }

    /**
     * XBA-240: the console's games fast-forward (the in-game menu's toggle, the fast-forward hotkeys): every console.
     * GBA, PS1 and N64 run several console frames per shown frame; an Xbox title's guest clock runs faster
     * (src/kernel/guest_clock.h).
     */
    boolean hasFastForward() {
        return true;
    }

    /** The console's short name on a library card's badge (a product name: not translated). */
    String badge() {
        switch (this) {
        case GBA:
            return "GBA";
        case PS1:
            return "PS1";
        case N64:
            return "N64";
        case GC:
            return "GC";
        case X360:
            return "360";
        default:
            return "Xbox";
        }
    }

    /**
     * Its games play in this build: the Xbox (through Prepare and a pack), the Game Boy Advance (XBA-157), the
     * PlayStation (XBA-168), the Nintendo 64 (XBA-174) and the GameCube (XBA-259: homebrew DOLs), whose runtimes are
     * interpreters: no import, no pack needed, the game file goes to the game activity as it is. Every console XRecomp
     * lists plays now; « Not playable yet » stays for the next one.
     */
    boolean playable() {
        return this == XBOX || this == GBA || this == PS1 || this == N64 || this == GC || this == X360;
    }

    /**
     * A game of this console starts from its file as it is, no Prepare or import (GBA, PS1, N64, GameCube DOL). The
     * Xbox 360 (XBA-280) too: the game process looks for its pack (packs/x360-&lt;KEY&gt;/, made by Prepare on the
     * device since XBA-282, or on a desktop by x360_tools.bringup --app-pack) and goes back to the launcher without
     * one.
     */
    boolean directLaunch() {
        return this == GBA || this == PS1 || this == N64 || this == GC || this == X360;
    }

    /**
     * A game of this console that starts from its file can also be prepared (translated for the device into a pack):
     * the GBA (XBA-158), the PS1 (XBA-168; a .chd too), the N64 (XBA-174) and the GameCube (XBA-264: a disc in any
     * image format, or a DOL) and the Xbox 360 (XBA-282: an XEX or a disc image, with the player's key file; its
     * runtime has no interpreter, so a 360 game plays only once prepared).
     */
    boolean prepares() {
        return this == GBA || this == PS1 || this == N64 || this == GC || this == X360;
    }

    /**
     * The console of a library key (games.json's and the index's game key, docs « Game library »): « gba-… »,
     * « ps1-… », « n64-… » (packDirName) for the other consoles; anything else is an Xbox title ID or « local-… ».
     */
    static Console ofKey(String key) {
        if (key != null) {
            for (Console c : values())
                if (c != XBOX && key.startsWith(c.id + "-"))
                    return c;
        }
        return XBOX;
    }

    // ---- Import filter ------------------------------------------------------------------------------------------

    /**
     * The console a game file's name says, for the import filter: .gba; PS1 .bin, .cue, .chd; N64 .z64, .n64, .v64;
     * GameCube .dol (XBA-259: homebrew programs), .gcm, .rvz, .gcz, .wia (XBA-261: discs); the Xbox images
     * (DiscImages.isImageName: .xiso .cso .ciso .zso .cci and split parts; a .ciso may also be a GameCube disc:
     * {@link #detect} reads it). Null for anything else, and for a plain .iso, which an Xbox, a PS1 and a GameCube
     * disc all use: {@link #detect} reads its content.
     */
    static Console byName(String name) {
        if (name == null)
            return null;
        String n = name.toLowerCase(Locale.ROOT);
        String ext = n.lastIndexOf('.') >= 0 ? n.substring(n.lastIndexOf('.') + 1) : "";
        switch (ext) {
        case "gba":
            return GBA;
        case "bin": case "cue": case "chd":
            return PS1;
        case "z64": case "n64": case "v64":
            return N64;
        case "dol": case "gcm": case "rvz": case "gcz": case "wia":
            return GC;
        case "xex":
            return X360;   // XBA-280: an Xbox 360 executable
        case "iso":
            return null;
        default:
            return DiscImages.isImageName(name) && !isPlainIso(name) ? XBOX : null;
        }
    }

    /** A name the import takes: one {@link #byName} places, or a plain .iso (placed by content). */
    static boolean importable(String name) {
        return byName(name) != null || isPlainIso(name);
    }

    /** An .iso, or a split part of one (Game.1.iso, Game.iso.1): Xbox or PS1, only the content tells. */
    static boolean isPlainIso(String name) {
        if (name == null)
            return false;
        String n = name.toLowerCase(Locale.ROOT);
        return n.endsWith(".iso") || (DiscImages.isImageName(name) && n.matches(".*\\.iso\\.\\d{1,4}$"));
    }

    /**
     * The console of a game file: by name, and for an .iso by content ({@link #sniffIso}); null when it is no game the
     * app takes (or an .iso of neither kind).
     */
    static Console detect(File f) {
        if (f == null)
            return null;
        if (isPlainIso(f.getName()))
            return sniffIso(f);
        if (f.getName().toLowerCase(Locale.ROOT).endsWith(".ciso") && sniffIso(f) == GC)
            return GC;   // a GameCube CISO; an Xbox one stays XBOX (by name)
        Console c = byName(f.getName());
        if (c == XBOX && sniffIso(f) == X360)
            return X360;   // XBA-280: an Xbox 360 image under an Xbox image's name (.xiso)
        return c;
    }

    /**
     * Where an Xbox image has its XDVDFS volume: XISO (0), redump full images (video partition first). An Xbox 360
     * image (XBA-280) has it at its game partition (XGD2 0xFD90000, XGD3 0x2080000), or at 0 extracted, with
     * default.xex in its root ({@link #x360DiscKey}).
     */
    private static final long[] XISO_OFFSETS = { 0L, 0x18300000L };
    private static final long[] X360_OFFSETS = { 0xFD90000L, 0x2080000L, 0L };
    private static final byte[] XISO_MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /**
     * An .iso's console by content: XBOX for the Xbox volume magic "MICROSOFT*XBOX*MEDIA" at sector 32 of the image or
     * of its game partition (checked first: a redump Xbox image also starts with an ISO 9660 video partition), PS1 for
     * an ISO 9660 volume whose system ID is "PLAYSTATION" (2048- or 2352-byte sectors), GC for a GameCube disc header
     * (magic 0xC2339F3D at 0x1C, {@link #gcHeader}: also CISO, GCZ, RVZ / WIA), else null.
     */
    static Console sniffIso(File f) {
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            return sniffIso(source(raf.getChannel()));
        } catch (IOException e) {
            return null;
        }
    }

    /** {@link #sniffIso(File)} over any seekable content (a document of the library folder, a test's bytes). */
    static Console sniffIso(Source src) {
        try {
            Rd r = new Rd(src);
            for (long base : X360_OFFSETS)
                if (x360DiscKey(r, base) != null)
                    return X360;
            byte[] m = new byte[XISO_MAGIC.length];
            for (long base : XISO_OFFSETS) {
                long at = base + 0x10000L;
                if (at + m.length > r.length())
                    continue;
                r.seek(at);
                r.readFully(m);
                if (java.util.Arrays.equals(m, XISO_MAGIC))
                    return XBOX;
            }
            Iso iso = Iso.open(r);
            if (iso != null && iso.playstation())
                return PS1;
            byte[] h = new byte[0xC0];
            r.seek(0);
            int hn = r.read(h);
            return gcHeader(r, h, hn) != null ? GC : null;
        } catch (IOException e) {
            return null;
        }
    }

    // ---- Title key ----------------------------------------------------------------------------------------------

    /** A game of another console than the Xbox: its console and generic title key. */
    static final class Identity {
        final Console console;
        final String key;

        Identity(Console console, String key) {
            this.console = console;
            this.key = key;
        }

        /** The pack folder's name, packs/&lt;console&gt;-&lt;key&gt;. */
        String packName() {
            return packDirName(console, key);
        }
    }

    /**
     * The console and title key of a GBA, PS1 or N64 game, from its content (recomp_pack_rom_identify):
     * <ul>
     * <li>GBA: game code + CRC32 of the file, « AXVE-1A2B3C4D » (header: 0x96 at 0xB2, complement check at 0xBD);</li>
     * <li>N64: game code + header CRC1, « NSME-635A2BFF », from any byte order (.z64 / .v64 / .n64);</li>
     * <li>PS1: the serial of SYSTEM.CNF's BOOT line, « SLUS-00594 » (ISO 9660 in 2048- or 2352-byte sectors, system
     * ID PLAYSTATION; without a serial-shaped boot file « PSX-&lt;CRC32 of the volume descriptor&gt; »; a .cue: its
     * first FILE).</li>
     * <li>GameCube DOL (XBA-259): « DOL-&lt;CRC32 of the file&gt; », when every field of its 256-byte header makes
     * sense (no magic: 7 text + 11 data sections inside the file and MEM1, a text section holding the entry, the BSS in
     * MEM1, the header's padding zero).</li>
     * <li>GameCube (XBA-261): game code + maker + « - » + revision, « -D&lt;n&gt; » added for the second disc of a set
     * and later (n from 2): « GALE01-0 », « GXXE01-1-D2 » (disc header magic 0xC2339F3D at 0x1C, read after the PS1
     * check, from an ISO / GCM, CISO, GCZ or RVZ / WIA image: {@link #gcHeader}).</li>
     * </ul>
     * A game code byte outside [A-Z0-9] becomes '_'. A CHD (« MComprHD » at 0: a PlayStation disc, compressed) is read
     * by the native disc reader ({@link #chdReader}, XBA-159). Null for anything else: an Xbox game.
     */
    static Identity identify(File f) {
        if (f == null || !f.isFile())
            return null;
        if (f.getName().toLowerCase(Locale.ROOT).endsWith(".cue")) {
            File bin = cueBin(f);
            if (bin == null || bin.getName().toLowerCase(Locale.ROOT).endsWith(".cue"))
                return null;
            Identity id = identify(bin);
            return id != null && id.console == PS1 ? id : null;
        }
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            return identify(source(raf.getChannel(), f.getPath()));
        } catch (IOException e) {
            return null;
        }
    }

    /** The PS1 title key of a disc only native code reads (a CHD), from a path or « fd:N »; null when none. */
    interface DiscKeyReader {
        String ps1Key(String nativePath);
    }

    /**
     * The CHD reader (XBA-159): libxba_import.so's recomp_pack_rom_identify with the PS1 disc reader (Ps1Native), the
     * one the game process uses: CHD hunks are zlib, LZMA, zstd or FLAC, and only zlib is in the JDK. Tests substitute
     * their own.
     */
    static volatile DiscKeyReader chdReader = Ps1Native::titleKey;

    private static final byte[] CHD_MAGIC = "MComprHD".getBytes(StandardCharsets.US_ASCII);

    /** h starts with a CHD's magic (MAME compressed hunks of data, any version). */
    static boolean isChd(byte[] h, int hn) {
        if (hn < CHD_MAGIC.length)
            return false;
        for (int i = 0; i < CHD_MAGIC.length; i++)
            if (h[i] != CHD_MAGIC[i])
                return false;
        return true;
    }

    /** A CHD's identity through {@link #chdReader}: a PS1 disc with a key that names a folder, else null. */
    private static Identity chdIdentity(Source src) {
        String path = src.nativePath();
        DiscKeyReader r = chdReader;
        String k = path == null || r == null ? null : r.ps1Key(path);
        return k != null && packDirName(PS1, k) != null ? new Identity(PS1, k) : null;
    }

    /**
     * {@link #identify(File)} over any seekable content: the library scan reads a document of a folder seen through
     * the Storage Access Framework this way. A .cue is not followed here (the caller resolves its FILE: cueFiles).
     */
    static Identity identify(Source src) {
        try {
            Rd r = new Rd(src);
            byte[] h = new byte[0xC0];
            int hn = r.read(h);
            String k;
            if (isChd(h, hn))
                return chdIdentity(src);
            if ((k = x360Key(r, h, hn)) != null)   // first: a 360 disc's video partition is an ISO 9660 volume
                return new Identity(X360, k);
            if ((k = n64Key(h, hn)) != null)
                return new Identity(N64, k);
            if ((k = gbaKey(r, h, hn)) != null)
                return new Identity(GBA, k);
            Iso iso = Iso.open(r);
            if (iso != null && iso.playstation())
                return new Identity(PS1, iso.ps1Key());
            byte[] gc = gcHeader(r, h, hn);
            if (gc != null)
                return new Identity(GC, gcKey(gc));
            if ((k = dolKey(r)) != null)
                return new Identity(GC, k);
            return null;
        } catch (IOException e) {
            return null;
        }
    }

    // ---- Xbox 360 (XBA-280): recomp_pack.c's x360_key ---------------------------------------------------------

    /**
     * An XEX's key from its header's execution info (optional header 0x00040006, which a plain rewrite keeps):
     * « X360-&lt;title id&gt;-&lt;CRC32 of the execution info's 24 bytes&gt; »; null without one.
     */
    static String xexKey(byte[] x, int n) {
        if (n < 24 || x[0] != 'X' || x[1] != 'E' || x[2] != 'X' || x[3] != '2')
            return null;
        long count = be32(x, 20);
        if (count > 64 || 24 + count * 8 > n)
            return null;
        for (int i = 0; i < count; i++) {
            int h = 24 + 8 * i;
            if (be32(x, h) != 0x00040006L)
                continue;
            long at = be32(x, h + 4);
            if (at + 24 > n)
                return null;
            java.util.zip.CRC32 crc = new java.util.zip.CRC32();
            crc.update(x, (int) at, 24);
            return String.format(Locale.ROOT, "X360-%08X-%08X", be32(x, (int) at + 12), crc.getValue());
        }
        return null;
    }

    private static String xexKeyAt(Rd r, long at) throws IOException {
        if (at < 0 || at >= r.length())
            return null;
        byte[] b = new byte[(int) Math.min(65536L, r.length() - at)];
        r.seek(at);
        r.readFully(b);
        return xexKey(b, b.length);
    }

    private static long le32x(byte[] b, int o) {
        return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 | (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24;
    }

    /** default.xex in the root of the XDVDFS volume at base: its key; null without a volume or such a file. */
    static String x360DiscKey(Rd r, long base) throws IOException {
        long[] at = x360DiscXex(r, base);
        return at == null ? null : xexKeyAt(r, at[0]);
    }

    /**
     * XBA-282: where default.xex lies in an Xbox 360 disc image, { offset, size } (an XDVDFS file is contiguous), or
     * null when the image holds none at any 360 partition offset. PrepareService copies it out of a disc document.
     */
    static long[] x360DiscXex(Source src) throws IOException {
        Rd r = new Rd(src);
        for (long base : X360_OFFSETS) {
            long[] at = x360DiscXex(r, base);
            if (at != null)
                return at;
        }
        return null;
    }

    private static long[] x360DiscXex(Rd r, long base) throws IOException {
        byte[] vd = new byte[0x800];
        if (base + 0x10000L + vd.length > r.length())
            return null;
        r.seek(base + 0x10000L);
        r.readFully(vd);
        for (int i = 0; i < XISO_MAGIC.length; i++)
            if (vd[i] != XISO_MAGIC[i])
                return null;
        long root = le32x(vd, 20), size = le32x(vd, 24);
        if (size == 0 || size > (1 << 20) || base + root * 2048 + size > r.length())
            return null;
        byte[] dir = new byte[(int) size];
        r.seek(base + root * 2048);
        r.readFully(dir);
        java.util.ArrayDeque<Integer> todo = new java.util.ArrayDeque<>();
        todo.push(0);
        for (int visits = 0; !todo.isEmpty() && visits < 4096; visits++) {
            int o = todo.pop();
            if (o + 14 > size)
                continue;
            int left = (dir[o] & 0xFF) | (dir[o + 1] & 0xFF) << 8, right = (dir[o + 2] & 0xFF) | (dir[o + 3] & 0xFF) << 8;
            int len = dir[o + 13] & 0xFF;
            if (left == 0xFFFF && right == 0xFFFF)
                continue;
            if (len == 11 && o + 14 + len <= size
                && new String(dir, o + 14, len, StandardCharsets.ISO_8859_1).equalsIgnoreCase("default.xex"))
                return new long[] { base + le32x(dir, o + 4) * 2048, le32x(dir, o + 8) };
            if (left != 0 && left != 0xFFFF && todo.size() < 256)
                todo.push(left * 4);
            if (right != 0 && right != 0xFFFF && todo.size() < 256)
                todo.push(right * 4);
        }
        return null;
    }

    /** An XEX's or an Xbox 360 disc image's key (recomp_pack.c x360_key), else null. */
    static String x360Key(Rd r, byte[] h, int hn) throws IOException {
        if (hn >= 4 && h[0] == 'X' && h[1] == 'E' && h[2] == 'X' && h[3] == '2')
            return xexKeyAt(r, 0);
        for (long base : X360_OFFSETS) {
            String k = x360DiscKey(r, base);
            if (k != null)
                return k;
        }
        return null;
    }

    /** The pack folder's name: the normalised title ID for XBOX, « &lt;console&gt;-&lt;key&gt; » otherwise. */
    static String packDirName(Console c, String key) {
        if (c == null || key == null)
            return null;
        if (c == XBOX)
            return Pack.normTitleId(key);
        return key.matches("[A-Za-z0-9_-]{1,40}") ? c.id + "-" + key : null;
    }

    private static char codeChar(byte b) {
        return (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') ? (char)b : '_';
    }

    private static String gbaKey(Rd r, byte[] h, int hn) throws IOException {
        if (hn < 0xC0 || (h[0xB2] & 0xff) != 0x96)
            return null;
        int sum = 0;
        for (int i = 0xA0; i <= 0xBC; i++)
            sum -= h[i] & 0xff;
        if (((sum - 0x19) & 0xff) != (h[0xBD] & 0xff))
            return null;
        CRC32 crc = new CRC32();
        byte[] buf = new byte[65536];
        r.seek(0);
        int n;
        while ((n = r.read(buf)) > 0)
            crc.update(buf, 0, n);
        return String.format(Locale.ROOT, "%c%c%c%c-%08X", codeChar(h[0xAC]), codeChar(h[0xAD]), codeChar(h[0xAE]),
                             codeChar(h[0xAF]), crc.getValue());
    }

    private static String n64Key(byte[] h, int hn) {
        if (hn < 0x40)
            return null;
        int w = (h[0] & 0xff) << 24 | (h[1] & 0xff) << 16 | (h[2] & 0xff) << 8 | (h[3] & 0xff);
        int order;
        if (w == 0x80371240) order = 0;          // .z64, big-endian
        else if (w == 0x37804012) order = 1;     // .v64, 16-bit words swapped
        else if (w == 0x40123780) order = 2;     // .n64, 32-bit words reversed
        else return null;
        byte[] b = new byte[0x40];
        for (int i = 0; i < 0x40; i++) {
            int base = i & ~3, k = i & 3;
            b[i] = h[base + (order == 0 ? k : order == 1 ? (k ^ 1) : 3 - k)];
        }
        return String.format(Locale.ROOT, "%c%c%c%c-%02X%02X%02X%02X", codeChar(b[0x3B]), codeChar(b[0x3C]),
                             codeChar(b[0x3D]), codeChar(b[0x3E]), b[0x10] & 0xff, b[0x11] & 0xff, b[0x12] & 0xff,
                             b[0x13] & 0xff);
    }

    private static long be32(byte[] b, int o) {
        return (b[o] & 0xffL) << 24 | (b[o + 1] & 0xffL) << 16 | (b[o + 2] & 0xffL) << 8 | (b[o + 3] & 0xffL);
    }

    /** A GameCube DOL's key, recomp_pack.c's dol_key: the header checked field by field, then the file's CRC32. */
    private static String dolKey(Rd r) throws IOException {
        long size = r.length();
        if (size < 0x100 || size > 0x1800000)
            return null;
        byte[] h = new byte[0x100];
        r.seek(0);
        r.readFully(h);
        long entry = be32(h, 0xE0), bss = be32(h, 0xD8), bssLen = be32(h, 0xDC);
        for (int i = 0xE4; i < 0x100; i++)
            if (h[i] != 0)
                return null;
        boolean text = false, entryIn = false;
        for (int i = 0; i < 18; i++) {
            long off = be32(h, 4 * i), addr = be32(h, 0x48 + 4 * i), len = be32(h, 0x90 + 4 * i);
            if (len == 0)
                continue;
            if (off < 0x100 || off + len > size || addr < 0x80000000L || addr + len > 0x81800000L)
                return null;
            if (i < 7) {
                text = true;
                entryIn |= entry >= addr && entry < addr + len;
            }
        }
        if (!text || !entryIn || (bssLen != 0 && (bss < 0x80000000L || bss + bssLen > 0x81800000L)))
            return null;
        CRC32 crc = new CRC32();
        byte[] buf = new byte[65536];
        r.seek(0);
        int n;
        while ((n = r.read(buf)) > 0)
            crc.update(buf, 0, n);
        return String.format(Locale.ROOT, "DOL-%08X", crc.getValue());
    }

    private static final int GC_MAGIC = 0xC2339F3D;

    private static int gcBe32(byte[] b, int o) {
        return (b[o] & 0xff) << 24 | (b[o + 1] & 0xff) << 16 | (b[o + 2] & 0xff) << 8 | (b[o + 3] & 0xff);
    }

    private static long le64(byte[] b, int o) {
        long v = 0;
        for (int i = 7; i >= 0; i--)
            v = v << 8 | (b[o + i] & 0xffL);
        return v;
    }

    /**
     * The first 0x20 bytes of the GameCube disc in an image whose first bytes are h (consoles/gc/runtime/gc_disc.c):
     * RVZ / WIA (their header keeps the disc's first 0x80 bytes at 0x58), CISO (block 0 after the 0x8000-byte header
     * when its map has it), GCZ (block 0, inflated unless stored), else the image itself (ISO / GCM). Null when it
     * carries no GameCube magic.
     */
    private static byte[] gcHeader(Rd r, byte[] h, int hn) throws IOException {
        byte[] d = null;
        if (hn >= 0x78 && (h[0] == 'R' && h[1] == 'V' && h[2] == 'Z' || h[0] == 'W' && h[1] == 'I' && h[2] == 'A')
            && h[3] == 1) {
            d = java.util.Arrays.copyOfRange(h, 0x58, 0x78);
        } else if (hn >= 12 && h[0] == 'C' && h[1] == 'I' && h[2] == 'S' && h[3] == 'O') {
            if (h[8] == 0 || r.length() < 0x8020)
                return null;
            d = new byte[0x20];
            r.seek(0x8000);
            r.readFully(d);
        } else if (hn >= 32 && gcBe32(h, 0) == 0x01C00BB1) {   // GCZ: 0xB10BC001 little-endian
            d = gczBlock0(r, h);
        } else if (hn >= 0x20) {
            d = java.util.Arrays.copyOf(h, 0x20);
        }
        return d != null && d.length >= 0x20 && gcBe32(d, 0x1C) == GC_MAGIC ? d : null;
    }

    /** A GCZ's block 0 (header: compressed size at 8, block size at 24, block count at 28, little-endian). */
    private static byte[] gczBlock0(Rd r, byte[] h) throws IOException {
        long comp = le64(h, 8);
        long bn = le64(h, 24);
        long block = bn & 0xffffffffL, n = bn >>> 32;
        if (n < 1 || n > (1 << 24) || block < 0x20 || block > (64 << 20))
            return null;
        byte[] ptrs = new byte[16];
        r.seek(32);
        r.readFully(ptrs, 0, n > 1 ? 16 : 8);
        long p0 = le64(ptrs, 0), end = n > 1 ? le64(ptrs, 8) & Long.MAX_VALUE : comp;
        long at = 32L + 12L * n + (p0 & Long.MAX_VALUE), len = end - (p0 & Long.MAX_VALUE);
        if (len <= 0 || len > block + 0x1000L || at + len > r.length())
            return null;
        byte[] raw = new byte[(int)len];
        r.seek(at);
        r.readFully(raw);
        if (p0 < 0)   // top bit: stored as is
            return raw;
        java.util.zip.Inflater inf = new java.util.zip.Inflater();
        try {
            inf.setInput(raw);
            byte[] out = new byte[0x20];
            int got = inf.inflate(out);
            return got == out.length ? out : null;
        } catch (java.util.zip.DataFormatException e) {
            return null;
        } finally {
            inf.end();
        }
    }

    /** The GameCube title key of a disc header's first 0x20 bytes (recomp_pack.c gc_header_key). */
    static String gcKey(byte[] d) {
        StringBuilder k = new StringBuilder();
        for (int i = 0; i < 6; i++)
            k.append(codeChar(d[i]));
        k.append('-').append(d[7] & 0xff);
        if (d[6] != 0)
            k.append("-D").append((d[6] & 0xff) + 1);
        return k.toString();
    }

    /** A .cue's first FILE, next to the .cue; null when it names none. */
    static File cueBin(File cue) {
        StringBuilder text = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(cue))) {
            String line;
            while ((line = r.readLine()) != null && text.length() < MAX_CUE)
                text.append(line).append('\n');
        } catch (IOException e) {
            return null;
        }
        List<String> names = cueFiles(text.toString());
        if (names.isEmpty())
            return null;
        File f = new File(names.get(0));
        return f.isAbsolute() ? f : new File(cue.getParentFile(), names.get(0));
    }

    /** The most of a .cue read (real ones: a few hundred bytes). */
    static final int MAX_CUE = 64 << 10;

    /**
     * Every FILE a .cue names, in order (a multi-track disc names one per track), as written (relative to the .cue);
     * reading stops at the first malformed FILE line.
     */
    static List<String> cueFiles(String cue) {
        List<String> out = new ArrayList<>();
        for (String line : cue.split("\\r?\\n|\\r")) {
            String t = line.trim();
            if (t.length() < 5 || !t.substring(0, 4).equalsIgnoreCase("FILE") || !Character.isWhitespace(t.charAt(4)))
                continue;
            t = t.substring(5).trim();
            String name;
            if (t.startsWith("\"")) {
                int end = t.indexOf('"', 1);
                name = end > 1 ? t.substring(1, end) : null;
            } else {
                String[] parts = t.split("\\s+");
                name = parts.length > 0 && !parts[0].isEmpty() ? parts[0] : null;
            }
            if (name == null)
                break;
            out.add(name);
        }
        return out;
    }

    // ---- Reading ------------------------------------------------------------------------------------------------

    /** Seekable content read at a position: a file, a document's descriptor, a test's bytes. */
    interface Source {
        long length() throws IOException;

        /** Up to len bytes at pos into b[off..]; -1 at the end. */
        int read(long pos, byte[] b, int off, int len) throws IOException;

        /**
         * The same content for native code ({@link #chdReader}): a file path, or « fd:N » for a descriptor its owner
         * keeps open; null when there is none (bytes in memory).
         */
        default String nativePath() {
            return null;
        }
    }

    /** A FileChannel as a Source (a RandomAccessFile's, or a FileInputStream's over a document's descriptor). */
    static Source source(FileChannel ch) {
        return source(ch, null);
    }

    /** {@link #source(FileChannel)} that native code can also open at nativePath (a path or « fd:N »). */
    static Source source(FileChannel ch, String nativePath) {
        return new Source() {
            @Override
            public String nativePath() {
                return nativePath;
            }

            @Override
            public long length() throws IOException {
                return ch.size();
            }

            @Override
            public int read(long pos, byte[] b, int off, int len) throws IOException {
                return ch.read(ByteBuffer.wrap(b, off, len), pos);
            }
        };
    }

    /** Bytes in memory as a Source. */
    static Source source(byte[] data) {
        return new Source() {
            @Override
            public long length() {
                return data.length;
            }

            @Override
            public int read(long pos, byte[] b, int off, int len) {
                if (pos >= data.length)
                    return -1;
                int n = (int)Math.min(len, data.length - pos);
                System.arraycopy(data, (int)pos, b, off, n);
                return n;
            }
        };
    }

    /** RandomAccessFile's seek / read / readFully over a Source. */
    private static final class Rd {
        final Source s;
        long pos;

        Rd(Source s) {
            this.s = s;
        }

        long length() throws IOException {
            return s.length();
        }

        void seek(long p) {
            pos = p;
        }

        int read(byte[] b) throws IOException {
            int n = s.read(pos, b, 0, b.length);
            if (n > 0)
                pos += n;
            return n;
        }

        void readFully(byte[] b) throws IOException {
            readFully(b, 0, b.length);
        }

        void readFully(byte[] b, int off, int len) throws IOException {
            while (len > 0) {
                int n = s.read(pos, b, off, len);
                if (n <= 0)
                    throw new java.io.EOFException();
                pos += n;
                off += n;
                len -= n;
            }
        }
    }

    /** An ISO 9660 data track: 2048-byte sectors, or raw 2352-byte ones (mode 1 data at 16, mode 2 form 1 at 24). */
    private static final class Iso {
        final Rd r;
        final int size, off;
        final byte[] pvd = new byte[2048];

        private Iso(Rd r, int size, int off) {
            this.r = r;
            this.size = size;
            this.off = off;
        }

        boolean read(long lba, byte[] out) throws IOException {
            long pos = lba * size + off;
            if (pos + 2048 > r.length())
                return false;
            r.seek(pos);
            r.readFully(out, 0, 2048);
            return true;
        }

        boolean isPvd() {
            return pvd[0] == 1 && pvd[1] == 'C' && pvd[2] == 'D' && pvd[3] == '0' && pvd[4] == '0' && pvd[5] == '1';
        }

        static Iso open(Rd r) throws IOException {
            Iso a = new Iso(r, 2048, 0);
            if (a.read(16, a.pvd) && a.isPvd())
                return a;
            long at = 16L * 2352;
            if (at + 16 > r.length())
                return null;
            byte[] head = new byte[16];
            r.seek(at);
            r.readFully(head);
            if (head[0] != 0 || head[11] != 0)
                return null;
            for (int i = 1; i <= 10; i++)
                if ((head[i] & 0xff) != 0xff)
                    return null;
            Iso b = new Iso(r, 2352, head[15] == 2 ? 24 : 16);
            return b.read(16, b.pvd) && b.isPvd() ? b : null;
        }

        boolean playstation() {
            return new String(pvd, 8, 11, StandardCharsets.US_ASCII).equals("PLAYSTATION");
        }

        private static long le32(byte[] b, int o) {
            return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
        }

        /** SYSTEM.CNF of the root directory, or null. */
        String systemCnf() throws IOException {
            long lba = le32(pvd, 156 + 2), len = le32(pvd, 156 + 10);
            byte[] sec = new byte[2048];
            for (long done = 0; done < len && done < 64 * 2048; done += 2048, lba++) {
                if (!read(lba, sec))
                    return null;
                for (int o = 0; o + 33 < 2048 && sec[o] != 0;) {
                    int rl = sec[o] & 0xff, nl = sec[o + 32] & 0xff;
                    if (rl < 34 || o + rl > 2048)
                        break;
                    if (nl >= 10 && o + 33 + nl <= 2048) {
                        String n = new String(sec, o + 33, nl, StandardCharsets.US_ASCII).toUpperCase(Locale.ROOT);
                        if (n.equals("SYSTEM.CNF") || n.startsWith("SYSTEM.CNF;")) {
                            long flba = le32(sec, o + 2), flen = Math.min(le32(sec, o + 10), 2048);
                            if (!read(flba, sec))
                                return null;
                            return new String(sec, 0, (int)flen, StandardCharsets.ISO_8859_1);
                        }
                    }
                    o += rl;
                }
            }
            return null;
        }

        String ps1Key() throws IOException {
            String cnf = systemCnf();
            String serial = cnf == null ? null : ps1Serial(cnf);
            if (serial != null)
                return serial;
            CRC32 crc = new CRC32();
            crc.update(pvd);
            return String.format(Locale.ROOT, "PSX-%08X", crc.getValue());
        }
    }

    private static final java.util.regex.Pattern BOOT =
        java.util.regex.Pattern.compile("BOOT[ \\t]*=[ \\t]*([^\\r\\n;]*)");
    private static final java.util.regex.Pattern SERIAL =
        java.util.regex.Pattern.compile("([A-Z]{4})[_-](\\d{3})\\.(\\d{2})");

    /** « BOOT = cdrom:\SLUS_005.94;1 » -> « SLUS-00594 »; null when the boot file is not serial-shaped. */
    static String ps1Serial(String cnf) {
        java.util.regex.Matcher m = BOOT.matcher(cnf);
        if (!m.find())
            return null;
        String v = m.group(1).trim();
        int cut = Math.max(v.lastIndexOf('\\'), Math.max(v.lastIndexOf(':'), v.lastIndexOf('/')));
        String name = v.substring(cut + 1).trim().toUpperCase(Locale.ROOT);
        java.util.regex.Matcher s = SERIAL.matcher(name);
        return s.matches() ? s.group(1) + "-" + s.group(2) + s.group(3) : null;
    }
}
