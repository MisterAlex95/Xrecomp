package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * What a PS1 Prepare reads of a disc that is a document of a library folder (XBA-168): tools.prepare (Python, its own
 * process) opens files by path, and a document has none; the disc is hundreds of MB, of which the Prepare needs the
 * boot EXE (2 MB at most) and the few sectors that lead to it. This copies exactly those sectors, at their place, into
 * a cooked (2048-byte sectors) ISO image in the work folder: the volume descriptor (sector 16), the root directory,
 * SYSTEM.CNF, every directory on the boot EXE's path and the EXE itself. The rest of the file is left as a hole (a
 * sparse file: a few hundred KB on disk however large the disc). tools.prepare then reads that image as any .iso, with
 * the same rules as consoles/ps1/tools/ps1_tools/exe.py (SYSTEM.CNF's first BOOT = line, « cdrom: » dropped; without
 * one PSX.EXE; names case-insensitive, « ;1 » ignored), so the title key and the boot EXE it finds are the disc's.
 * docs/android/prepare.md « PS1 packs ».
 */
final class Ps1Disc {
    private Ps1Disc() {
    }

    /** The boot EXE read at most (ps1_runtime.c PS1_EXE_MAX: the header and the whole RAM). */
    static final long EXE_MAX = 2L * 1024 * 1024 + 0x800;
    /** A directory read at most. */
    static final long DIR_MAX = 1L << 20;
    /** SYSTEM.CNF read at most. */
    static final long CNF_MAX = 64L << 10;

    private static final byte[] SYNC = { 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0 };
    private static final Pattern BOOT = Pattern.compile("BOOT\\s*=\\s*(\\S+)", Pattern.CASE_INSENSITIVE);
    private static final Pattern CDROM = Pattern.compile("^cdrom:?", Pattern.CASE_INSENSITIVE);

    /** What extract copied: the boot EXE's path on the disc, its size, the sectors written. */
    static final class Result {
        String boot = "";
        long exeSize;
        int sectors;
    }

    /** A data track: cooked 2048-byte sectors, or raw 2352-byte ones (mode 2 form 1 data at 24, mode 1 at 16). */
    private static final class Track {
        final Console.Source src;
        final boolean raw;

        Track(Console.Source src) throws IOException {
            this.src = src;
            byte[] pvd = new byte[2048];
            boolean cooked = readAt(src, 16L * 2048, pvd) && isPvd(pvd);
            boolean r = false;
            if (!cooked) {
                byte[] head = new byte[12];
                r = readAt(src, 16L * 2352, head) && java.util.Arrays.equals(head, SYNC);
            }
            if (!cooked && !r)
                throw new IOException("no ISO 9660 volume (not a PlayStation data track)");
            raw = r;
        }

        /** The 2048 user bytes of sector lba. */
        byte[] sector(long lba) throws IOException {
            byte[] out = new byte[2048];
            if (!raw) {
                if (!readAt(src, lba * 2048, out))
                    throw new IOException("sector " + lba + " past the end of the disc");
                return out;
            }
            byte[] s = new byte[2352];
            if (!readAt(src, lba * 2352, s))
                throw new IOException("sector " + lba + " past the end of the disc");
            System.arraycopy(s, s[15] == 2 ? 24 : 16, out, 0, 2048);
            return out;
        }
    }

    private static boolean isPvd(byte[] p) {
        return p[0] == 1 && p[1] == 'C' && p[2] == 'D' && p[3] == '0' && p[4] == '0' && p[5] == '1';
    }

    private static boolean readAt(Console.Source s, long pos, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int n = s.read(pos + off, b, off, b.length - off);
            if (n <= 0)
                return false;
            off += n;
        }
        return true;
    }

    private static long le32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }

    /** The copy being made: each sector read is written at its place. */
    private static final class Copy {
        final Track t;
        final RandomAccessFile out;
        long last = -1;
        int sectors;

        Copy(Track t, RandomAccessFile out) {
            this.t = t;
            this.out = out;
        }

        /** size bytes from lba (max at most), copied, returned. */
        byte[] extent(long lba, long size, long max, String what) throws IOException {
            if (size > max)
                throw new IOException(what + " is " + size + " bytes, over " + max);
            byte[] data = new byte[(int)size];
            for (long done = 0; done < size; done += 2048, lba++) {
                byte[] s = t.sector(lba);
                out.seek(lba * 2048);
                out.write(s);
                sectors++;
                last = Math.max(last, lba);
                System.arraycopy(s, 0, data, (int)done, (int)Math.min(2048, size - done));
            }
            return data;
        }
    }

    /** (extent, size) of the entry want (upper case, no « ;1 ») in the directory at (lba, size), or null. */
    private static long[] dirFind(Copy c, long lba, long size, String want, boolean dir) throws IOException {
        byte[] d = c.extent(lba, size, DIR_MAX, "a directory");
        int pos = 0;
        while (pos < d.length) {
            int n = d[pos] & 0xff;
            if (n == 0) {
                pos = (pos / 2048 + 1) * 2048;   // records never cross a sector
                continue;
            }
            if (n < 33 || pos + n > d.length)
                break;
            int nl = d[pos + 32] & 0xff;
            if (33 + nl > n)
                break;
            String name = new String(d, pos + 33, nl, StandardCharsets.ISO_8859_1);
            boolean isDir = (d[pos + 25] & 2) != 0;
            if (!name.equals("\0") && !name.equals("\1")) {
                int semi = name.indexOf(';');
                String base = (semi >= 0 ? name.substring(0, semi) : name).toUpperCase(Locale.ROOT);
                if (base.equals(want) && isDir == dir)
                    return new long[] { le32(d, pos + 2), le32(d, pos + 10) };
            }
            pos += n;
        }
        return null;
    }

    /** A file by path (« \DIR\NAME.EXT;1 », '\' or '/') from the root (rootLba, rootSize): (extent, size) or null. */
    private static long[] find(Copy c, long rootLba, long rootSize, String path) throws IOException {
        List<String> parts = new ArrayList<>();
        for (String p : path.split("[\\\\/]+"))
            if (!p.isEmpty())
                parts.add(p);
        long lba = rootLba, size = rootSize;
        for (int i = 0; i < parts.size(); i++) {
            String p = parts.get(i);
            int semi = p.indexOf(';');
            long[] hit = dirFind(c, lba, size, (semi >= 0 ? p.substring(0, semi) : p).toUpperCase(Locale.ROOT),
                                 i < parts.size() - 1);
            if (hit == null)
                return null;
            lba = hit[0];
            size = hit[1];
        }
        return parts.isEmpty() ? null : new long[] { lba, size };
    }

    /** The boot file SYSTEM.CNF names (« cdrom: » dropped), or null. */
    static String bootPath(String cnf) {
        Matcher m = BOOT.matcher(cnf);
        return m.find() ? CDROM.matcher(m.group(1)).replaceFirst("") : null;
    }

    /**
     * Copies the sectors that lead to src's boot EXE into out (a cooked, sparse ISO image; replaced). IOException
     * when src is not a PS1 data track or its boot EXE cannot be found or read.
     */
    static Result extract(Console.Source src, File out) throws IOException {
        Track t = new Track(src);
        File tmp = new File(out.getPath() + ".tmp");
        //noinspection ResultOfMethodCallIgnored
        tmp.delete();
        Result r = new Result();
        try (RandomAccessFile raf = new RandomAccessFile(tmp, "rw")) {
            Copy c = new Copy(t, raf);
            byte[] pvd = c.extent(16, 2048, 2048, "the volume descriptor");
            if (!isPvd(pvd))
                throw new IOException("no ISO 9660 volume descriptor");
            long rootLba = le32(pvd, 156 + 2), rootSize = le32(pvd, 156 + 10);
            String boot = "PSX.EXE";
            long[] cnf = find(c, rootLba, rootSize, "SYSTEM.CNF");
            if (cnf != null) {
                String text = new String(c.extent(cnf[0], cnf[1], CNF_MAX, "SYSTEM.CNF"), StandardCharsets.ISO_8859_1);
                String b = bootPath(text);
                if (b != null)
                    boot = b;
            }
            long[] exe = find(c, rootLba, rootSize, boot);
            if (exe == null)
                throw new IOException("the boot EXE " + boot + " is not on the disc");
            c.extent(exe[0], exe[1], EXE_MAX, "the boot EXE " + boot);
            raf.setLength((c.last + 1) * 2048);
            r.boot = boot;
            r.exeSize = exe[1];
            r.sectors = c.sectors;
        } catch (IOException | RuntimeException e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw e instanceof IOException ? (IOException)e : new IOException(e);
        }
        if (!tmp.renameTo(out))
            throw new IOException("cannot write " + out);
        return r;
    }
}
