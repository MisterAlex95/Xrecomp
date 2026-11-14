package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * What the game library shows of a game, from its default.xbe certificate (public layout, xboxdevwiki « Xbe »;
 * docs/formats/xbe.md). Plain Java, no Android: the JVM tests run it as is.
 *
 * <pre>
 * header:      "XBEH", base address at 0x104, certificate address at 0x118
 * certificate: +0x08 title ID (u32), +0x0C title name (40 UTF-16LE units), +0xA0 game region (bit mask:
 *              1 North America, 2 Japan, 4 rest of the world, 0x80000000 debug), +0xA8 disc number, +0xAC version
 * </pre>
 * A short certificate (an XBE cut after the name, as the launcher's test fixtures are) leaves region, disc and version
 * at 0.
 */
final class XbeInfo {
    static final int REGION_NA = 1, REGION_JAPAN = 2, REGION_WORLD = 4;

    /** 8 upper-case hex digits. */
    final String titleId;
    /** The certificate's title name, trimmed; "" when blank. */
    final String title;
    final int region, disc, version;

    XbeInfo(String titleId, String title, int region, int disc, int version) {
        this.titleId = titleId;
        this.title = title;
        this.region = region;
        this.disc = disc;
        this.version = version;
    }

    /** The certificate of xbe, or null when the file is not an XBE (or cannot be read). */
    static XbeInfo parse(File xbe) {
        if (xbe == null)
            return null;
        try (RandomAccessFile f = new RandomAccessFile(xbe, "r")) {
            long len = f.length();
            if (len < 0x11c)
                return null;
            byte[] hdr = new byte[0x11c];
            f.readFully(hdr);
            if (hdr[0] != 'X' || hdr[1] != 'B' || hdr[2] != 'E' || hdr[3] != 'H')
                return null;
            long base = le32(hdr, 0x104), certAddr = le32(hdr, 0x118);
            if (certAddr < base)
                return null;
            long cert = certAddr - base;
            if (cert + 0x0c + 80 > len)
                return null;
            byte[] c = new byte[(int)Math.min(0xb0, len - cert)];
            f.seek(cert);
            f.readFully(c);
            String tid = String.format(Locale.ROOT, "%08X", le32(c, 0x08));
            int nlen = 0;
            while (nlen < 80 && (c[0x0c + nlen] != 0 || c[0x0c + nlen + 1] != 0))
                nlen += 2;
            String title = new String(c, 0x0c, nlen, StandardCharsets.UTF_16LE).trim();
            int region = c.length >= 0xa4 ? (int)le32(c, 0xa0) : 0;
            int disc = c.length >= 0xac ? (int)le32(c, 0xa8) : 0;
            int version = c.length >= 0xb0 ? (int)le32(c, 0xac) : 0;
            return new XbeInfo(tid, title, region, disc, version);
        } catch (IOException e) {
            return null;
        }
    }

    /** « NA », « JP », « EU » (rest of the world), joined with « / » for a multi-region disc; "" when none is set. */
    static String regionCode(int region) {
        StringBuilder b = new StringBuilder();
        if ((region & REGION_NA) != 0)
            b.append("NA");
        if ((region & REGION_JAPAN) != 0)
            b.append(b.length() > 0 ? "/" : "").append("JP");
        if ((region & REGION_WORLD) != 0)
            b.append(b.length() > 0 ? "/" : "").append("EU");
        return b.toString();
    }

    private static long le32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }
}
