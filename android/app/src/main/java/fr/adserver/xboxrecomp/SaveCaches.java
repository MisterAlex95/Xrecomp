package fr.adserver.xboxrecomp;

import java.io.File;
import java.io.IOException;

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
 * Tolerant of a move left half done (the process killed in the middle): every step is a rename that never replaces
 * an existing target; an entry that cannot move stays where it is. Plain Java.
 */
final class SaveCaches {
    private SaveCaches() {}

    /** The live set, without launch_data.bin (dropped, not stashed). */
    static final String[] LIVE = { "Cache", "TitleCache", "Partition3.img", "Partition4.img", "Partition5.img" };
    static final String LAUNCH_DATA = "launch_data.bin", STASH = ".stash";

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
        if (titleId.equals(owner))
            return false;
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
            if (!out.isDirectory() && !out.mkdirs())
                throw new IOException("cannot create " + out);
            //noinspection ResultOfMethodCallIgnored
            src.renameTo(dst);
        }
        // 2. The new owner's stash back, where nothing is in the way.
        File in = stashDir(saveDir, titleId);
        for (String name : LIVE) {
            File src = new File(in, name), dst = new File(saveDir, name);
            if (src.exists() && !dst.exists())
                //noinspection ResultOfMethodCallIgnored
                src.renameTo(dst);
        }
        String[] left = in.list();
        if (left != null && left.length == 0)
            //noinspection ResultOfMethodCallIgnored
            in.delete();
        // 3. The previous title's quick-reboot hand-off.
        //noinspection ResultOfMethodCallIgnored
        new File(saveDir, LAUNCH_DATA).delete();
        // 4. The owner.
        writeOwner(ownerFile, titleId);
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
