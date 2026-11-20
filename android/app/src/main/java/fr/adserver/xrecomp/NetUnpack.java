package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Enumeration;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Games shared as .zip archives (XBA-331: a Redump set is one .zip per disc, « Game (Europe) (Disc 1).zip » holding
 * the .iso). The network library lists such a .zip as one game (RemoteLibrary.games); once downloaded, NetRunner
 * unpacks it here into a folder beside it named after it (« Game (Europe) (Disc 1)/ »), then deletes the .zip: the
 * download folder's scan finds the image inside like any other. Plain Java (java.util.zip, Zip64 included), so the
 * JVM tests cover it.
 *
 * The unpack writes into a hidden sibling (« .<name>.unpacking », never scanned: its name starts with « . ») and
 * renames it when every entry is out, so the scan never sees half an image; the folder is a function of the .zip's
 * torrent path alone ({@link #dirPath}), so the launcher and the library find the game again without the job. An
 * entry's path is checked like a torrent path (NetDownloads.safePath) and at most {@link #MAX_DEPTH} deep (the
 * download folder's scan reaches LibraryScanner.NET_MAX_DEPTH, which counts it); an archive with no file a library
 * scan would list as a game is refused before anything is written ({@link #E_NO_GAME}).
 */
final class NetUnpack {
    private NetUnpack() {}

    /** Job errors: unpacking now (a progress, not a failure), no game inside, an archive java.util.zip cannot read. */
    static final String E_UNPACKING = "unpacking", E_NO_GAME = "no_game", E_BAD_ZIP = "bad_zip";
    /** Not an error: the player cancelled the download while it was unpacked (the temporary folder is deleted). */
    static final String CANCELLED = "cancelled";
    static final int MAX_DEPTH = 4, MAX_ENTRIES = 10_000;

    interface Cancel {
        boolean cancelled();
    }

    interface Progress {
        void at(long done, long total);
    }

    /** A game file the network library unpacks: a .zip (any case). */
    static boolean isArchive(String name) {
        return name.toLowerCase(Locale.ROOT).endsWith(".zip");
    }

    /** A job that downloads one archive. */
    static boolean isArchiveJob(NetDownloads.Job j) {
        return j.files.size() == 1 && isArchive(j.files.get(0).path);
    }

    /** The folder the archive at torrent path zipPath unpacks into, as a torrent path: beside it, without « .zip ». */
    static String dirPath(String zipPath) {
        int s = zipPath.lastIndexOf('/');
        String name = zipPath.substring(s + 1);
        return zipPath.substring(0, s + 1) + name.substring(0, name.length() - 4);
    }

    /** The hidden folder it is unpacked into first. */
    static File tempDir(File dir) {
        return new File(dir.getParentFile(), "." + dir.getName() + ".unpacking");
    }

    /** The archive of job j, its unpacked folder, or null (no base, an unsafe path, not an archive job). */
    static File zipOf(File downloadDir, NetDownloads.Job j) {
        return isArchiveJob(j) ? NetDownloads.jobFile(downloadDir, j, j.files.get(0).path) : null;
    }

    static File dirOf(File downloadDir, NetDownloads.Job j) {
        return isArchiveJob(j) ? NetDownloads.jobFile(downloadDir, j, dirPath(j.files.get(0).path)) : null;
    }

    /** A file name a library scan lists as a game (an image, a .cue, a ROM) or a game folder's executable. */
    static boolean isGameName(String name) {
        String n = name.toLowerCase(Locale.ROOT);
        return LibraryScanner.isImage(name) || LibraryScanner.isCue(name) || LibraryScanner.isRom(name)
               || n.equals("default.xbe");
    }

    /**
     * Unpacks zip into dir (through its hidden temporary folder). Returns null when dir holds the archive's files,
     * NetRunner.E_SPACE when they do not fit in free bytes, {@link #E_NO_GAME}, {@link #E_BAD_ZIP} or
     * {@link #CANCELLED} (nothing left behind but the archive in each case). Already unpacked (dir is there): null at
     * once. The archive itself is the caller's to delete.
     */
    static String unpack(File zip, File dir, long free, Cancel cancel, Progress progress) throws IOException {
        if (dir.isDirectory())
            return null;
        File tmp = tempDir(dir);
        deleteTree(tmp);
        try (ZipFile z = new ZipFile(zip)) {
            long total = 0;
            int count = 0;
            boolean game = false;
            for (Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements(); ) {
                ZipEntry en = e.nextElement();
                if (++count > MAX_ENTRIES) {
                    LibraryScanner.warn("net: " + zip + ": more than " + MAX_ENTRIES + " entries");
                    return E_BAD_ZIP;
                }
                if (en.isDirectory())
                    continue;
                String p = en.getName();
                if (!NetDownloads.safePath(p) || p.split("/").length > MAX_DEPTH) {
                    LibraryScanner.warn("net: " + zip + ": refused entry " + p);
                    return E_BAD_ZIP;
                }
                game |= isGameName(p.substring(p.lastIndexOf('/') + 1));
                total += Math.max(0, en.getSize() >= 0 ? en.getSize() : en.getCompressedSize());
            }
            if (!game)
                return E_NO_GAME;
            if (total > free)
                return NetRunner.E_SPACE;
            byte[] buf = new byte[1 << 20];
            long done = 0;
            for (Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements(); ) {
                ZipEntry en = e.nextElement();
                if (en.isDirectory())
                    continue;
                File out = new File(tmp, en.getName());
                File parent = out.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs())
                    throw new IOException("cannot create " + parent);
                try (InputStream in = z.getInputStream(en); OutputStream o = Files.newOutputStream(out.toPath())) {
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        o.write(buf, 0, n);
                        done += n;
                        if (cancel != null && cancel.cancelled()) {
                            o.close();
                            deleteTree(tmp);
                            return CANCELLED;
                        }
                        if (progress != null)
                            progress.at(done, Math.max(done, total));
                    }
                }
            }
        } catch (java.util.zip.ZipException e) {
            LibraryScanner.warn("net: cannot unpack " + zip + ": " + e);
            deleteTree(tmp);
            return E_BAD_ZIP;
        } catch (IOException | RuntimeException e) {
            deleteTree(tmp);
            throw e;
        }
        if (!tmp.isDirectory() && !tmp.mkdirs())
            throw new IOException("cannot create " + tmp);
        Files.move(tmp.toPath(), dir.toPath(), StandardCopyOption.ATOMIC_MOVE);
        return null;
    }

    /** Deletes a file or a folder and everything in it (symbolic links are deleted, never followed). */
    static void deleteTree(File f) {
        if (f == null || !f.exists() && !Files.isSymbolicLink(f.toPath()))
            return;
        if (f.isDirectory() && !Files.isSymbolicLink(f.toPath())) {
            File[] kids = f.listFiles();
            if (kids != null)
                for (File k : kids)
                    deleteTree(k);
        }
        if (!f.delete())
            LibraryScanner.warn("net: cannot delete " + f);
    }
}
