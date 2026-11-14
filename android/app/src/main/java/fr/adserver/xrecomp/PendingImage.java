package fr.adserver.xrecomp;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;

/**
 * A disc's other image prepared on its first launch (XBA-82, docs/android/prepare.md « Images »). The first Prepare
 * builds default.xbe only and names the other images in pack.json "images" with library "". When the running title
 * launches one of those, the shell (shell_main.c) writes work/TID/image.pending and the launcher offers « Prepare
 * IMAGE »: the image goes into work/TID/images.want, which tools.prepare reads (it then builds that image, and keeps
 * building the ones the pack had), the regular Prepare runs (the same queue), and the next start goes on into the
 * image: image.pending becomes image.next, which pack_loader.c reads, and the launch data the kernel left in the save
 * dir reaches it as after a quick reboot. Declined (Play), the pending image and its launch data are dropped:
 * default.xbe starts from the beginning. Plain Java.
 */
final class PendingImage {
    private PendingImage() {}

    static final String PENDING = "image.pending", NEXT = "image.next", WANT = "images.want";

    /** The image the title launched that is not prepared yet (work/TID/image.pending), or null. */
    static String pending(File work) {
        File f = new File(work, PENDING);
        if (!f.isFile())
            return null;
        try {
            String s = PrepareProtocol.readText(f, 1024).trim();
            return s.isEmpty() ? null : s;
        } catch (IOException e) {
            return null;
        }
    }

    /** An image path for comparison: lowercase, '/', no leading separator (recomp_pack.c image_norm). */
    static String norm(String p) {
        String s = p == null ? "" : p.trim().replace('\\', '/');
        while (s.startsWith("/"))
            s = s.substring(1);
        return s.toLowerCase(Locale.ROOT);
    }

    /** pack.json "images" of the pack in packDir: the library of image, "" when named but not prepared, else null. */
    static String lib(File packDir, String image) {
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(new File(packDir, Pack.JSON), 1 << 20));
            JSONObject images = o.optJSONObject("images");
            if (images == null)
                return null;
            String want = norm(image);
            for (Iterator<String> it = images.keys(); it.hasNext();) {
                String k = it.next();
                if (norm(k).equals(want))
                    return images.optString(k, "");
            }
        } catch (IOException | JSONException ignored) {
        }
        return null;
    }

    /** The pack in packDir has image's library (a plain name of a file there). */
    static boolean built(File packDir, String image) {
        String lib = lib(packDir, image);
        return lib != null && PackHandover.plainName(lib) && new File(packDir, lib).isFile();
    }

    /** « Prepare IMAGE »: image added to work/TID/images.want (once), which the next Prepare builds. */
    static void want(File work, String image) throws IOException {
        Set<String> lines = wanted(work);
        if (!lines.add(norm(image)))
            return;
        StringBuilder b = new StringBuilder();
        for (String l : lines)
            b.append(l).append('\n');
        if (!(work.mkdirs() || work.isDirectory()))
            throw new IOException("cannot create " + work);
        Library.writeAtomically(new File(work, WANT), b.toString());
    }

    /** The images asked for (images.want), normalised. */
    static Set<String> wanted(File work) {
        Set<String> out = new LinkedHashSet<>();
        File f = new File(work, WANT);
        if (!f.isFile())
            return out;
        try {
            for (String l : PrepareProtocol.readText(f, 1 << 16).split("\n")) {
                if (!l.trim().isEmpty())
                    out.add(norm(l));
            }
        } catch (IOException ignored) {
        }
        return out;
    }

    /** Declined, or the image could not be prepared: the pending image and its launch data are dropped. */
    static void drop(File work, File saveDir) {
        //noinspection ResultOfMethodCallIgnored
        new File(work, PENDING).delete();
        //noinspection ResultOfMethodCallIgnored
        new File(saveDir, SaveCaches.LAUNCH_DATA).delete();
    }

    /**
     * Before a start of the title: a pending image now in the pack becomes image.next (the start goes on into it,
     * with the launch data left for it); one still not prepared is dropped with its launch data (default.xbe starts
     * from the beginning, not with a page meant for another image). Returns the image the start goes into, or null.
     */
    static String beforeLaunch(File work, File packDir, File saveDir) {
        String img = pending(work);
        if (img == null)
            return null;
        if (built(packDir, img)) {
            File next = new File(work, NEXT);
            try {
                Library.writeAtomically(next, img + "\n");
                //noinspection ResultOfMethodCallIgnored
                new File(work, PENDING).delete();
                return img;
            } catch (IOException ignored) {
                // falls through: dropped, default.xbe starts
            }
        }
        drop(work, saveDir);
        return null;
    }
}
