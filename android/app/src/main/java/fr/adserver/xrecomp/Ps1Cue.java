package fr.adserver.xrecomp;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.IntConsumer;

/**
 * A PlayStation disc's .cue in a library folder seen through the Storage Access Framework (XBA-168): the runtime
 * (consoles/ps1/runtime, ps1_disc_open) reads a .cue and opens its tracks by name next to it, which a document has
 * not. The game activity opens the .cue and every track document (detached descriptors: the game process keeps them
 * for its life), and writes a .cue of its own whose FILE lines name each track as {@code /proc/self/fd/N}, an
 * absolute path to that open descriptor; the runtime and the title key reader (recomp_pack_rom_identify) open those
 * paths like any file. No copy of the disc. A single-file image (.bin, .iso, .chd) needs none of this: it goes over as
 * {@code fd:N} as a GBA ROM does. docs/android/app-shell.md « PlayStation games ».
 */
final class Ps1Cue {
    private Ps1Cue() {
    }

    /** The largest .cue read (a real one is a few hundred bytes). */
    static final int MAX_CUE = 64 * 1024;

    /** Opens a document for reading. */
    interface Reader {
        InputStream open(String uri) throws IOException;
    }

    /** A document's display name (its file name in the folder), or null. */
    interface Namer {
        String name(String uri);
    }

    /**
     * The .cue text with each FILE line's name replaced by the path its track file has here (paths: lower-case
     * base name, the folder part of the .cue's name dropped, to path). The rest of the sheet is kept as it is. Null
     * when a FILE line names a track that is not in paths.
     */
    static String rewrite(String cue, Map<String, String> paths) {
        List<String> out = new java.util.ArrayList<>();
        for (String line : cue.split("\\r?\\n|\\r", -1)) {
            String t = line.trim();
            if (t.length() >= 5 && t.substring(0, 4).equalsIgnoreCase("FILE") && Character.isWhitespace(t.charAt(4))) {
                String rest = t.substring(5).trim(), name, after;
                if (rest.startsWith("\"")) {
                    int end = rest.indexOf('"', 1);
                    if (end < 1)
                        return null;
                    name = rest.substring(1, end);
                    after = rest.substring(end + 1);
                } else {
                    int sp = rest.indexOf(' ');
                    name = sp < 0 ? rest : rest.substring(0, sp);
                    after = sp < 0 ? "" : rest.substring(sp);
                }
                String path = paths.get(baseName(name));
                if (path == null)
                    return null;
                line = "FILE \"" + path + "\"" + after;
            }
            out.add(line);
        }
        String text = String.join("\n", out);
        return text.endsWith("\n") ? text : text + "\n";
    }

    /** A track name's file part, lower case (cue sheets made on Windows use backslashes and any case). */
    static String baseName(String name) {
        String b = name.substring(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
        return b.toLowerCase(Locale.ROOT);
    }

    /**
     * The .cue at cueUri with its track documents parts opened (opener: a detached descriptor each) and named as
     * /proc/self/fd/N, written as outDir/disc.cue; returns its path, or null when the .cue cannot be read or names a
     * track that is not among parts or cannot be opened.
     */
    static String runtimeCue(String cueUri, List<String> parts, Reader reader, Namer namer,
                             DiscImages.Opener opener, File outDir) {
        return runtimeCue(cueUri, parts, reader, namer, opener, DiscImages::closeFd, outDir);
    }

    /**
     * As above, with the closer that gives back a track descriptor already opened when the disc then fails (a later
     * track missing or unopenable, the sheet not rewritten or not written): the fds are detached, nothing else would
     * close them. A test passes its own, since its fake fd numbers are not descriptors it owns.
     */
    static String runtimeCue(String cueUri, List<String> parts, Reader reader, Namer namer,
                             DiscImages.Opener opener, IntConsumer closer, File outDir) {
        List<Integer> opened = new java.util.ArrayList<>();
        String path = runtimeCue(cueUri, parts, reader, namer, opener, outDir, opened);
        if (path == null)
            for (int fd : opened)
                closer.accept(fd);
        return path;
    }

    private static String runtimeCue(String cueUri, List<String> parts, Reader reader, Namer namer,
                                     DiscImages.Opener opener, File outDir, List<Integer> opened) {
        String text;
        try (InputStream in = reader.open(cueUri)) {
            byte[] buf = new byte[MAX_CUE];
            int n = 0, r;
            while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0)
                n += r;
            text = new String(buf, 0, n, StandardCharsets.ISO_8859_1);
        } catch (IOException | RuntimeException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot read the cue sheet " + cueUri, e);
            return null;
        }
        Map<String, String> paths = new HashMap<>();
        for (String want : Console.cueFiles(text)) {
            String key = baseName(want);
            if (paths.containsKey(key))
                continue;
            String uri = null;
            for (String p : parts) {
                String name = namer.name(p);
                if (name != null && name.toLowerCase(Locale.ROOT).equals(key)) {
                    uri = p;
                    break;
                }
            }
            if (uri == null) {
                Log.w(LauncherActivity.TAG, "shell: the cue sheet names " + want + ", not found beside it");
                return null;
            }
            try {
                int fd = opener.open(uri);
                if (fd < 0)
                    return null;
                opened.add(fd);
                paths.put(key, "/proc/self/fd/" + fd);
            } catch (IOException | RuntimeException e) {
                Log.w(LauncherActivity.TAG, "shell: cannot open the track " + uri, e);
                return null;
            }
        }
        String cue = rewrite(text, paths);
        if (cue == null)
            return null;
        File f = new File(outDir, "disc.cue");
        try {
            if (!outDir.isDirectory() && !outDir.mkdirs())
                throw new IOException("cannot make " + outDir);
            try (FileOutputStream o = new FileOutputStream(f)) {
                o.write(cue.getBytes(StandardCharsets.ISO_8859_1));
            }
        } catch (IOException e) {
            Log.w(LauncherActivity.TAG, "shell: cannot write " + f, e);
            return null;
        }
        return f.getAbsolutePath();
    }
}
