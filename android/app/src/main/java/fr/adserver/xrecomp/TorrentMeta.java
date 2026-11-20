package fr.adserver.xrecomp;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * What a .torrent says (BEP 3, the v1 « files » list; a hybrid v1/v2 torrent is read through its v1 part): the name,
 * the files with their sizes, the private flag and the v1 info hash (SHA-1 of the info dictionary's bytes exactly as
 * they are in the file, never re-encoded). A v2-only torrent (no v1 file list) is refused: libtorrent would number
 * its files differently. Plain Java (the network library, docs « Network library », XBA-319), so the JVM tests read
 * hand-made torrents.
 *
 * Paths are checked here once: a component that is empty, « . », « .. », holds a « / », a « \ » or a NUL, or a path
 * deeper than {@link #MAX_DEPTH}, refuses the whole torrent, so every later File join under a download folder stays
 * inside it. Pad files (BEP 47, attr « p ») are dropped from {@link #files} but keep their place in {@link File#index},
 * the engine's file number.
 */
final class TorrentMeta {
    static final int MAX_DEPTH = 16;
    static final int MAX_FILES = 100_000;

    /** One file of the torrent. */
    static final class File {
        /** Its number in the torrent's file list (pad files counted), as libtorrent numbers it. */
        final int index;
        /** Its path inside the torrent, « / »-separated, below the torrent's name for a multi-file torrent. */
        final String path;
        final long size;

        File(int index, String path, long size) {
            this.index = index;
            this.path = path;
            this.size = size;
        }

        /** The last path component. */
        String name() {
            int s = path.lastIndexOf('/');
            return s < 0 ? path : path.substring(s + 1);
        }

        /** The folder part of the path, "" at the top. */
        String dir() {
            int s = path.lastIndexOf('/');
            return s < 0 ? "" : path.substring(0, s);
        }
    }

    final String name;
    /** The v1 info hash, 40 lower-case hex digits. */
    final String infoHash;
    final boolean isPrivate;
    /** One file (single-file torrent: the path is the name), or the files below the folder name. */
    final boolean multiFile;
    /** The real files, in torrent order (pad files left out). */
    final List<File> files;

    private TorrentMeta(String name, String infoHash, boolean isPrivate, boolean multiFile, List<File> files) {
        this.name = name;
        this.infoHash = infoHash;
        this.isPrivate = isPrivate;
        this.multiFile = multiFile;
        this.files = Collections.unmodifiableList(files);
    }

    /** The file at this torrent path, or null. */
    File file(String path) {
        for (File f : files)
            if (f.path.equals(path))
                return f;
        return null;
    }

    long totalSize() {
        long s = 0;
        for (File f : files)
            s += f.size;
        return s;
    }

    // ---- Parsing ----------------------------------------------------------------------------------------------------

    static TorrentMeta parse(byte[] torrent) throws IOException {
        Bencode b = new Bencode(torrent);
        Object top = b.value();
        if (!(top instanceof Map))
            throw new IOException("not a torrent: no dictionary");
        Map<?, ?> root = (Map<?, ?>)top;
        Object info = root.get("info");
        if (!(info instanceof Map) || b.infoStart < 0)
            throw new IOException("not a torrent: no info dictionary");
        return fromInfo((Map<?, ?>)info, sha1Hex(torrent, b.infoStart, b.infoEnd));
    }

    /** A bare info dictionary (what a magnet's metadata exchange gives). */
    static TorrentMeta parseInfo(byte[] info) throws IOException {
        Bencode b = new Bencode(info);
        Object top = b.value();
        if (!(top instanceof Map))
            throw new IOException("not an info dictionary");
        return fromInfo((Map<?, ?>)top, sha1Hex(info, 0, b.pos));
    }

    private static TorrentMeta fromInfo(Map<?, ?> info, String hash) throws IOException {
        String name = text(info.get("name"));
        if (name == null || !goodComponent(name))
            throw new IOException("bad torrent name");
        boolean priv = Long.valueOf(1).equals(info.get("private"));
        List<File> out = new ArrayList<>();
        Object files = info.get("files");
        if (files instanceof List) {
            List<?> l = (List<?>)files;
            if (l.size() > MAX_FILES)
                throw new IOException("too many files: " + l.size());
            for (int i = 0; i < l.size(); i++) {
                if (!(l.get(i) instanceof Map))
                    throw new IOException("bad file entry " + i);
                Map<?, ?> f = (Map<?, ?>)l.get(i);
                Object len = f.get("length");
                Object path = f.get("path");
                if (!(len instanceof Long) || (Long)len < 0 || !(path instanceof List))
                    throw new IOException("bad file entry " + i);
                String attr = text(f.get("attr"));
                if (attr != null && attr.indexOf('p') >= 0)
                    continue;   // a pad file: keeps its number, never listed
                List<?> parts = (List<?>)path;
                if (parts.isEmpty() || parts.size() > MAX_DEPTH)
                    throw new IOException("bad path in file entry " + i);
                StringBuilder p = new StringBuilder();
                for (Object o : parts) {
                    String c = text(o);
                    if (c == null || !goodComponent(c))
                        throw new IOException("bad path in file entry " + i);
                    if (p.length() > 0)
                        p.append('/');
                    p.append(c);
                }
                out.add(new File(i, p.toString(), (Long)len));
            }
            return new TorrentMeta(name, hash, priv, true, out);
        }
        Object len = info.get("length");
        if (len instanceof Long && (Long)len >= 0) {
            out.add(new File(0, name, (Long)len));
            return new TorrentMeta(name, hash, priv, false, out);
        }
        if (info.get("file tree") != null)
            throw new IOException("a v2-only torrent: make it hybrid (v1 + v2) or v1");
        throw new IOException("not a torrent: no files");
    }

    /** A path component that cannot leave its folder. */
    static boolean goodComponent(String c) {
        if (c.isEmpty() || c.equals(".") || c.equals("..") || c.length() > 255)
            return false;
        for (int i = 0; i < c.length(); i++) {
            char ch = c.charAt(i);
            if (ch == '/' || ch == '\\' || ch == 0)
                return false;
        }
        return true;
    }

    private static String text(Object o) {
        return o instanceof byte[] ? new String((byte[])o, StandardCharsets.UTF_8) : null;
    }

    static String sha1Hex(byte[] b, int from, int to) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-1");
            md.update(b, from, to - from);
            StringBuilder s = new StringBuilder(40);
            for (byte x : md.digest())
                s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
            return s.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    // ---- Magnet links -----------------------------------------------------------------------------------------------

    /**
     * The v1 info hash of a magnet link (xt=urn:btih: 40 hex digits or 32 base32 letters), lower-case hex; null when
     * it is no magnet link this app takes (a v2-only btmh link included).
     */
    static String magnetHash(String magnet) {
        if (magnet == null)
            return null;
        String m = magnet.trim();
        if (!m.regionMatches(true, 0, "magnet:?", 0, 8))
            return null;
        for (String kv : m.substring(8).split("&")) {
            int eq = kv.indexOf('=');
            if (eq < 0 || !kv.substring(0, eq).equalsIgnoreCase("xt"))
                continue;
            String v = kv.substring(eq + 1);
            if (!v.regionMatches(true, 0, "urn:btih:", 0, 9))
                continue;
            String h = v.substring(9);
            if (h.matches("[0-9a-fA-F]{40}"))
                return h.toLowerCase(Locale.ROOT);
            if (h.matches("[A-Za-z2-7]{32}"))
                return base32ToHex(h.toUpperCase(Locale.ROOT));
        }
        return null;
    }

    /** The magnet's display name (dn), or "". */
    static String magnetName(String magnet) {
        if (magnet == null || !magnet.trim().regionMatches(true, 0, "magnet:?", 0, 8))
            return "";
        for (String kv : magnet.trim().substring(8).split("&")) {
            if (kv.regionMatches(true, 0, "dn=", 0, 3)) {
                try {
                    return java.net.URLDecoder.decode(kv.substring(3), "UTF-8");
                } catch (java.io.UnsupportedEncodingException | IllegalArgumentException e) {
                    return kv.substring(3);
                }
            }
        }
        return "";
    }

    private static String base32ToHex(String s) {
        String abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        StringBuilder hex = new StringBuilder(40);
        long buf = 0;
        int bits = 0;
        for (char ch : s.toCharArray()) {
            buf = buf << 5 | abc.indexOf(ch);
            bits += 5;
            while (bits >= 8) {
                bits -= 8;
                hex.append(String.format(Locale.ROOT, "%02x", (buf >> bits) & 0xff));
            }
        }
        return hex.toString();
    }

    // ---- Bencode ----------------------------------------------------------------------------------------------------

    /** A strict bencode reader: integers as Long, strings as byte[], lists, dictionaries (String keys). */
    static final class Bencode {
        private final byte[] b;
        int pos;
        /** Where the top dictionary's « info » value starts and ends (its raw bytes), or -1. */
        int infoStart = -1, infoEnd = -1;
        private int depth;

        Bencode(byte[] b) {
            this.b = b;
        }

        Object value() throws IOException {
            if (pos >= b.length)
                throw new IOException("bencode: truncated");
            if (++depth > 64)
                throw new IOException("bencode: too deep");
            try {
                byte c = b[pos];
                if (c == 'i') {
                    pos++;
                    long v = number('e');
                    return v;
                }
                if (c == 'l') {
                    pos++;
                    List<Object> l = new ArrayList<>();
                    while (peek() != 'e')
                        l.add(value());
                    pos++;
                    return l;
                }
                if (c == 'd') {
                    pos++;
                    Map<String, Object> m = new LinkedHashMap<>();
                    while (peek() != 'e') {
                        String k = new String(bytes(), StandardCharsets.UTF_8);
                        boolean info = depth == 1 && k.equals("info");
                        int start = pos;
                        Object v = value();
                        if (info) {
                            infoStart = start;
                            infoEnd = pos;
                        }
                        m.put(k, v);
                    }
                    pos++;
                    return m;
                }
                if (c >= '0' && c <= '9')
                    return bytes();
                throw new IOException("bencode: bad byte at " + pos);
            } finally {
                depth--;
            }
        }

        private byte peek() throws IOException {
            if (pos >= b.length)
                throw new IOException("bencode: truncated");
            return b[pos];
        }

        private byte[] bytes() throws IOException {
            long n = number(':');
            if (n < 0 || n > b.length - pos)
                throw new IOException("bencode: bad string length");
            byte[] s = java.util.Arrays.copyOfRange(b, pos, pos + (int)n);
            pos += (int)n;
            return s;
        }

        private long number(char end) throws IOException {
            int start = pos;
            while (pos < b.length && b[pos] != end)
                pos++;
            if (pos >= b.length || pos == start || pos - start > 19)
                throw new IOException("bencode: bad number");
            String s = new String(b, start, pos - start, StandardCharsets.US_ASCII);
            pos++;
            try {
                return Long.parseLong(s);
            } catch (NumberFormatException e) {
                throw new IOException("bencode: bad number " + s);
            }
        }
    }
}
