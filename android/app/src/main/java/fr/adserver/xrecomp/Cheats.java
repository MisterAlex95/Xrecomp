package fr.adserver.xrecomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * A GBA, PS1, N64, GameCube or Xbox game's cheat codes (XBA-197, XBA-241, XBA-264, docs/android/app-shell.md « Cheats »): one file per
 * game beside its
 * settings, files/settings/game/&lt;KEY&gt;.cheats, which the runtime reads (RECOMP_CHEATS_FILE, set by
 * {@link ShellActivity}) and reads again when it changes, so a change made in the in-game menu takes effect at once.
 *
 * <p>The file's format is the native engine's (src/platform/recomp_cheats.h): one section per cheat, its name in
 * brackets, « enabled=1 » or « enabled=0 », an optional « format= » (auto, raw, gsa, gsa-raw, arv3, arv3-raw, cb, gs, xbox,
 * ar, gecko; auto when absent), then the code's lines. Formats per console: GBA raw, gsa, gsa-raw, arv3, arv3-raw, cb;
 * PS1 and N64 raw, gs; GameCube raw, ar (Action Replay, decrypted), gecko; Xbox xbox. Comments start with #. Written through a temporary file and a rename, so
 * the runtime never reads half a file.
 *
 * <p>The app checks a code's shape only ({@link #check}): lines of hex digits, « AAAAAAAA VVVV » or « AAAAAAAA
 * VVVVVVVV » (GBA), or « ADDRESS:VALUE »; the runtime decodes it (and logs why a code it cannot use is not applied).
 * An Xbox title's codes (XBA-241) are XRecomp's own guest-memory writes (recomp_cheats.h « Xbox »): typed lines
 * « TAAAAAAA VVVVVVVV » (T 0 / 1 / 2: an 8 / 16 / 32-bit write, 4 / 5 / 6: « if the value equals, run the next line »),
 * or raw « ADDRESS:VALUE » / « ADDRESS VALUE » writes; {@link #check} checks those fully, types and sizes included.
 * A GameCube game's codes (XBA-264) are « AAAAAAAA VVVVVVVV » lines (Action Replay decrypted or Gecko, the runtime
 * tells them apart) or « ADDRESS:VALUE »; an encrypted Action Replay code (« XXXX-XXXX-XXXXX ») is refused with its own
 * message ({@link Problem#ENCRYPTED}): the runtime does not decrypt them.
 */
final class Cheats {
    static final String SUFFIX = ".cheats";
    static final String HEADER = "# This game's cheat codes (XRecomp): [name], enabled=1 or 0, then the code's lines.";
    /** The most cheats per game and lines per cheat (the native engine's limits are higher). */
    static final int MAX_CHEATS = 200, MAX_LINES = 256, MAX_NAME = 80;

    static final class Cheat {
        String name;
        boolean enabled;
        String format;          // null: auto
        final List<String> lines = new ArrayList<>();

        Cheat(String name, boolean enabled) {
            this.name = name;
            this.enabled = enabled;
        }

        String code() {
            return String.join("\n", lines);
        }
    }

    /** Why a code cannot be added ({@link #check}). */
    enum Problem { EMPTY, NOT_HEX, SHAPE, TOO_LONG, ENCRYPTED }

    final File file;
    final List<Cheat> list;

    private Cheats(File file, List<Cheat> list) {
        this.file = file;
        this.list = list;
    }

    // ---- Files ------------------------------------------------------------------------------------------------------

    static File file(File filesDir, String key) {
        return new File(GameSettings.dir(filesDir), GameSettings.norm(key) + SUFFIX);
    }

    /** The game's cheats (none when the file is missing or unreadable). */
    static Cheats load(File filesDir, String key) {
        File f = file(filesDir, key);
        String text = "";
        try {
            if (f.isFile())
                text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        } catch (IOException e) {
            text = "";
        }
        return new Cheats(f, parse(text));
    }

    /** Writes the file (temporary file + rename); false on an error. */
    boolean save() {
        File dir = file.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs())
            return false;
        File tmp = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(format(list).getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (IOException e) {
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(file)) {
            tmp.delete();
            return false;
        }
        return true;
    }

    // ---- The format ---------------------------------------------------------------------------------------------------

    static List<Cheat> parse(String text) {
        List<Cheat> out = new ArrayList<>();
        Cheat cur = null;
        for (String raw : text.split("\n", -1)) {
            String l = raw.trim();
            if (l.isEmpty() || l.startsWith("#"))
                continue;
            if (l.startsWith("[")) {
                int end = l.lastIndexOf(']');
                if (end < 0)
                    continue;
                cur = new Cheat(l.substring(1, end).trim(), false);
                out.add(cur);
                continue;
            }
            if (cur == null)
                continue;
            int eq = l.indexOf('=');
            if (eq >= 0) {
                String k = l.substring(0, eq).trim(), v = l.substring(eq + 1).trim().toLowerCase(Locale.ROOT);
                if (k.equals("enabled"))
                    cur.enabled = v.equals("1") || v.equals("on") || v.equals("true") || v.equals("yes");
                else if (k.equals("format"))
                    cur.format = v.isEmpty() || v.equals("auto") ? null : v;
                continue;
            }
            cur.lines.add(l);
        }
        return out;
    }

    static String format(List<Cheat> cheats) {
        StringBuilder b = new StringBuilder(HEADER).append('\n');
        for (Cheat c : cheats) {
            b.append('\n').append('[').append(cleanName(c.name)).append("]\n");
            b.append("enabled=").append(c.enabled ? '1' : '0').append('\n');
            if (c.format != null)
                b.append("format=").append(c.format).append('\n');
            for (String l : c.lines)
                b.append(l).append('\n');
        }
        return b.toString();
    }

    /** One line, no brackets at the start, at most MAX_NAME characters. */
    static String cleanName(String name) {
        String n = name == null ? "" : name.replaceAll("[\\r\\n\\t]+", " ").trim();
        while (n.startsWith("["))
            n = n.substring(1).trim();
        if (n.length() > MAX_NAME)
            n = n.substring(0, MAX_NAME).trim();
        return n;
    }

    // ---- Codes ------------------------------------------------------------------------------------------------------

    /**
     * The code as it is stored: one line per code line, upper case, single spaces, comments and blank lines dropped;
     * « AAAAAAAAVVVV » / « AAAAAAAAVVVVVVVV » typed without the space get it.
     */
    static List<String> normalize(String code) {
        List<String> out = new ArrayList<>();
        if (code == null)
            return out;
        for (String raw : code.split("[\\r\\n]+")) {
            String l = raw;
            int hash = l.indexOf('#');
            if (hash >= 0)
                l = l.substring(0, hash);
            l = l.trim().replaceAll("[\\s,]+", " ").toUpperCase(Locale.ROOT);
            if (l.isEmpty())
                continue;
            if (l.matches("[0-9A-F]{12}|[0-9A-F]{16}"))
                l = l.substring(0, 8) + " " + l.substring(8);
            out.add(l);
        }
        return out;
    }

    /** Null when the code's lines look like codes for the console, else the first problem. */
    static Problem check(Console console, List<String> lines) {
        if (lines.isEmpty())
            return Problem.EMPTY;
        if (lines.size() > MAX_LINES)
            return Problem.TOO_LONG;
        if (console == Console.XBOX)
            return checkXbox(lines);
        if (console == Console.GC)
            return checkGc(lines);
        boolean gba = console == Console.GBA;
        for (String l : lines) {
            String s = l.replace("0X", "");
            if (!s.matches("[0-9A-F :\\-]+"))
                return Problem.NOT_HEX;
            if (s.contains(":")) {
                if (!s.matches("[0-9A-F]{1,8} ?: ?[0-9A-F]{1,8}"))
                    return Problem.SHAPE;
                continue;
            }
            boolean ok = s.matches("[0-9A-F]{8}[ \\-][0-9A-F]{4}") || (gba && s.matches("[0-9A-F]{8} [0-9A-F]{8}"));
            if (!ok)
                return Problem.SHAPE;
        }
        return null;
    }

    /**
     * An Xbox title's lines (XBA-241, the native « xbox » format): « TAAAAAAA VVVVVVVV » with T 0 (value up to FF),
     * 1 (up to FFFF), 2, 4 (up to FF), 5 (up to FFFF) or 6, a conditional (4-6) followed by a line; else a raw write,
     * « ADDRESS:VALUE » or « ADDRESS VALUE » (1-8 digits each).
     */
    private static Problem checkXbox(List<String> lines) {
        boolean open = false;   // a conditional waiting for its line
        for (String l : lines) {
            String s = l.replace("0X", "");
            if (!s.matches("[0-9A-F :\\-]+"))
                return Problem.NOT_HEX;
            if (s.matches("[0-9A-F]{8}[ \\-][0-9A-F]{8}")) {
                int t = Character.digit(s.charAt(0), 16);
                long v = Long.parseLong(s.substring(9), 16);
                if (t == 3 || t > 6)
                    return Problem.SHAPE;
                int size = 1 << (t & 3);
                if ((size == 1 && v > 0xFFL) || (size == 2 && v > 0xFFFFL))
                    return Problem.SHAPE;
                open = t >= 4;
                continue;
            }
            if (!s.matches("[0-9A-F]{1,8} ?[: \\-] ?[0-9A-F]{1,8}"))
                return Problem.SHAPE;
            open = false;
        }
        return open ? Problem.SHAPE : null;
    }

    /**
     * A GameCube game's lines (XBA-264): « AAAAAAAA VVVVVVVV » (Action Replay decrypted, Gecko, a Gecko code's data
     * lines) or « ADDRESS:VALUE »; an encrypted Action Replay line (« XXXX-XXXX-XXXXX », letters and digits) is
     * {@link Problem#ENCRYPTED}.
     */
    private static Problem checkGc(List<String> lines) {
        for (String l : lines)
            if (l.replace(" ", "").matches("[0-9A-Z]{4}-[0-9A-Z]{4}-[0-9A-Z]{5}"))
                return Problem.ENCRYPTED;
        for (String l : lines) {
            String s = l.replace("0X", "");
            if (!s.matches("[0-9A-F :\\-]+"))
                return Problem.NOT_HEX;
            if (s.contains(":")) {
                if (!s.matches("[0-9A-F]{1,8} ?: ?[0-9A-F]{1,8}"))
                    return Problem.SHAPE;
                continue;
            }
            if (!s.matches("[0-9A-F]{8}[ \\-][0-9A-F]{8}"))
                return Problem.SHAPE;
        }
        return null;
    }

    // ---- Changes ----------------------------------------------------------------------------------------------------

    /** Adds a cheat, on; its name a default one (« Cheat N ») when blank. Returns it. */
    Cheat add(String name, List<String> lines, String defaultName) {
        String n = cleanName(name);
        if (n.isEmpty())
            n = defaultName;
        Cheat c = new Cheat(n, true);
        c.lines.addAll(lines);
        list.add(c);
        return c;
    }

    void remove(int i) {
        if (i >= 0 && i < list.size())
            list.remove(i);
    }

    void toggle(int i) {
        if (i >= 0 && i < list.size())
            list.get(i).enabled = !list.get(i).enabled;
    }

    int enabledCount() {
        int n = 0;
        for (Cheat c : list)
            n += c.enabled ? 1 : 0;
        return n;
    }

    List<Cheat> view() {
        return Collections.unmodifiableList(list);
    }
}
