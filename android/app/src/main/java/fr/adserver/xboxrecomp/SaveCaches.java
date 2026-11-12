package fr.adserver.xboxrecomp;

import java.io.File;
import java.io.IOException;
import java.util.Locale;

/**
 * The caches of <files>/save that are not per title (docs: « Game library »): Cache/ (Z:, and the cache partitions
 * behind X: Y: Z:), TitleCache/ (E:\CACHE), Partition3-5.img and launch_data.bin. Saves (UDATA/<tid>, TDATA/<tid>)
 * are per title already and never move.
 *
 * <files>/library/cache_owner holds the title ID that owns the live set. Before launching another title the launcher
 * calls switchTo(): the live set moves aside by rename into save/.stash/<owner>/ (not wiped: a title's unpacked cache
 * can take minutes to rebuild), the new title's stash comes back if it has one, launch_data.bin (a quick-reboot
 * hand-off of the previous title) is deleted, and the owner is written. xbox_path_init recreates what is missing at
 * the next start: Cache/ and sparse Partition1-5.img (src/kernel/kernel_path.c), TitleCache/ on the title's first
 * write.
 *
 * Z: is per title since K-9: the runtime maps it to Cache/<title id in lower case>, so the live Cache holds one
 * folder per title that ran. Before that, Z: was Cache itself. migrateLegacyCache() moves such old top-level entries
 * into Cache/<owner> once, by rename, when the owner is known -- the live set is the owner's by construction -- so a
 * title does not rebuild its utility drive (Fable unpacks 554 MB) after the update. A stash comes back in whatever
 * layout it was stashed in and is migrated then.
 *
 * Tolerant of a move left half done (the process killed in the middle): every step is a rename that never replaces
 * an existing target; an entry that cannot move stays where it is. Plain Java.
 */
final class SaveCaches {
    private SaveCaches() {}

    /** The live set, without launch_data.bin (dropped, not stashed). */
    static final String[] LIVE = { "Cache", "TitleCache", "Partition3.img", "Partition4.img", "Partition5.img" };
    static final String LAUNCH_DATA = "launch_data.bin", STASH = ".stash";
    /** The utility drive's folder in the live set (LIVE[0]). */
    static final String CACHE = "Cache";

    static File stashDir(File saveDir, String titleId) {
        return new File(new File(saveDir, STASH), titleId);
    }

    /** The owner's title ID, or null when unknown (no file, empty, or not a usable name). */
    static String owner(File ownerFile) {
        if (!ownerFile.isFile())
            return null;
        try {
            String s = PrepareProtocol.readText(ownerFile, 256).trim();
            return safeName(s) ? s : null;
        } catch (IOException e) {
            return null;
        }
    }

    static void writeOwner(File ownerFile, String titleId) throws IOException {
        if (!safeName(titleId))
            throw new IOException("bad title ID " + titleId);
        Library.writeAtomically(ownerFile, titleId + "\n");
    }

    /**
     * Makes titleId the owner of the live set before it runs. Returns true when the caches were switched, false when
     * titleId owned them already, or there was no owner (the live set is then taken as this title's: nothing moves,
     * the owner is written).
     */
    static boolean switchTo(File saveDir, File ownerFile, String titleId) throws IOException {
        if (!safeName(titleId))
            throw new IOException("bad title ID " + titleId);
        String owner = owner(ownerFile);
        if (titleId.equals(owner)) {
            migrateLegacyCache(saveDir, titleId);
            return false;
        }
        if (owner == null) {
            writeOwner(ownerFile, titleId);
            return false;
        }
        // 1. The live set into .stash/<owner>.
        File out = stashDir(saveDir, owner);
        for (String name : LIVE) {
            File src = new File(saveDir, name), dst = new File(out, name);
            if (!src.exists() || dst.exists())
                continue;
            if (!(out.mkdirs() || out.isDirectory()))
                throw new IOException("cannot create " + out);
            //noinspection ResultOfMethodCallIgnored
            src.renameTo(dst);
        }
        // 2. The new owner's stash back, where nothing is in the way.
        File in = stashDir(saveDir, titleId);
        boolean cacheBack = false;
        for (String name : LIVE) {
            File src = new File(in, name), dst = new File(saveDir, name);
            if (src.exists() && !dst.exists() && src.renameTo(dst) && name.equals(CACHE))
                cacheBack = true;
        }
        String[] left = in.list();
        if (left != null && left.length == 0)
            //noinspection ResultOfMethodCallIgnored
            in.delete();
        // 2b. A Cache that came back may predate per-title Z: (K-9). One that could not be stashed in step 1 (a
        // half-done earlier switch) is the previous owner's: left as it is.
        if (cacheBack)
            migrateLegacyCache(saveDir, titleId);
        // 3. The previous title's quick-reboot hand-off.
        //noinspection ResultOfMethodCallIgnored
        new File(saveDir, LAUNCH_DATA).delete();
        // 4. The owner.
        writeOwner(ownerFile, titleId);
        return true;
    }

    /**
     * Moves the entries of saveDir/Cache that predate per-title Z: (K-9) into Cache/<titleId in lower case>, by
     * rename. The per-title folders (8 hex digits) are left alone, and an entry whose name is already taken in the
     * title's folder stays where it is: nothing is ever replaced, and a second call finds nothing to move. Only for a
     * title ID the runtime would use as a folder name (not a local key). Returns the number of entries moved.
     * The caller vouches that the live Cache is titleId's (it owns the live set).
     */
    static int migrateLegacyCache(File saveDir, String titleId) {
        String id = Pack.normTitleId(titleId);
        if (id == null)
            return 0;
        File cache = new File(saveDir, CACHE);
        File dst = new File(cache, id.toLowerCase(Locale.ROOT));
        String[] names = cache.list();
        if (names == null)
            return 0;
        int moved = 0;
        for (String n : names) {
            File src = new File(cache, n);
            if (isTitleFolder(n) && src.isDirectory())
                continue;
            File to = new File(dst, n);
            if (to.exists())
                continue;
            if (!(dst.mkdirs() || dst.isDirectory()))
                return moved;
            if (src.renameTo(to))
                moved++;
        }
        return moved;
    }

    /** A Cache/<title id> folder as the runtime names it: 8 hex digits. */
    static boolean isTitleFolder(String n) {
        if (n.length() != 8)
            return false;
        for (int i = 0; i < 8; i++) {
            char c = n.charAt(i);
            if (!(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F'))
                return false;
        }
        return true;
    }

    /**
     * Bytes in a title's caches: the live set when it owns them, else its stash (the Storage screen). Apparent sizes:
     * the Partition images are sparse and usually take far less.
     */
    static long bytes(File saveDir, File ownerFile, String titleId) {
        if (titleId == null)
            return 0;
        if (titleId.equals(owner(ownerFile))) {
            long n = 0;
            for (String name : LIVE)
                n += GameStore.treeSize(new File(saveDir, name));
            return n;
        }
        return GameStore.treeSize(stashDir(saveDir, titleId));
    }

    /** Title IDs are file names here: 8 hex digits or « local-xxxxxxxx », never a path. */
    static boolean safeName(String s) {
        if (s == null || s.isEmpty() || s.length() > 64 || s.equals(".") || s.equals(".."))
            return false;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (!(c >= '0' && c <= '9' || c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c == '-' || c == '_'))
                return false;
        }
        return true;
    }
}
