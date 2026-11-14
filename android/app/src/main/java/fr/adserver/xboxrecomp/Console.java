package fr.adserver.xboxrecomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.zip.CRC32;

/**
 * The consoles XRecomp runs (XBA-147; Plane « Spec · Other consoles », consoles/README.md): Xbox, and Game Boy Advance,
 * PlayStation and Nintendo 64 statically recompiled the same way. pack.json's "console" says which one a title pack is
 * for (absent: xbox, every pack made before the field); the native loader (pack_loader.c) hands a pack to that
 * console's runtime.
 *
 * Mirrors src/platform/recomp_pack.c: the import filter (file names the app takes as games of each console), and
 * {@link #identify}, the console and generic title key read from a game file's content, which names its pack folder
 * (packs/&lt;console&gt;-&lt;key&gt;; an Xbox pack stays packs/&lt;TITLEID&gt;).
 */
enum Console {
    XBOX("xbox"), GBA("gba"), PS1("ps1"), N64("n64");

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

    // ---- Import filter ------------------------------------------------------------------------------------------

    /**
     * The console a game file's name says, for the import filter: .gba; PS1 .bin, .cue, .chd; N64 .z64, .n64, .v64;
     * the Xbox images (DiscImages.isImageName: .xiso .cso .ciso .zso .cci and split parts). Null for anything else,
     * and for a plain .iso, which both an Xbox and a PS1 disc use: {@link #detect} reads its content.
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
        return byName(f.getName());
    }

    /** Where an Xbox image has its XDVDFS volume: XISO (0), redump full images (video partition first). */
    private static final long[] XISO_OFFSETS = { 0L, 0x18300000L, 0xFD90000L, 0x2080000L };
    private static final byte[] XISO_MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /**
     * An .iso's console by content: XBOX for the Xbox volume magic "MICROSOFT*XBOX*MEDIA" at sector 32 of the image or
     * of its game partition (checked first: a redump Xbox image also starts with an ISO 9660 video partition), PS1 for
     * an ISO 9660 volume whose system ID is "PLAYSTATION" (2048- or 2352-byte sectors), else null.
     */
    static Console sniffIso(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
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
            return iso != null && iso.playstation() ? PS1 : null;
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
     * </ul>
     * A game code byte outside [A-Z0-9] becomes '_'. Null for anything else: an Xbox game, and CHD for now (compressed:
     * read once a CHD reader is in).
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
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            byte[] h = new byte[0xC0];
            int hn = r.read(h);
            String k;
            if ((k = n64Key(h, hn)) != null)
                return new Identity(N64, k);
            if ((k = gbaKey(r, h, hn)) != null)
                return new Identity(GBA, k);
            Iso iso = Iso.open(r);
            if (iso != null && iso.playstation())
                return new Identity(PS1, iso.ps1Key());
            return null;
        } catch (IOException e) {
            return null;
        }
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

    private static String gbaKey(RandomAccessFile r, byte[] h, int hn) throws IOException {
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

    /** A .cue's first FILE, next to the .cue; null when it names none. */
    static File cueBin(File cue) {
        try (BufferedReader r = new BufferedReader(new FileReader(cue))) {
            String line;
            while ((line = r.readLine()) != null) {
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
                    return null;
                File f = new File(name);
                return f.isAbsolute() ? f : new File(cue.getParentFile(), name);
            }
        } catch (IOException e) {
            return null;
        }
        return null;
    }

    /** An ISO 9660 data track: 2048-byte sectors, or raw 2352-byte ones (mode 1 data at 16, mode 2 form 1 at 24). */
    private static final class Iso {
        final RandomAccessFile r;
        final int size, off;
        final byte[] pvd = new byte[2048];

        private Iso(RandomAccessFile r, int size, int off) {
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

        static Iso open(RandomAccessFile r) throws IOException {
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
