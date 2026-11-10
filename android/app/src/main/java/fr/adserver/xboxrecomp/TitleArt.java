package fr.adserver.xboxrecomp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The game's title image, for Home's hero: the XBE's « $$XTIMAGE » section, decoded on the device and cached as a PNG
 * per title ID (<files dir>/art/<TITLEID>.png), plus the monogram fallback and the colour the backdrop is tinted with.
 * Nothing of the game is in the APK or the repo: the image comes from the player's own default.xbe.
 *
 * Formats (public documentation, xboxdevwiki « Xbe » and « XPR »; docs/formats/xbe.md):
 * - XBE: base address at 0x104, section count at 0x11C, section headers' address at 0x120 (56 bytes each: raw
 *   address +0x0C, raw size +0x10, name address +0x14); addresses minus the base address are file offsets in the
 *   header area.
 * - $$XTIMAGE holds an XPR0 resource file: "XPR0", total size (+4), header size (+8, the data follows it, 2048-aligned),
 *   then 20-byte resource headers (Common, Data, Lock, Format, Size) until 0xFFFFFFFF. A texture has type 4 in Common
 *   bits 16..18; Data is its offset from the end of the header. Format: bits 8..15 the colour format, bits 20..23 /
 *   24..27 log2 of the width / height (Size, when not 0, gives width - 1 and height - 1 in 12-bit fields).
 * - Colour formats (src/d3d/d3d8_xbox.h, nv2a_regs.h): 0x0C DXT1, 0x0E DXT3, 0x0F DXT5 (4x4 blocks, not swizzled);
 *   0x06 A8R8G8B8, 0x07 X8R8G8B8, 0x05 R5G6B5 swizzled (Morton order, src/d3d/d3d8_swizzle.h); 0x12, 0x1E, 0x11 their
 *   linear forms.
 */
final class TitleArt {
    private TitleArt() {}

    static final String SECTION = "$$XTIMAGE";
    /** The largest section or image taken (title images are 128 x 128 DXT1, 8 KB). */
    private static final int MAX_BYTES = 4 << 20, MAX_SIDE = 1024;

    static File cacheFile(File filesDir, String titleId) {
        return new File(new File(filesDir, "art"), titleId + ".png");
    }

    /**
     * The title image for titleId: from the cache, else decoded from xbe and cached. null when the XBE has none (or
     * one this class cannot read); the launcher then shows a monogram.
     */
    static Bitmap load(File filesDir, File xbe, String titleId) {
        if (titleId == null)
            return null;
        File png = cacheFile(filesDir, titleId);
        if (png.isFile()) {
            Bitmap b = BitmapFactory.decodeFile(png.getAbsolutePath());
            if (b != null)
                return b;
        }
        if (xbe == null)
            return null;
        byte[] sec = xbeSection(xbe, SECTION);
        int[] wh = new int[2];
        int[] px = sec == null ? null : decodeXpr(sec, wh);
        if (px == null)
            return null;
        Bitmap b = Bitmap.createBitmap(px, wh[0], wh[1], Bitmap.Config.ARGB_8888);
        File dir = png.getParentFile();
        if (dir != null)
            dir.mkdirs();
        try (OutputStream o = new FileOutputStream(png)) {
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (IOException ignored) {   // shown anyway, decoded again next time
        }
        return b;
    }

    // ---- XBE ---------------------------------------------------------------------------------------------------

    static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24;
    }

    /** The raw bytes of the XBE section called name, or null (no such section, not an XBE, unreadable). */
    static byte[] xbeSection(File xbe, String name) {
        try (RandomAccessFile f = new RandomAccessFile(xbe, "r")) {
            long len = f.length();
            if (len < 0x178)
                return null;
            byte[] hdr = new byte[0x178];
            f.readFully(hdr);
            if (hdr[0] != 'X' || hdr[1] != 'B' || hdr[2] != 'E' || hdr[3] != 'H')
                return null;
            long base = le32(hdr, 0x104) & 0xffffffffL, headers = le32(hdr, 0x108) & 0xffffffffL;
            int n = le32(hdr, 0x11c);
            long secs = (le32(hdr, 0x120) & 0xffffffffL) - base;
            if (n <= 0 || n > 512 || secs < 0 || headers > MAX_BYTES || secs + 56L * n > Math.min(len, headers))
                return null;
            byte[] head = new byte[(int)Math.min(len, headers)];
            f.seek(0);
            f.readFully(head);
            byte[] want = name.getBytes(StandardCharsets.US_ASCII);
            for (int i = 0; i < n; i++) {
                int sh = (int)secs + 56 * i;
                long nameOff = (le32(head, sh + 0x14) & 0xffffffffL) - base;
                if (nameOff < 0 || nameOff + want.length + 1 > head.length)
                    continue;
                boolean same = head[(int)nameOff + want.length] == 0;
                for (int k = 0; same && k < want.length; k++)
                    same = head[(int)nameOff + k] == want[k];
                if (!same)
                    continue;
                long raw = le32(head, sh + 0x0c) & 0xffffffffL, size = le32(head, sh + 0x10) & 0xffffffffL;
                if (size == 0 || size > MAX_BYTES || raw + size > len)
                    return null;
                byte[] out = new byte[(int)size];
                f.seek(raw);
                f.readFully(out);
                return out;
            }
        } catch (IOException ignored) {
        }
        return null;
    }

    // ---- XPR0 --------------------------------------------------------------------------------------------------

    /** The first texture of an XPR0 file as ARGB pixels (wh gets its width and height), or null. */
    static int[] decodeXpr(byte[] b, int[] wh) {
        if (b.length < 32 || b[0] != 'X' || b[1] != 'P' || b[2] != 'R' || b[3] != '0')
            return null;
        int hdr = le32(b, 8);
        if (hdr < 12 || hdr > b.length)
            return null;
        for (int o = 12; o + 20 <= hdr; o += 20) {
            int common = le32(b, o);
            if (common == 0xFFFFFFFF)
                break;
            if ((common >> 16 & 7) != 4)
                continue;   // not a texture
            int data = le32(b, o + 4), format = le32(b, o + 12), size = le32(b, o + 16);
            int fmt = format >> 8 & 0xff;
            int w = 1 << (format >> 20 & 0xf), h = 1 << (format >> 24 & 0xf);
            if (size != 0) {
                w = (size & 0xfff) + 1;
                h = (size >> 12 & 0xfff) + 1;
            }
            if (w > MAX_SIDE || h > MAX_SIDE)
                return null;
            long at = (long)hdr + (data & 0xffffffffL);
            int need = bytesFor(fmt, w, h);
            if (need <= 0)
                return null;
            if (at + need > b.length && (data & 0xffffffffL) >= hdr)
                at = data & 0xffffffffL;   // an absolute offset past the header, as some writers put it
            if (at < 0 || at + need > b.length)
                return null;
            int[] px = decode(b, (int)at, fmt, w, h);
            if (px != null) {
                wh[0] = w;
                wh[1] = h;
            }
            return px;
        }
        return null;
    }

    static int bytesFor(int fmt, int w, int h) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        switch (fmt) {
        case 0x0C:
            return bw * bh * 8;
        case 0x0E:
        case 0x0F:
            return bw * bh * 16;
        case 0x06:
        case 0x07:
        case 0x12:
        case 0x1E:
            return w * h * 4;
        case 0x05:
        case 0x11:
            return w * h * 2;
        default:
            return -1;
        }
    }

    static int[] decode(byte[] b, int at, int fmt, int w, int h) {
        int[] px = new int[w * h];
        switch (fmt) {
        case 0x0C:
        case 0x0E:
        case 0x0F:
            decodeDxt(b, at, fmt, w, h, px);
            return px;
        case 0x06:
        case 0x07:
        case 0x05:
            if (Integer.bitCount(w) != 1 || Integer.bitCount(h) != 1)
                return null;
            int[] xm = new int[1], ym = new int[1];
            swizzleMasks(w, h, xm, ym);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++)
                    px[y * w + x] = texel(b, at, fmt, deposit(x, xm[0]) | deposit(y, ym[0]));
            }
            return px;
        case 0x12:
        case 0x1E:
        case 0x11:
            for (int i = 0; i < w * h; i++)
                px[i] = texel(b, at, fmt, i);
            return px;
        default:
            return null;
        }
    }

    private static int texel(byte[] b, int at, int fmt, int i) {
        if (fmt == 0x05 || fmt == 0x11) {
            int v = (b[at + 2 * i] & 0xff) | (b[at + 2 * i + 1] & 0xff) << 8;
            return rgb565(v);
        }
        int v = le32(b, at + 4 * i);
        return fmt == 0x07 || fmt == 0x1E ? v | 0xFF000000 : v;
    }

    /** The NV2A swizzle masks (xemu's generate_swizzle_masks): x and y bits interleaved, x first. */
    static void swizzleMasks(int w, int h, int[] xm, int[] ym) {
        int x = 0, y = 0, bit = 1;
        for (int i = 1; i < w || i < h; i <<= 1) {
            if (i < w) {
                x |= bit;
                bit <<= 1;
            }
            if (i < h) {
                y |= bit;
                bit <<= 1;
            }
        }
        xm[0] = x;
        ym[0] = y;
    }

    static int deposit(int v, int mask) {
        int r = 0, bit = 1;
        while (mask != 0) {
            int low = mask & -mask;
            if ((v & bit) != 0)
                r |= low;
            mask &= mask - 1;
            bit <<= 1;
        }
        return r;
    }

    static int rgb565(int v) {
        int r = v >> 11 & 31, g = v >> 5 & 63, bl = v & 31;
        return 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (bl << 3 | bl >> 2);
    }

    /** DXT1 / DXT3 / DXT5 (BC1 / BC2 / BC3) blocks, row by row, into px. */
    static void decodeDxt(byte[] b, int at, int fmt, int w, int h, int[] px) {
        int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == 0x0C ? 8 : 16;
        int[] pal = new int[4];
        int[] alpha = new int[16];
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int o = at + (by * bw + bx) * bs;
                if (fmt == 0x0E) {
                    for (int i = 0; i < 16; i++) {
                        int nib = (b[o + i / 2] >> ((i & 1) * 4)) & 0xf;
                        alpha[i] = nib * 17;
                    }
                } else if (fmt == 0x0F) {
                    int a0 = b[o] & 0xff, a1 = b[o + 1] & 0xff;
                    long bits = 0;
                    for (int k = 0; k < 6; k++)
                        bits |= (long)(b[o + 2 + k] & 0xff) << (8 * k);
                    for (int i = 0; i < 16; i++) {
                        int code = (int)(bits >> (3 * i) & 7);
                        int a;
                        if (code == 0)
                            a = a0;
                        else if (code == 1)
                            a = a1;
                        else if (a0 > a1)
                            a = ((8 - code) * a0 + (code - 1) * a1) / 7;
                        else if (code == 6)
                            a = 0;
                        else if (code == 7)
                            a = 255;
                        else
                            a = ((6 - code) * a0 + (code - 1) * a1) / 5;
                        alpha[i] = a;
                    }
                }
                int c = fmt == 0x0C ? o : o + 8;
                int c0 = (b[c] & 0xff) | (b[c + 1] & 0xff) << 8, c1 = (b[c + 2] & 0xff) | (b[c + 3] & 0xff) << 8;
                pal[0] = rgb565(c0);
                pal[1] = rgb565(c1);
                if (c0 > c1 || fmt != 0x0C) {
                    pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                    pal[3] = mix(pal[0], pal[1], 1, 2, 3);
                } else {
                    pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                    pal[3] = 0;   // transparent black
                }
                int idx = le32(b, c + 4);
                for (int i = 0; i < 16; i++) {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h)
                        continue;
                    int col = pal[idx >>> (2 * i) & 3];
                    if (fmt != 0x0C)
                        col = (col & 0x00FFFFFF) | alpha[i] << 24;
                    px[y * w + x] = col;
                }
            }
        }
    }

    private static int mix(int a, int b, int wa, int wb, int d) {
        int r = (((a >> 16 & 0xff) * wa) + ((b >> 16 & 0xff) * wb)) / d;
        int g = (((a >> 8 & 0xff) * wa) + ((b >> 8 & 0xff) * wb)) / d;
        int bl = (((a & 0xff) * wa) + ((b & 0xff) * wb)) / d;
        return 0xFF000000 | r << 16 | g << 8 | bl;
    }

    // ---- Colour and monogram ---------------------------------------------------------------------------------

    /**
     * The colour to tint the backdrop with: the mean of the image's saturated, opaque pixels, brought to a vivid but
     * not glaring value; the accent when the image is grey.
     */
    static int tint(Bitmap b) {
        if (b == null)
            return Theme.ACCENT;
        int w = b.getWidth(), h = b.getHeight(), step = Math.max(1, Math.max(w, h) / 48);
        float[] hsv = new float[3];
        double sx = 0, sy = 0, wsum = 0, sat = 0;
        for (int y = 0; y < h; y += step) {
            for (int x = 0; x < w; x += step) {
                int p = b.getPixel(x, y);
                if (Color.alpha(p) < 128)
                    continue;
                Color.colorToHSV(p, hsv);
                double wt = hsv[1] * hsv[2];
                if (wt < 0.08)
                    continue;
                double a = Math.toRadians(hsv[0]);
                sx += Math.cos(a) * wt;
                sy += Math.sin(a) * wt;
                sat += hsv[1] * wt;
                wsum += wt;
            }
        }
        if (wsum <= 0)
            return Theme.ACCENT;
        float hue = (float)Math.toDegrees(Math.atan2(sy, sx));
        float s = (float)Math.max(0.45, Math.min(0.8, sat / wsum));
        return Theme.hsv(hue, s, 0.92f);
    }

    /** The monogram's colour: a hue from the title ID (stable for a title), vivid. */
    static int monogramColor(String titleId) {
        int hsh = titleId == null ? 0 : titleId.toUpperCase(Locale.ROOT).hashCode();
        hsh ^= hsh >>> 16;
        hsh *= 0x45d9f3b;
        hsh ^= hsh >>> 16;
        return Theme.hsv((hsh & 0x7fffffff) % 360, 0.62f, 0.9f);
    }

    /** « Sample Game » -> « SG », « Halo » -> « HA », « 4x4 Evo 2 » -> « 4E ». */
    static String initials(String title) {
        if (title == null)
            return "";
        String[] words = title.trim().split("[\\s:\\-_]+");
        StringBuilder sb = new StringBuilder();
        for (String wd : words) {
            if (wd.isEmpty())
                continue;
            int cp = wd.codePointAt(0);
            if (Character.isLetterOrDigit(cp))
                sb.appendCodePoint(Character.toUpperCase(cp));
            if (sb.codePointCount(0, sb.length()) == 2)
                break;
        }
        if (sb.codePointCount(0, sb.length()) == 1 && words.length > 0 && words[0].length() > 1) {
            int second = words[0].offsetByCodePoints(0, 1);
            int cp = words[0].codePointAt(second);
            if (Character.isLetterOrDigit(cp))
                sb.appendCodePoint(Character.toUpperCase(cp));
        }
        return sb.toString();
    }
}
