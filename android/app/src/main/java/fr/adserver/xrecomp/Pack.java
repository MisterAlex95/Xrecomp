package fr.adserver.xrecomp;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.Collections;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * A title pack made on the device by the on-device Prepare (docs/android/prepare.md): the game's generated C compiled
 * into <files dir>/packs/<TITLEID>/libtitle.so, described by pack.json. Internal storage only (external storage is
 * mounted noexec). This class only says whether a pack is there and usable; loading it is the native side's job.
 *
 * pack.json: {"format":1, "title_id", "title", "xbe_sha256", "abi":"arm64-v8a", "shell_abi":1, "toolkit",
 * "toolkit_sha256", "lib":"libtitle.so", "lib_sha256", "lib_size", "entry":"recomp_title_main", "cc", "built_at"}, and
 * "env_force":{"RECOMP_LSFG":"0", ...} when the title profile forces options (the native loader sets them over
 * recomp.cfg; the launcher only shows them), and "resume" (XBA-258: the generated code has resume entries,
 * {@link #resume}).
 *
 * Other consoles (XBA-147, {@link Console}): "console" ("xbox" when absent, so every pack made before stays valid, same
 * format 1) and, for gba / ps1 / n64, "title_key" in place of title_id. Such a pack is packs/&lt;console&gt;-&lt;key&gt;
 * ({@link #check(File, Console, String)}, {@link #route}).
 */
final class Pack {
    private Pack() {}

    /** The pack ABI this app's native side loads; a pack built for another one is rebuilt. */
    static final int SHELL_ABI = 1;
    static final int FORMAT = 1;
    static final String PACKS_DIR = "packs", JSON = "pack.json";

    enum Check { VALID, NO_TITLE_ID, NO_PACK, BAD_JSON, BAD_FORMAT, WRONG_CONSOLE, WRONG_TITLE, WRONG_SHELL_ABI, NO_LIB,
                 LIB_SIZE }

    static File packsDir(File filesDir) {
        return new File(filesDir, PACKS_DIR);
    }

    static File packDir(File filesDir, String titleId) {
        return new File(packsDir(filesDir), titleId);
    }

    /**
     * Whether <files>/packs/<titleId> holds a usable pack for that Xbox title (console xbox, or none). A library key of
     * another console (« gba-&lt;KEY&gt; », XBA-158: the GBA's Prepare goes by it) is that console's pack.
     */
    static Check check(File filesDir, String titleId) {
        if (titleId == null || titleId.isEmpty())
            return Check.NO_TITLE_ID;
        Console c = Console.ofKey(titleId);
        if (c != Console.XBOX)
            return check(filesDir, c, titleId.substring(c.id.length() + 1));
        return checkDir(packDir(filesDir, titleId), Console.XBOX, titleId);
    }

    /** The GBA runtime's discovered addresses of a game (XBA-158): &lt;files&gt;/work/&lt;library key&gt;/discovered.txt. */
    static File discoveredFile(File filesDir, String libraryKey) {
        return new File(new File(new File(filesDir, "work"), libraryKey), "discovered.txt");
    }

    /**
     * How many addresses the runs found since the pack was made (XBA-158, the discovery loop): the discovered file's
     * addresses beyond pack.json's « discovered » (what its Prepare handed to the lifter). 0 when there is no pack, no
     * file, or nothing new. The runtime only ever adds to the file (sorted, no repeats).
     */
    static int newlyDiscovered(File filesDir, String libraryKey) {
        return newlyDiscovered(filesDir, libraryKey, true);
    }

    /**
     * {@link #newlyDiscovered(File, String)} without a long read, for the UI thread (AND-3: Home is built again at
     * every refresh): the count of a discovered file seen before with the same length and modification time, or of a
     * small one; -1 when a large file would have to be read (count it on a worker with newlyDiscovered, then build
     * again).
     */
    static int newlyDiscoveredNow(File filesDir, String libraryKey) {
        return newlyDiscovered(filesDir, libraryKey, false);
    }

    private static int newlyDiscovered(File filesDir, String libraryKey, boolean mayRead) {
        File json = new File(packDir(filesDir, libraryKey), JSON);
        int used;
        try {
            used = new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optInt("discovered", 0);
        } catch (IOException | JSONException e) {
            return 0;
        }
        int n = countAddressesCached(discoveredFile(filesDir, libraryKey), mayRead);
        if (n < 0)
            return -1;
        int found = Math.max(0, n - used);
        if (Console.ofKey(libraryKey) == Console.X360)   // XBA-282: shaders met that the pack lacks heal the same way
            found += newShaders(filesDir, libraryKey);
        return found;
    }

    /**
     * XBA-282: the Xbox 360 shaders the runs met (the runtime's dump, &lt;files&gt;/work/&lt;key&gt;/shaders/*.bin)
     * that the pack does not have (packs/&lt;key&gt;/shaders/: the next Prepare translates them; one it cannot
     * translate is kept there as its .bin, so it is not counted again). A directory listing each, no read.
     */
    static int newShaders(File filesDir, String libraryKey) {
        String[] met = new File(new File(new File(filesDir, "work"), libraryKey), "shaders").list();
        if (met == null)
            return 0;
        File have = new File(packDir(filesDir, libraryKey), "shaders");
        int n = 0;
        for (String s : met)
            if ((s.startsWith("vs_") || s.startsWith("ps_")) && s.endsWith(".bin") && !new File(have, s).isFile())
                n++;
        return n;
    }

    /** A discovered file read at once on the UI thread: up to 256 KB (a GBA's, a short N64 history). */
    static final long COUNT_NOW_MAX = 256 << 10;
    /** path -> { length, lastModified, records } of the discovered files counted in this process. */
    private static final Map<String, long[]> COUNTS = new java.util.HashMap<>();
    static int countReadsForTest;

    /**
     * countAddresses, remembered by (length, lastModified): the runtime only appends, so a file that kept both is
     * not read again. -1 when it is not known, larger than COUNT_NOW_MAX, and mayRead is false.
     */
    static int countAddressesCached(File f, boolean mayRead) {
        long len = f.length(), mod = f.lastModified();
        if (len == 0 && !f.isFile())
            return 0;
        String k = f.getAbsolutePath();
        synchronized (COUNTS) {
            long[] c = COUNTS.get(k);
            if (c != null && c[0] == len && c[1] == mod)
                return (int)c[2];
        }
        if (!mayRead && len > COUNT_NOW_MAX)
            return -1;
        int n = countAddresses(f);
        synchronized (COUNTS) {
            countReadsForTest++;
            COUNTS.put(k, new long[] { len, mod, n });
        }
        return n;
    }

    /**
     * The records in a discovered file (one per line, `ADDR [HASH [HEXBYTES]]`; blank lines and # comments skipped),
     * as tools/prepare's reader counts them for pack.json's « discovered ». Read line by line: the N64's file holds
     * the code bytes of each record and grows past tens of MB.
     */
    static int countAddresses(File f) {
        int n = 0;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                 new java.io.InputStreamReader(new java.io.FileInputStream(f), java.nio.charset.StandardCharsets.UTF_8),
                 1 << 16)) {
            for (String line; (line = r.readLine()) != null;) {
                int hash = line.indexOf('#');
                if (hash >= 0)
                    line = line.substring(0, hash);
                if (!line.trim().isEmpty())
                    n++;
            }
        } catch (IOException e) {
            return n;
        }
        return n;
    }

    /**
     * Whether the pack of a game of console c holds a usable pack: an Xbox title's as {@link #check(File, String)}
     * (key = its title ID), another console's in packs/&lt;console&gt;-&lt;key&gt; with that console and title_key.
     */
    static Check check(File filesDir, Console c, String key) {
        if (c == Console.XBOX)
            return check(filesDir, key);
        File dir = packDir(filesDir, c, key);
        return dir == null ? Check.NO_TITLE_ID : checkDir(dir, c, key);
    }

    /** packs/&lt;name&gt; of a game of console c ({@link Console#packDirName}), or null when key cannot name one. */
    static File packDir(File filesDir, Console c, String key) {
        String name = Console.packDirName(c, key);
        return name == null ? null : new File(packsDir(filesDir), name);
    }

    private static Check checkDir(File dir, Console c, String key) {
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
        if (Console.fromPack(o.optString("console", "")) != c)
            return Check.WRONG_CONSOLE;
        if (c == Console.XBOX ? !sameTitleId(o.optString("title_id", ""), key) : !o.optString("title_key", "").equals(key))
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

    /** Where starting a game goes (XBA-147): its console (the runtime the native loader hands it to), key and pack. */
    static final class Route {
        final Console console;
        /** The Xbox title ID or the other console's title key; null when the app cannot read it here (an Xbox image). */
        final String key;
        final Check check;

        Route(Console console, String key, Check check) {
            this.console = console;
            this.key = key;
            this.check = check;
        }
    }

    /**
     * The route of a game: an Xbox game folder (its default.xbe's title ID), an Xbox disc image (key null: the
     * launcher reads its XBE through XisoNative), or a game of another console ({@link Console#identify}: by content,
     * an .iso included). Null when game is none of those. The game goes to ShellActivity as RECOMP_GAME_DIR either
     * way; the native side tells them apart the same way and picks the runtime (pack_loader.c).
     */
    static Route route(File filesDir, File game) {
        if (game == null)
            return null;
        if (game.isDirectory()) {
            String tid = xbeTitleId(new File(game, "default.xbe"));
            return tid == null ? null : new Route(Console.XBOX, tid, check(filesDir, tid));
        }
        Console.Identity id = Console.identify(game);
        if (id != null)
            return new Route(id.console, id.key, check(filesDir, id.console, id.key));
        return Console.detect(game) == Console.XBOX ? new Route(Console.XBOX, null, Check.NO_TITLE_ID) : null;
    }

    /**
     * The SHA-256 of the default.xbe the pack of titleId was built from (pack.json's xbe_sha256, lower case), or null
     * when there is no readable pack.json or it does not say. The library compares it with the active source's XBE:
     * another version of the game needs preparing again.
     */
    static String xbeSha256(File filesDir, String titleId) {
        return hashField(filesDir, titleId, "xbe_sha256");
    }

    /**
     * The toolkit digest the pack of titleId was lifted with (pack.json's toolkit_sha256, lower case; XBA-127), or null
     * when there is no readable pack.json or it does not say (a pack from before the field). PackFreshness compares it
     * with Toolchain.toolkitDigest.
     */
    static String toolkitSha256(File filesDir, String titleId) {
        return hashField(filesDir, titleId, "toolkit_sha256");
    }

    /**
     * The toolkit digest the pack of a game of another console was lifted with (packs/&lt;library key&gt;/pack.json's
     * toolkit_sha256, lower case), or null when there is no readable pack.json or it does not say. PackFreshness
     * compares it with Toolchain.toolkitDigest, as for an Xbox title.
     */
    static String toolkitSha256OfKey(File filesDir, String libraryKey) {
        if (libraryKey == null || libraryKey.isEmpty())
            return null;
        File json = new File(packDir(filesDir, libraryKey), JSON);
        if (!json.isFile())
            return null;
        try {
            String h = new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optString("toolkit_sha256", "").trim();
            return h.isEmpty() ? null : h.toLowerCase(Locale.ROOT);
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /**
     * XBA-258: what the launcher can say, before a launch, about « Resume where I left off » (the automatic slot,
     * XBA-249 / XBA-250). YES: the game's states survive the run (a GBA / PS1 / N64 game always; an Xbox title whose
     * pack.json says "resume": true, its generated code has resume entries). PREPARE_AGAIN: an Xbox pack made before
     * pack.json said (a Prepare again gives it the entries). NO: no Xbox pack, or one whose code has none.
     */
    enum Resume { YES, PREPARE_AGAIN, NO }

    static Resume resume(File filesDir, String libraryKey) {
        if (libraryKey == null || libraryKey.isEmpty())
            return Resume.NO;
        Console c = Console.ofKey(libraryKey);
        if (c != Console.XBOX)
            return c != null && c.hasSaveStates() ? Resume.YES : Resume.NO;
        String id = normTitleId(libraryKey);
        if (id == null)
            return Resume.NO;
        File json = new File(packDir(filesDir, id), JSON);
        if (!json.isFile())
            return Resume.NO;
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(json, 1 << 20));
            if (!o.has("resume"))
                return Resume.PREPARE_AGAIN;
            return o.optBoolean("resume", false) ? Resume.YES : Resume.NO;
        } catch (IOException | JSONException e) {
            return Resume.NO;
        }
    }

    /**
     * Whether the pack of titleId was built with the toolkit's optional enhancements for the title (pack.json's
     * title_layer, profile-free WP6): "on", "off" (left out: the Enhancements switch was off), or null when the toolkit
     * has none for it (or no readable pack.json says).
     */
    static String titleLayer(File filesDir, String titleId) {
        String v = hashField(filesDir, titleId, "title_layer");
        return "on".equals(v) || "off".equals(v) ? v : null;
    }

    private static String hashField(File filesDir, String titleId, String field) {
        String id = normTitleId(titleId);
        if (id == null)
            return null;
        File json = new File(packDir(filesDir, id), JSON);
        if (!json.isFile())
            return null;
        try {
            String h = new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optString(field, "").trim();
            return h.isEmpty() ? null : h.toLowerCase(Locale.ROOT);
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /** The env_force names the native side accepts (src/platform/recomp_cfg.c recomp_cfg_force_key_ok). */
    private static final Pattern ENV_FORCE_KEY = Pattern.compile("RECOMP_[A-Z0-9_]{1,56}");

    /**
     * The options the pack of titleId forces (pack.json's env_force: title.json's, copied by prepare), in their
     * order; empty when there is no readable pack.json or it forces nothing. Entries the native side would refuse
     * (a name not RECOMP_[A-Z0-9_]+, a value that is not a string) are left out.
     */
    static Map<String, String> envForce(File filesDir, String titleId) {
        String id = normTitleId(titleId);
        if (id == null)
            return Collections.emptyMap();
        File json = new File(packDir(filesDir, id), JSON);
        if (!json.isFile())
            return Collections.emptyMap();
        JSONObject env;
        try {
            env = new JSONObject(PrepareProtocol.readText(json, 1 << 20)).optJSONObject("env_force");
        } catch (IOException | JSONException e) {
            return Collections.emptyMap();
        }
        if (env == null)
            return Collections.emptyMap();
        Map<String, String> out = new LinkedHashMap<>();
        for (Iterator<String> it = env.keys(); it.hasNext();) {
            String k = it.next();
            Object v = env.opt(k);
            if (ENV_FORCE_KEY.matcher(k).matches() && v instanceof String)
                out.put(k, (String)v);
        }
        return out;
    }

    /** True when the pack of titleId forces key off: env_force sets it to "0" or "" (RECOMP_LSFG=0). */
    static boolean forcesOff(File filesDir, String titleId, String key) {
        String v = envForce(filesDir, titleId).get(key);
        return v != null && (v.trim().isEmpty() || v.trim().equals("0"));
    }

    /**
     * False when the pack of titleId was built from another XBE than the one whose SHA-256 is xbeSha256. An unknown
     * side (no pack.json, no field, no hash given) is not a mismatch.
     */
    static boolean builtFrom(File filesDir, String titleId, String xbeSha256) {
        String h = xbeSha256(filesDir, titleId);
        return h == null || xbeSha256 == null || xbeSha256.isEmpty() || h.equalsIgnoreCase(xbeSha256.trim());
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
