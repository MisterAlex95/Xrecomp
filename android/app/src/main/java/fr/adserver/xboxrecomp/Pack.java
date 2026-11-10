package fr.adserver.xboxrecomp;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.Locale;

/**
 * A title pack made on the device by the on-device Prepare (docs/android/prepare.md): the game's generated C compiled
 * into <files dir>/packs/<TITLEID>/libtitle.so, described by pack.json. Internal storage only (external storage is
 * mounted noexec). This class only says whether a pack is there and usable; loading it is the native side's job.
 *
 * pack.json: {"format":1, "title_id", "title", "xbe_sha256", "abi":"arm64-v8a", "shell_abi":1, "toolkit",
 * "lib":"libtitle.so", "lib_sha256", "lib_size", "entry":"recomp_title_main", "cc", "built_at"}.
 */
final class Pack {
    private Pack() {}

    /** The pack ABI this app's native side loads; a pack built for another one is rebuilt. */
    static final int SHELL_ABI = 1;
    static final int FORMAT = 1;
    static final String PACKS_DIR = "packs", JSON = "pack.json";

    enum Check { VALID, NO_TITLE_ID, NO_PACK, BAD_JSON, BAD_FORMAT, WRONG_TITLE, WRONG_SHELL_ABI, NO_LIB, LIB_SIZE }

    static File packsDir(File filesDir) {
        return new File(filesDir, PACKS_DIR);
    }

    static File packDir(File filesDir, String titleId) {
        return new File(packsDir(filesDir), titleId);
    }

    /** Whether <files>/packs/<titleId> holds a usable pack for that title. */
    static Check check(File filesDir, String titleId) {
        if (titleId == null || titleId.isEmpty())
            return Check.NO_TITLE_ID;
        File dir = packDir(filesDir, titleId);
        File json = new File(dir, JSON);
        if (!json.isFile())
            return Check.NO_PACK;
        JSONObject o;
        try {
            o = new JSONObject(PrepareProtocol.readText(json, 1 << 20));
        } catch (IOException | JSONException e) {
            return Check.BAD_JSON;
        }
        if (o.optInt("format", -1) != FORMAT)
            return Check.BAD_FORMAT;
        if (!sameTitleId(o.optString("title_id", ""), titleId))
            return Check.WRONG_TITLE;
        if (o.optInt("shell_abi", -1) != SHELL_ABI)
            return Check.WRONG_SHELL_ABI;
        String lib = o.optString("lib", "");
        if (lib.isEmpty() || lib.contains("/") || lib.equals("..") || lib.equals("."))
            return Check.NO_LIB;
        File so = new File(dir, lib);
        if (!so.isFile())
            return Check.NO_LIB;
        long want = o.optLong("lib_size", -1);
        if (want <= 0 || so.length() != want)
            return Check.LIB_SIZE;
        return Check.VALID;
    }

    /**
     * Removes packs/<titleId> (the native loader refused it): the next check says NO_PACK and the launcher offers
     * Prepare, which builds it again (its link step redoes a pack without pack.json).
     */
    static void drop(File filesDir, String titleId) {
        String id = normTitleId(titleId);
        if (id != null)
            GameStore.deleteTree(packDir(filesDir, id));
    }

    /** « 4D530004 », « 0x4d530004 » and « 4d530004 » are the same title ID. */
    static boolean sameTitleId(String a, String b) {
        String x = normTitleId(a), y = normTitleId(b);
        return x != null && x.equals(y);
    }

    static String normTitleId(String s) {
        if (s == null)
            return null;
        String t = s.trim();
        if (t.startsWith("0x") || t.startsWith("0X"))
            t = t.substring(2);
        if (t.isEmpty() || t.length() > 8)
            return null;
        try {
            return String.format(Locale.ROOT, "%08X", Long.parseLong(t, 16));
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /**
     * The title ID of an XBE, from its certificate, as 8 upper-case hex digits; null when the file is not an XBE.
     * Header: "XBEH", base address at 0x104, certificate address at 0x118; the title ID at certificate + 0x08.
     */
    static String xbeTitleId(File xbe) {
        if (xbe == null)
            return null;
        try (RandomAccessFile f = new RandomAccessFile(xbe, "r")) {
            if (f.length() < 0x11c)
                return null;
            byte[] hdr = new byte[0x11c];
            f.readFully(hdr);
            if (hdr[0] != 'X' || hdr[1] != 'B' || hdr[2] != 'E' || hdr[3] != 'H')
                return null;
            long base = le32(hdr, 0x104), cert = le32(hdr, 0x118);
            long off = cert - base + 0x08;
            if (cert < base || off + 4 > f.length())
                return null;
            byte[] id = new byte[4];
            f.seek(off);
            f.readFully(id);
            return String.format(Locale.ROOT, "%08X", le32(id, 0));
        } catch (IOException e) {
            return null;
        }
    }

    private static long le32(byte[] b, int o) {
        return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24;
    }
}
