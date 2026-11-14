package fr.adserver.xrecomp;

import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.function.Predicate;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * Copies an APK (a zip) entry by entry without recompressing it: entries are dropped, replaced or copied raw (their
 * compressed bytes as they are, which keeps a 300 MB APK's copy at disk speed). Used by TitleApk (docs/android/
 * app-shell.md « A game's own app ») before apksig signs the result.
 *
 * Stored (uncompressed) entries are aligned as zipalign does: their data starts at a multiple of 4, or of 16384 for a
 * stored .so (16 KB pages; this app's libraries are compressed, useLegacyPackaging), with the padding in an « Android
 * ZIP alignment » extra field (0xd935, as apksig writes it). Local headers carry the sizes and CRC (no data
 * descriptor); central directory extras are dropped. Zip64 is refused (an APK past 4 GB is not one Android installs
 * from this app anyway).
 */
final class ApkZip {
    private ApkZip() {}

    static final int LOCAL_SIG = 0x04034b50, CENTRAL_SIG = 0x02014b50, EOCD_SIG = 0x06054b50;
    static final int STORED = 0, DEFLATED = 8;
    static final int ALIGN = 4, SO_ALIGN = 16384;
    static final int ALIGNMENT_EXTRA_ID = 0xd935;

    /** One central directory record. */
    static final class Entry {
        String name;
        int versionMadeBy, versionNeeded, flags, method, time, date, internalAttr;
        long crc, compressedSize, size, externalAttr, localOffset;
        byte[] comment = new byte[0];

        /** The offset of the entry's data in the file it was read from (after its local header). */
        long dataOffset;
    }

    /** Progress of a copy: bytes written so far out of the input's size. */
    interface Progress {
        void update(long done, long total);
    }

    /** The entries of a zip, in central directory order, with their data offsets. */
    static List<Entry> entries(RandomAccessFile f) throws IOException {
        long len = f.length();
        int tail = (int)Math.min(len, 22 + 0xffff);
        byte[] t = new byte[tail];
        f.seek(len - tail);
        f.readFully(t);
        ByteBuffer bb = ByteBuffer.wrap(t).order(ByteOrder.LITTLE_ENDIAN);
        int eocd = -1;
        for (int i = tail - 22; i >= 0; i--) {
            if (bb.getInt(i) == EOCD_SIG && i + 22 + (bb.getShort(i + 20) & 0xffff) == tail) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0)
            throw new IOException("not a zip file (no end of central directory)");
        int count = bb.getShort(eocd + 10) & 0xffff;
        long cdSize = bb.getInt(eocd + 12) & 0xffffffffL, cdOffset = bb.getInt(eocd + 16) & 0xffffffffL;
        if (count == 0xffff || cdSize == 0xffffffffL || cdOffset == 0xffffffffL)
            throw new IOException("zip64 is not supported");
        if (cdOffset + cdSize > len)
            throw new IOException("bad central directory");
        byte[] cd = new byte[(int)cdSize];
        f.seek(cdOffset);
        f.readFully(cd);
        ByteBuffer c = ByteBuffer.wrap(cd).order(ByteOrder.LITTLE_ENDIAN);
        List<Entry> list = new ArrayList<>(count);
        int p = 0;
        byte[] lh = new byte[30];
        ByteBuffer l = ByteBuffer.wrap(lh).order(ByteOrder.LITTLE_ENDIAN);
        for (int i = 0; i < count; i++) {
            if (p + 46 > cd.length || c.getInt(p) != CENTRAL_SIG)
                throw new IOException("bad central directory record " + i);
            Entry e = new Entry();
            e.versionMadeBy = c.getShort(p + 4) & 0xffff;
            e.versionNeeded = c.getShort(p + 6) & 0xffff;
            e.flags = c.getShort(p + 8) & 0xffff;
            e.method = c.getShort(p + 10) & 0xffff;
            e.time = c.getShort(p + 12) & 0xffff;
            e.date = c.getShort(p + 14) & 0xffff;
            e.crc = c.getInt(p + 16) & 0xffffffffL;
            e.compressedSize = c.getInt(p + 20) & 0xffffffffL;
            e.size = c.getInt(p + 24) & 0xffffffffL;
            int nameLen = c.getShort(p + 28) & 0xffff, extraLen = c.getShort(p + 30) & 0xffff;
            int commentLen = c.getShort(p + 32) & 0xffff;
            e.internalAttr = c.getShort(p + 36) & 0xffff;
            e.externalAttr = c.getInt(p + 38) & 0xffffffffL;
            e.localOffset = c.getInt(p + 42) & 0xffffffffL;
            if (e.compressedSize == 0xffffffffL || e.size == 0xffffffffL || e.localOffset == 0xffffffffL)
                throw new IOException("zip64 is not supported");
            e.name = new String(cd, p + 46, nameLen, StandardCharsets.UTF_8);
            e.comment = java.util.Arrays.copyOfRange(cd, p + 46 + nameLen + extraLen,
                                                     p + 46 + nameLen + extraLen + commentLen);
            p += 46 + nameLen + extraLen + commentLen;
            f.seek(e.localOffset);
            f.readFully(lh);
            if (l.getInt(0) != LOCAL_SIG)
                throw new IOException("bad local header of " + e.name);
            e.dataOffset = e.localOffset + 30 + (l.getShort(26) & 0xffff) + (l.getShort(28) & 0xffff);
            if (e.dataOffset + e.compressedSize > len)
                throw new IOException(e.name + ": data past the end of the file");
            list.add(e);
        }
        return list;
    }

    /** The data alignment a stored entry needs (0: a compressed one, no alignment). */
    static int alignment(String name, int method) {
        if (method != STORED)
            return 0;
        return name.endsWith(".so") ? SO_ALIGN : ALIGN;
    }

    /**
     * Copies in to out: entries for which drop is true are left out; an entry named in replace gets those bytes
     * (compressed like the original was, deflated when it was compressed), every other one its raw bytes. Entries of
     * replace that in does not have are not added. Throws InterruptedIOException when cancelled says so.
     */
    static void rewrite(File in, File out, Map<String, byte[]> replace, Predicate<String> drop, Progress progress,
                        java.util.function.BooleanSupplier cancelled) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(in, "r");
             OutputStream os = new BufferedOutputStream(new FileOutputStream(out), 1 << 16)) {
            List<Entry> entries = entries(f);
            long total = f.length();
            CountingOut o = new CountingOut(os);
            List<Entry> written = new ArrayList<>();
            byte[] buf = new byte[1 << 16];
            for (Entry e : entries) {
                if (cancelled != null && cancelled.getAsBoolean())
                    throw new InterruptedIOException("cancelled");
                if (drop != null && drop.test(e.name))
                    continue;
                Entry w = copyHeader(e);
                byte[] data = null;
                if (replace != null && replace.containsKey(e.name)) {
                    byte[] plain = replace.get(e.name);
                    CRC32 crc = new CRC32();
                    crc.update(plain);
                    w.crc = crc.getValue();
                    w.size = plain.length;
                    data = w.method == STORED ? plain : deflate(plain);
                    if (w.method != STORED)
                        w.method = DEFLATED;
                    w.compressedSize = data.length;
                }
                w.flags &= ~0x0008;                         // sizes and CRC are in the local header
                w.localOffset = o.count;
                byte[] name = w.name.getBytes(StandardCharsets.UTF_8);
                int align = alignment(w.name, w.method);
                byte[] extra = new byte[0];
                if (align > 0) {
                    long dataAt = o.count + 30 + name.length;
                    int pad = (int)((align - (dataAt + 6) % align) % align);
                    extra = new byte[6 + pad];
                    ByteBuffer x = ByteBuffer.wrap(extra).order(ByteOrder.LITTLE_ENDIAN);
                    x.putShort((short)ALIGNMENT_EXTRA_ID).putShort((short)(2 + pad)).putShort((short)align);
                }
                ByteBuffer h = ByteBuffer.allocate(30).order(ByteOrder.LITTLE_ENDIAN);
                h.putInt(LOCAL_SIG).putShort((short)w.versionNeeded).putShort((short)w.flags)
                 .putShort((short)w.method).putShort((short)w.time).putShort((short)w.date).putInt((int)w.crc)
                 .putInt((int)w.compressedSize).putInt((int)w.size).putShort((short)name.length)
                 .putShort((short)extra.length);
                o.write(h.array());
                o.write(name);
                o.write(extra);
                if (data != null) {
                    o.write(data);
                } else {
                    f.seek(e.dataOffset);
                    long left = e.compressedSize;
                    while (left > 0) {
                        int n = f.read(buf, 0, (int)Math.min(buf.length, left));
                        if (n < 0)
                            throw new IOException(e.name + ": short read");
                        o.write(buf, 0, n);
                        left -= n;
                    }
                }
                written.add(w);
                if (progress != null)
                    progress.update(Math.min(o.count, total), total);
            }
            long cdStart = o.count;
            for (Entry w : written) {
                byte[] name = w.name.getBytes(StandardCharsets.UTF_8);
                ByteBuffer c = ByteBuffer.allocate(46).order(ByteOrder.LITTLE_ENDIAN);
                c.putInt(CENTRAL_SIG).putShort((short)w.versionMadeBy).putShort((short)w.versionNeeded)
                 .putShort((short)w.flags).putShort((short)w.method).putShort((short)w.time)
                 .putShort((short)w.date).putInt((int)w.crc).putInt((int)w.compressedSize).putInt((int)w.size)
                 .putShort((short)name.length).putShort((short)0).putShort((short)w.comment.length)
                 .putShort((short)0).putShort((short)w.internalAttr).putInt((int)w.externalAttr)
                 .putInt((int)w.localOffset);
                o.write(c.array());
                o.write(name);
                o.write(w.comment);
            }
            long cdSize = o.count - cdStart;
            if (o.count > 0xffffffffL || written.size() >= 0xffff)
                throw new IOException("the copy needs zip64");
            ByteBuffer end = ByteBuffer.allocate(22).order(ByteOrder.LITTLE_ENDIAN);
            end.putInt(EOCD_SIG).putShort((short)0).putShort((short)0).putShort((short)written.size())
               .putShort((short)written.size()).putInt((int)cdSize).putInt((int)cdStart).putShort((short)0);
            o.write(end.array());
        }
    }

    private static Entry copyHeader(Entry e) {
        Entry w = new Entry();
        w.name = e.name;
        w.versionMadeBy = e.versionMadeBy;
        w.versionNeeded = e.versionNeeded;
        w.flags = e.flags;
        w.method = e.method;
        w.time = e.time;
        w.date = e.date;
        w.crc = e.crc;
        w.compressedSize = e.compressedSize;
        w.size = e.size;
        w.internalAttr = e.internalAttr;
        w.externalAttr = e.externalAttr;
        w.comment = e.comment;
        return w;
    }

    static byte[] deflate(byte[] plain) {
        Deflater d = new Deflater(9, true);
        try {
            d.setInput(plain);
            d.finish();
            ByteArrayOutputStream o = new ByteArrayOutputStream(plain.length / 2 + 64);
            byte[] b = new byte[1 << 14];
            while (!d.finished()) {
                int n = d.deflate(b);
                o.write(b, 0, n);
            }
            return o.toByteArray();
        } finally {
            d.end();
        }
    }

    /** An entry's uncompressed bytes (stored or deflated). */
    static byte[] read(RandomAccessFile f, Entry e) throws IOException {
        byte[] raw = new byte[(int)e.compressedSize];
        f.seek(e.dataOffset);
        f.readFully(raw);
        if (e.method == STORED)
            return raw;
        if (e.method != DEFLATED)
            throw new IOException(e.name + ": compression method " + e.method);
        Inflater inf = new Inflater(true);
        try {
            inf.setInput(raw);
            byte[] out = new byte[(int)e.size];
            int n = 0;
            while (n < out.length) {
                int k = inf.inflate(out, n, out.length - n);
                if (k == 0 && (inf.finished() || inf.needsInput() || inf.needsDictionary()))
                    break;
                n += k;
            }
            if (n != out.length)
                throw new IOException(e.name + ": inflated " + n + " of " + out.length + " bytes");
            return out;
        } catch (java.util.zip.DataFormatException ex) {
            throw new IOException(e.name + ": " + ex.getMessage(), ex);
        } finally {
            inf.end();
        }
    }

    private static final class CountingOut {
        final OutputStream out;
        long count;

        CountingOut(OutputStream out) {
            this.out = out;
        }

        void write(byte[] b) throws IOException {
            write(b, 0, b.length);
        }

        void write(byte[] b, int off, int len) throws IOException {
            out.write(b, off, len);
            count += len;
        }
    }
}
