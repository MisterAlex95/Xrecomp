package fr.adserver.xrecomp;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Disc images read in place (XBA-126, docs/android/app-shell.md « Disc images read in place »).
 *
 * An image the app can reach every time the game starts -- a path with All files access, or a document whose read
 * permission is persisted -- is not extracted. The game's folder is then a small <b>companion folder</b>,
 * <internal files>/discs/<TID>/, holding the image's XBEs (default.xbe for the title, art, hash and Prepare; the
 * other images for XBA-82) and disc.json, which says where the image is. Everything that reads the game's folder
 * keeps working on it; ShellActivity hands the image itself to the runtime (RECOMP_GAME_DIR = its path, or
 * "fd:N[,M...]" opened from the documents), which serves D:\ from it (src/kernel/kernel_disc.c).
 *
 * The names follow src/xiso/xiso_source.c: .iso .xiso .cso .ciso .zso .cci, and the FAT32 split parts
 * NAME.<n>.EXT, NAME.part<n>.EXT, NAME.EXT.<n> (any part names the set, numbered from 1, or 0).
 */
final class DiscImages {
    private DiscImages() {}

    static final String DISC_JSON = "disc.json";
    static final String DISCS_SUBDIR = "discs";
    static final int MAX_PARTS = 16;
    /** The largest XBE copied into a companion folder (real ones: a few MB). */
    static final long MAX_XBE = 32L << 20;

    private static final String EXT = "(iso|xiso|cso|ciso|zso|cci)";
    private static final Pattern[] SPLITS = {
        Pattern.compile("^(.*\\." + EXT + "\\.)(\\d{1,4})()$", Pattern.CASE_INSENSITIVE),          // Game.iso.1, .001
        Pattern.compile("^(.+\\.part)(\\d{1,4})(\\." + EXT + ")$", Pattern.CASE_INSENSITIVE),      // Game.part1.iso
        Pattern.compile("^(.+\\.)(\\d{1,4})(\\." + EXT + ")$", Pattern.CASE_INSENSITIVE),          // Game.1.iso
    };

    /** A name the app takes for a disc image, or one part of a split image. */
    static boolean isImageName(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        for (String e : new String[] { ".iso", ".xiso", ".cso", ".ciso", ".zso", ".cci" })
            if (n.endsWith(e))
                return true;
        return SPLITS[0].matcher(name).matches();
    }

    /** One part of a split name: its set's key (the name with the number taken out), number and digits. */
    static final class Part {
        final String prefix, suffix;
        final int number, width;

        Part(String prefix, String suffix, int number, int width) {
            this.prefix = prefix;
            this.suffix = suffix;
            this.number = number;
            this.width = width;
        }

        String key() {
            return prefix + "#" + suffix;
        }

        String name(int n) {
            return prefix + String.format(Locale.ROOT, "%0" + width + "d", n) + suffix;
        }
    }

    /** The split part a name is, or null. */
    static Part part(String name) {
        for (Pattern p : SPLITS) {
            Matcher m = p.matcher(name);
            if (!m.matches())
                continue;
            String digits = m.group(p == SPLITS[0] ? 3 : 2);
            String pre = m.group(1);
            String post = p == SPLITS[0] ? "" : m.group(3);
            int width = digits.length() > 1 && digits.startsWith("0") ? digits.length() : 1;
            return new Part(pre, post, Integer.parseInt(digits), width);
        }
        return null;
    }

    /** The parts f belongs to, in order: [f] when it is not a split part or its set is incomplete. */
    static List<File> splitParts(File f) {
        List<File> one = Collections.singletonList(f);
        Part p = part(f.getName());
        if (p == null)
            return one;
        List<File> parts = new ArrayList<>();
        int first = p.number == 0 ? 0 : 1;
        for (int k = first; k < first + MAX_PARTS; k++) {
            File c = new File(f.getParentFile(), p.name(k));
            if (!c.isFile())
                break;
            parts.add(c);
        }
        return parts.size() >= 2 && parts.contains(f) ? parts : one;
    }

    /**
     * The order of picked names forming one image: indexes into names, by part number. One name is itself; several
     * must be the consecutive parts of one set from its first. null when they are not.
     */
    static int[] orderParts(List<String> names) {
        if (names.size() == 1)
            return new int[] { 0 };
        if (names.size() > MAX_PARTS)
            return null;
        String key = null;
        Integer[] idx = new Integer[names.size()];
        int[] num = new int[names.size()];
        for (int i = 0; i < names.size(); i++) {
            Part p = part(names.get(i));
            if (p == null || (key != null && !key.equalsIgnoreCase(p.key())))
                return null;
            key = p.key();
            num[i] = p.number;
            idx[i] = i;
        }
        java.util.Arrays.sort(idx, (a, b) -> Integer.compare(num[a], num[b]));
        int first = num[idx[0]];
        if (first > 1)
            return null;
        int[] out = new int[idx.length];
        for (int i = 0; i < idx.length; i++) {
            if (num[idx[i]] != first + i)
                return null;
            out[i] = idx[i];
        }
        return out;
    }

    // ---- The companion folder ----------------------------------------------------------------------------

    /** Where an image is: a path (All files access), or documents with a persisted read permission, in order. */
    static final class Disc {
        String path = "";
        final List<String> uris = new ArrayList<>();
        /** The parts' display names, in order (the first is the image's name). */
        final List<String> names = new ArrayList<>();
        /** XISO kind name ("iso", "cso split", ...), the bytes stored (all parts) and the files' total. */
        String kind = "";
        long stored, files;

        String name() {
            return names.isEmpty() ? new File(path).getName() : names.get(0);
        }

        boolean viaPath() {
            return !path.isEmpty();
        }

        JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("path", path);
            o.put("uris", new JSONArray(uris));
            o.put("names", new JSONArray(names));
            o.put("kind", kind);
            o.put("stored", stored);
            o.put("files", files);
            return o;
        }

        static Disc fromJson(JSONObject o) {
            Disc d = new Disc();
            d.path = o.optString("path", "");
            JSONArray u = o.optJSONArray("uris"), n = o.optJSONArray("names");
            for (int i = 0; u != null && i < u.length(); i++)
                d.uris.add(u.optString(i));
            for (int i = 0; n != null && i < n.length(); i++)
                d.names.add(n.optString(i));
            d.kind = o.optString("kind", "");
            d.stored = o.optLong("stored", 0);
            d.files = o.optLong("files", 0);
            return d;
        }
    }

    static File discsDir(Context c) {
        return new File(c.getFilesDir(), DISCS_SUBDIR);
    }

    static File companionDir(Context c, String tid) {
        return new File(discsDir(c), tid);
    }

    /** The disc a companion folder stands for, or null when dir is not one. */
    static Disc read(File dir) {
        if (dir == null)
            return null;
        File f = new File(dir, DISC_JSON);
        if (!f.isFile())
            return null;
        StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null)
                sb.append(line);
            return Disc.fromJson(new JSONObject(sb.toString()));
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    static boolean isCompanion(String dir) {
        return dir != null && new File(dir, DISC_JSON).isFile();
    }

    static void write(File dir, Disc d) throws IOException {
        File tmp = new File(dir, DISC_JSON + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(d.toJson().toString(1).getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        } catch (JSONException e) {
            throw new IOException(e);
        }
        if (!tmp.renameTo(new File(dir, DISC_JSON)))
            throw new IOException("cannot write " + DISC_JSON + " in " + dir);
    }

    // ---- What the game process gets ----------------------------------------------------------------------

    /** Opens a document for reading and hands over its fd (ParcelFileDescriptor.detachFd): the caller owns it. */
    interface Opener {
        int open(String uri) throws IOException;
    }

    /**
     * RECOMP_GAME_DIR for a disc: its path when it is readable, else "fd:N[,M...]" from its documents (the fds are
     * detached: the game process keeps them open for its life, and a restart -- a quick reboot, a hand-over to
     * another image -- is a new process that opens them again). null when the image cannot be reached.
     */
    static String runtimeSpec(Disc d, Opener opener) {
        if (d.viaPath()) {
            File f = new File(d.path);
            return f.isFile() && f.canRead() ? d.path : null;
        }
        if (d.uris.isEmpty() || opener == null)
            return null;
        List<Integer> fds = new ArrayList<>();
        try {
            for (String u : d.uris) {
                int fd = opener.open(u);
                if (fd < 0)
                    throw new IOException("cannot open " + u);
                fds.add(fd);
            }
        } catch (IOException | RuntimeException e) {
            for (int fd : fds)
                closeFd(fd);
            return null;
        }
        StringBuilder sb = new StringBuilder("fd:");
        for (int i = 0; i < fds.size(); i++)
            sb.append(i == 0 ? "" : ",").append(fds.get(i));
        return sb.toString();
    }

    static void closeFd(int fd) {
        try {
            ParcelFileDescriptor.adoptFd(fd).close();
        } catch (IOException | RuntimeException | LinkageError ignored) {
        }
    }

    /** The production Opener: the document opened through the content resolver, its fd detached. */
    static Opener contentOpener(Context c) {
        return u -> {
            ParcelFileDescriptor p = c.getContentResolver().openFileDescriptor(Uri.parse(u), "r");
            if (p == null)
                throw new IOException("cannot open " + u);
            return p.detachFd();
        };
    }

    /** Images are read in place unless recomp.cfg says RECOMP_DISC_IMAGE=0 (then extracted, as before). */
    static boolean inPlaceAllowed(Context c) {
        return !"0".equals(GameStore.cfgValue(c, "RECOMP_DISC_IMAGE"));
    }

    /** A picked document's path, when the provider is a storage volume (as GameStore.treeUriToPath for a tree). */
    static String documentUriToPath(Uri doc) {
        String auth = doc.getAuthority(), id;
        try {
            id = DocumentsContract.getDocumentId(doc);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if ("com.android.providers.downloads.documents".equals(auth) && id.startsWith("raw:"))
            return id.substring(4);
        if (!"com.android.externalstorage.documents".equals(auth))
            return null;
        int colon = id.indexOf(':');
        if (colon < 0)
            return null;
        String vol = id.substring(0, colon), rel = id.substring(colon + 1);
        String root = vol.equals("primary") ? Environment.getExternalStorageDirectory().getAbsolutePath()
                    : vol.equals("home") ? new File(Environment.getExternalStorageDirectory(),
                                                    Environment.DIRECTORY_DOCUMENTS).getAbsolutePath()
                    : "/storage/" + vol;
        while (rel.startsWith("/"))
            rel = rel.substring(1);
        return rel.isEmpty() ? null : root + "/" + rel;
    }

    /** "Game.iso", "Game.1.cso + 1 part", for the screens. */
    static String label(Disc d) {
        int n = d.viaPath() ? 1 : d.uris.size();
        return n > 1 ? d.name() + " (+" + (n - 1) + ")" : d.name();
    }
}
