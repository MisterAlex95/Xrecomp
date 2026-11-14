package fr.adserver.xrecomp;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Android's binary XML (« AXML »: the compiled AndroidManifest.xml inside an APK), read into an editable model and
 * written back. Enough for TitleApk's manifest patch (docs/android/app-shell.md « A game's own app »): change attribute
 * values, add attributes, insert elements. The layout is frameworks/base libs/androidfw ResourceTypes.h:
 *
 * <pre>
 * RES_XML_TYPE header (8 bytes), then chunks: STRING_POOL, XML_RESOURCE_MAP (the resource id of each attribute name
 * string, by index: the first resIds.length strings are attribute names), then nodes (START/END_NAMESPACE,
 * START/END_ELEMENT, CDATA), each a 16-byte node header (type, header size, size, line, comment) plus its body.
 * </pre>
 *
 * Strings are indices into the pool (-1: none). A string is only appended, never removed or reordered, so a model
 * written back without changes gives the same bytes. An attribute name with a resource id that the pool does not
 * have yet goes at the end of the resource-mapped prefix, and every index after it moves up by one.
 */
final class BinaryXml {
    static final int RES_XML_TYPE = 0x0003, RES_STRING_POOL_TYPE = 0x0001, RES_XML_RESOURCE_MAP_TYPE = 0x0180;
    static final int START_NAMESPACE = 0x0100, END_NAMESPACE = 0x0101, START_ELEMENT = 0x0102, END_ELEMENT = 0x0103,
                     CDATA = 0x0104;
    /** Res_value data types used here. */
    static final int TYPE_REFERENCE = 0x01, TYPE_STRING = 0x03, TYPE_INT_DEC = 0x10, TYPE_INT_BOOLEAN = 0x12;
    static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";
    private static final int UTF8_FLAG = 0x100, SORTED_FLAG = 0x1;

    /** One attribute of an element; value is a Res_value (type, data), raw a string index or -1. */
    static final class Attr {
        int ns = -1, name, raw = -1, type, data;

        Attr copy() {
            Attr a = new Attr();
            a.ns = ns;
            a.name = name;
            a.raw = raw;
            a.type = type;
            a.data = data;
            return a;
        }
    }

    /** A node: its type, line and comment, and the body fields of its type. */
    static final class Node {
        int type, line, comment = -1;
        /** Namespace: prefix and uri. Element: ns and name. CDATA: data (and type/value in attrs' style below). */
        int a = -1, b = -1;
        /** START_ELEMENT: its attributes and the 1-based id/class/style attribute positions (0: none). */
        final List<Attr> attrs = new ArrayList<>();
        int idIndex, classIndex, styleIndex;
        /** CDATA: the typed value. */
        int cdType, cdData;
        /** A chunk of another type, kept as is. */
        byte[] raw;
    }

    final List<String> strings = new ArrayList<>();
    int[] resIds = new int[0];
    final List<Node> nodes = new ArrayList<>();
    private boolean utf8;
    private int poolFlags;

    // ---- Reading ---------------------------------------------------------------------------------------------

    static BinaryXml parse(byte[] bytes) throws IOException {
        ByteBuffer bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        if (bytes.length < 8 || (bb.getShort(0) & 0xffff) != RES_XML_TYPE)
            throw new IOException("not a binary XML file");
        int headerSize = bb.getShort(2) & 0xffff;
        int total = bb.getInt(4);
        if (total > bytes.length || headerSize < 8)
            throw new IOException("binary XML: bad header");
        BinaryXml x = new BinaryXml();
        int off = headerSize;
        while (off + 8 <= total) {
            int type = bb.getShort(off) & 0xffff;
            int hs = bb.getShort(off + 2) & 0xffff;
            int size = bb.getInt(off + 4);
            if (size < 8 || off + size > total)
                throw new IOException("binary XML: bad chunk at " + off);
            switch (type) {
            case RES_STRING_POOL_TYPE:
                x.readPool(bb, off, size);
                break;
            case RES_XML_RESOURCE_MAP_TYPE: {
                int n = (size - hs) / 4;
                x.resIds = new int[n];
                for (int i = 0; i < n; i++)
                    x.resIds[i] = bb.getInt(off + hs + 4 * i);
                break;
            }
            case START_NAMESPACE: case END_NAMESPACE: case START_ELEMENT: case END_ELEMENT: case CDATA:
                x.nodes.add(readNode(bb, off, type, hs));
                break;
            default: {
                Node n = new Node();
                n.type = type;
                n.raw = Arrays.copyOfRange(bytes, off, off + size);
                x.nodes.add(n);
            }
            }
            off += size;
        }
        return x;
    }

    private void readPool(ByteBuffer bb, int off, int size) throws IOException {
        int count = bb.getInt(off + 8), styles = bb.getInt(off + 12);
        poolFlags = bb.getInt(off + 16);
        int stringsStart = bb.getInt(off + 20);
        if (styles != 0)
            throw new IOException("binary XML: styled strings are not supported");
        utf8 = (poolFlags & UTF8_FLAG) != 0;
        int hs = bb.getShort(off + 2) & 0xffff;
        for (int i = 0; i < count; i++) {
            int p = off + stringsStart + bb.getInt(off + hs + 4 * i);
            if (p >= off + size)
                throw new IOException("binary XML: string " + i + " out of the pool");
            strings.add(utf8 ? readUtf8(bb, p) : readUtf16(bb, p));
        }
    }

    private static String readUtf8(ByteBuffer bb, int p) {
        int c = bb.get(p) & 0xff;
        p += (c & 0x80) != 0 ? 2 : 1;           // the length in UTF-16 units, not needed
        int n = bb.get(p) & 0xff;
        if ((n & 0x80) != 0) {
            n = (n & 0x7f) << 8 | bb.get(p + 1) & 0xff;
            p += 2;
        } else {
            p += 1;
        }
        byte[] b = new byte[n];
        for (int i = 0; i < n; i++)
            b[i] = bb.get(p + i);
        return new String(b, StandardCharsets.UTF_8);
    }

    private static String readUtf16(ByteBuffer bb, int p) {
        int n = bb.getShort(p) & 0xffff;
        if ((n & 0x8000) != 0) {
            n = (n & 0x7fff) << 16 | bb.getShort(p + 2) & 0xffff;
            p += 4;
        } else {
            p += 2;
        }
        char[] c = new char[n];
        for (int i = 0; i < n; i++)
            c[i] = bb.getChar(p + 2 * i);
        return new String(c);
    }

    private static Node readNode(ByteBuffer bb, int off, int type, int hs) {
        Node n = new Node();
        n.type = type;
        n.line = bb.getInt(off + 8);
        n.comment = bb.getInt(off + 12);
        int e = off + hs;
        switch (type) {
        case START_NAMESPACE: case END_NAMESPACE: case END_ELEMENT:
            n.a = bb.getInt(e);
            n.b = bb.getInt(e + 4);
            break;
        case CDATA:
            n.a = bb.getInt(e);
            n.cdType = bb.get(e + 7) & 0xff;
            n.cdData = bb.getInt(e + 8);
            break;
        case START_ELEMENT: {
            n.a = bb.getInt(e);
            n.b = bb.getInt(e + 4);
            int attrStart = bb.getShort(e + 8) & 0xffff, attrSize = bb.getShort(e + 10) & 0xffff;
            int count = bb.getShort(e + 12) & 0xffff;
            n.idIndex = bb.getShort(e + 14) & 0xffff;
            n.classIndex = bb.getShort(e + 16) & 0xffff;
            n.styleIndex = bb.getShort(e + 18) & 0xffff;
            for (int i = 0; i < count; i++) {
                int p = e + attrStart + i * attrSize;
                Attr at = new Attr();
                at.ns = bb.getInt(p);
                at.name = bb.getInt(p + 4);
                at.raw = bb.getInt(p + 8);
                at.type = bb.get(p + 15) & 0xff;
                at.data = bb.getInt(p + 16);
                n.attrs.add(at);
            }
            break;
        }
        default:
        }
        return n;
    }

    // ---- Writing ---------------------------------------------------------------------------------------------

    byte[] toBytes() {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        writeBytes(body, pool());
        ByteBuffer map = le(8 + 4 * resIds.length);
        map.putShort((short)RES_XML_RESOURCE_MAP_TYPE).putShort((short)8).putInt(8 + 4 * resIds.length);
        for (int id : resIds)
            map.putInt(id);
        writeBytes(body, map.array());
        for (Node n : nodes)
            writeBytes(body, node(n));
        byte[] b = body.toByteArray();
        ByteBuffer out = le(8 + b.length);
        out.putShort((short)RES_XML_TYPE).putShort((short)8).putInt(8 + b.length).put(b);
        return out.array();
    }

    private static void writeBytes(ByteArrayOutputStream o, byte[] b) {
        o.write(b, 0, b.length);
    }

    private static ByteBuffer le(int n) {
        return ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN);
    }

    private byte[] pool() {
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        int[] offsets = new int[strings.size()];
        for (int i = 0; i < strings.size(); i++) {
            offsets[i] = data.size();
            writeBytes(data, utf8 ? utf8(strings.get(i)) : utf16(strings.get(i)));
        }
        while (data.size() % 4 != 0)
            data.write(0);
        int header = 28, start = header + 4 * strings.size();
        ByteBuffer b = le(start + data.size());
        b.putShort((short)RES_STRING_POOL_TYPE).putShort((short)header).putInt(start + data.size());
        b.putInt(strings.size()).putInt(0).putInt(poolFlags & ~SORTED_FLAG | (utf8 ? UTF8_FLAG : 0))
         .putInt(start).putInt(0);
        for (int o : offsets)
            b.putInt(o);
        b.put(data.toByteArray());
        return b.array();
    }

    private static byte[] utf8(String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        len8(o, s.length());
        len8(o, b.length);
        writeBytes(o, b);
        o.write(0);
        return o.toByteArray();
    }

    private static void len8(ByteArrayOutputStream o, int n) {
        if (n > 0x7f)
            o.write(0x80 | n >> 8 & 0x7f);
        o.write(n & 0xff);
    }

    private static byte[] utf16(String s) {
        int n = s.length();
        ByteBuffer b = le((n > 0x7fff ? 4 : 2) + 2 * n + 2);
        if (n > 0x7fff)
            b.putShort((short)(0x8000 | n >>> 16)).putShort((short)n);
        else
            b.putShort((short)n);
        for (int i = 0; i < n; i++)
            b.putChar(s.charAt(i));
        b.putShort((short)0);
        return b.array();
    }

    private static byte[] node(Node n) {
        if (n.raw != null)
            return n.raw;
        int body = n.type == START_ELEMENT ? 20 + 20 * n.attrs.size() : n.type == CDATA ? 12 : 8;
        ByteBuffer b = le(16 + body);
        b.putShort((short)n.type).putShort((short)16).putInt(16 + body).putInt(n.line).putInt(n.comment);
        switch (n.type) {
        case CDATA:
            b.putInt(n.a).putShort((short)8).put((byte)0).put((byte)n.cdType).putInt(n.cdData);
            break;
        case START_ELEMENT:
            b.putInt(n.a).putInt(n.b).putShort((short)20).putShort((short)20).putShort((short)n.attrs.size())
             .putShort((short)n.idIndex).putShort((short)n.classIndex).putShort((short)n.styleIndex);
            for (Attr a : n.attrs)
                b.putInt(a.ns).putInt(a.name).putInt(a.raw).putShort((short)8).put((byte)0).put((byte)a.type)
                 .putInt(a.data);
            break;
        default:
            b.putInt(n.a).putInt(n.b);
        }
        return b.array();
    }

    // ---- Strings ---------------------------------------------------------------------------------------------

    /** s, or null for -1 (or an index out of the pool). */
    String str(int i) {
        return i >= 0 && i < strings.size() ? strings.get(i) : null;
    }

    /** The index of a plain string (outside the attribute-name prefix), appended when the pool has none. */
    int string(String s) {
        for (int i = resIds.length; i < strings.size(); i++) {
            if (strings.get(i).equals(s))
                return i;
        }
        strings.add(s);
        return strings.size() - 1;
    }

    /** The index of the attribute name with this resource id, inserted at the end of the prefix when absent. */
    int attrName(String name, int resId) {
        for (int i = 0; i < resIds.length; i++) {
            if (resIds[i] == resId)
                return i;
        }
        int at = resIds.length;
        strings.add(at, name);
        resIds = Arrays.copyOf(resIds, at + 1);
        resIds[at] = resId;
        for (Node n : nodes) {
            if (n.raw != null)
                continue;
            n.comment = up(n.comment, at);
            n.a = up(n.a, at);
            if (n.type != CDATA)
                n.b = up(n.b, at);
            else if (n.cdType == TYPE_STRING)
                n.cdData = up(n.cdData, at);
            for (Attr a : n.attrs) {
                a.ns = up(a.ns, at);
                a.name = up(a.name, at);
                a.raw = up(a.raw, at);
                if (a.type == TYPE_STRING)
                    a.data = up(a.data, at);
            }
        }
        return at;
    }

    private static int up(int i, int at) {
        return i >= at ? i + 1 : i;
    }

    // ---- Elements --------------------------------------------------------------------------------------------

    /** The index of the first START_ELEMENT, or -1. */
    int root() {
        for (int i = 0; i < nodes.size(); i++) {
            if (nodes.get(i).type == START_ELEMENT)
                return i;
        }
        return -1;
    }

    /** The index of the END_ELEMENT closing the element that starts at start. */
    int end(int start) {
        int depth = 0;
        for (int i = start; i < nodes.size(); i++) {
            int t = nodes.get(i).type;
            if (t == START_ELEMENT)
                depth++;
            else if (t == END_ELEMENT && --depth == 0)
                return i;
        }
        throw new IllegalStateException("element at " + start + " is not closed");
    }

    /** The element's name. */
    String name(int el) {
        return str(nodes.get(el).b);
    }

    /** The START_ELEMENT indices of the direct children of el named name (any name when null). */
    List<Integer> children(int el, String name) {
        List<Integer> l = new ArrayList<>();
        int end = end(el);
        for (int i = el + 1; i < end; i++) {
            if (nodes.get(i).type != START_ELEMENT)
                continue;
            if (name == null || name.equals(name(i)))
                l.add(i);
            i = end(i);
        }
        return l;
    }

    /** Every START_ELEMENT index from el to its end, el included. */
    List<Integer> descendants(int el) {
        List<Integer> l = new ArrayList<>();
        int end = end(el);
        for (int i = el; i < end; i++) {
            if (nodes.get(i).type == START_ELEMENT)
                l.add(i);
        }
        return l;
    }

    /** The attribute of el with this resource id, or null. */
    Attr attr(int el, int resId) {
        for (Attr a : nodes.get(el).attrs) {
            if (a.name >= 0 && a.name < resIds.length && resIds[a.name] == resId)
                return a;
        }
        return null;
    }

    /** The attribute of el without a namespace named name (« package » of manifest), or null. */
    Attr plainAttr(int el, String name) {
        for (Attr a : nodes.get(el).attrs) {
            if (a.ns == -1 && name.equals(str(a.name)) && !(a.name < resIds.length && resIds[a.name] != 0))
                return a;
        }
        return null;
    }

    /** The string value of an attribute (raw, else a TYPE_STRING's data), or null. */
    String value(Attr a) {
        if (a == null)
            return null;
        if (a.raw >= 0)
            return str(a.raw);
        return a.type == TYPE_STRING ? str(a.data) : null;
    }

    /** Sets a to the string s (raw and data). */
    void setString(Attr a, String s) {
        int i = string(s);
        a.raw = i;
        a.type = TYPE_STRING;
        a.data = i;
    }

    /** The android: namespace's uri index (the manifest's xmlns:android), added when absent. */
    int androidNs() {
        for (Node n : nodes) {
            if (n.type == START_NAMESPACE && ANDROID_NS.equals(str(n.b)))
                return n.b;
        }
        return string(ANDROID_NS);
    }

    /**
     * The android:name attribute (by resource id) of el, created when absent: its position keeps the attributes sorted
     * by resource id (the framework walks them in that order), and the id/class/style positions follow.
     */
    Attr androidAttr(int el, String name, int resId) {
        Attr a = attr(el, resId);
        if (a != null)
            return a;
        int ns = androidNs();
        a = new Attr();
        a.name = attrName(name, resId);
        a.ns = ns;
        a.type = TYPE_INT_DEC;
        Node n = nodes.get(el);
        int pos = n.attrs.size();
        for (int i = 0; i < n.attrs.size(); i++) {
            Attr o = n.attrs.get(i);
            int oid = o.name >= 0 && o.name < resIds.length ? resIds[o.name] : 0;
            if (oid == 0 || Integer.compareUnsigned(oid, resId) > 0) {
                pos = i;
                break;
            }
        }
        n.attrs.add(pos, a);
        if (n.idIndex > pos)
            n.idIndex++;
        if (n.classIndex > pos)
            n.classIndex++;
        if (n.styleIndex > pos)
            n.styleIndex++;
        return a;
    }

    /** Inserts an empty element named name as the last child of parent; returns its START_ELEMENT index. */
    int addChild(int parent, String name) {
        int end = end(parent);
        Node p = nodes.get(parent);
        Node s = new Node();
        s.type = START_ELEMENT;
        s.line = p.line;
        s.b = string(name);
        Node e = new Node();
        e.type = END_ELEMENT;
        e.line = p.line;
        e.b = s.b;
        nodes.add(end, e);
        nodes.add(end, s);
        return end;
    }

    /** Removes the element at el and everything inside it. */
    void remove(int el) {
        int end = end(el);
        nodes.subList(el, end + 1).clear();
    }
}
