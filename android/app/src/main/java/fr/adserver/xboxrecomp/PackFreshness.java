package fr.adserver.xboxrecomp;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Whether a valid title pack still matches (XBA-127, docs/android/prepare.md « When a pack is stale »):
 *
 * - OTHER_XBE: the game folder's default.xbe is not the one the pack was built from (pack.json xbe_sha256): another
 *   revision or region of the disc under the same title ID. The pack is never run; Home offers Prepare again.
 * - OLDER_TOOLKIT: the pack was lifted by another toolkit than the one this APK carries (pack.json toolkit_sha256
 *   against assets/prepare/toolkit.zip.id, the digest of tools/prepare/toolkit.py). It still loads when its shell_abi
 *   matches (Pack.check), but misses the lifter's and the profiles' fixes since: Home keeps Play and offers Prepare
 *   again with a note. The digest covers the toolkit tree only, so an update that changed Java or the shell's .c files
 *   keeps every pack current.
 *
 * An unknown side (no hash in pack.json, a pack from before toolkit_sha256, an APK without the toolchain or without
 * the digest) is not a mismatch, except a pack without toolkit_sha256 under an APK that has a digest: it was made
 * before the digest existed, so by an older toolkit.
 *
 * The XBE's SHA-256 is kept per (path, size, mtime) for the process, so Home and Play hash a game's XBE once (a few MB;
 * game data is never hashed).
 */
final class PackFreshness {
    private PackFreshness() {}

    enum Verdict { CURRENT, OTHER_XBE, OLDER_TOOLKIT }

    /** The verdict from the four hashes; null or "" is « unknown ». OTHER_XBE wins over OLDER_TOOLKIT. */
    static Verdict judge(String packXbe, String gameXbe, String packToolkit, String appToolkit) {
        if (LibraryScreen.isSha256(packXbe) && LibraryScreen.isSha256(gameXbe) && !packXbe.equalsIgnoreCase(gameXbe))
            return Verdict.OTHER_XBE;
        if (LibraryScreen.isSha256(appToolkit)
            && !(packToolkit != null && packToolkit.trim().equalsIgnoreCase(appToolkit)))
            return Verdict.OLDER_TOOLKIT;
        return Verdict.CURRENT;
    }

    /** The verdict for the pack of tid against the game's XBE hash (null: not known yet) and this APK's toolkit. */
    static Verdict of(File filesDir, String tid, String gameXbe, String appToolkit) {
        String id = Pack.normTitleId(tid);
        if (id == null || !new File(Pack.packDir(filesDir, id), Pack.JSON).isFile())
            return Verdict.CURRENT;   // no pack: nothing to judge
        return judge(Pack.xbeSha256(filesDir, tid), gameXbe, Pack.toolkitSha256(filesDir, tid), appToolkit);
    }

    private static final class Known {
        final long size, mtime;
        final String sha;

        Known(long size, long mtime, String sha) {
            this.size = size;
            this.mtime = mtime;
            this.sha = sha;
        }
    }

    private static final Map<String, Known> known = new HashMap<>();

    /** The SHA-256 of xbe when it was hashed already and has not changed since (size, mtime); else null. No I/O beyond stat. */
    static String knownXbeSha(File xbe) {
        if (xbe == null)
            return null;
        synchronized (known) {
            Known k = known.get(xbe.getAbsolutePath());
            return k != null && k.size == xbe.length() && k.mtime == xbe.lastModified() ? k.sha : null;
        }
    }

    /** The SHA-256 of xbe (lower case), hashed at most once per change of the file; null when it cannot be read. */
    static String xbeSha(File xbe) {
        String s = knownXbeSha(xbe);
        if (s != null || xbe == null)
            return s;
        long size = xbe.length(), mtime = xbe.lastModified();
        try {
            s = GameStore.sha256(xbe).toLowerCase(Locale.ROOT);
        } catch (IOException e) {
            return null;
        }
        synchronized (known) {
            known.put(xbe.getAbsolutePath(), new Known(size, mtime, s));
        }
        return s;
    }

    /** Tests: forget the hashes. */
    static void forget() {
        synchronized (known) {
            known.clear();
        }
    }
}
