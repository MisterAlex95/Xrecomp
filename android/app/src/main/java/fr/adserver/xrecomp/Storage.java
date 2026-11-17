package fr.adserver.xrecomp;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;

/**
 * What the app stores per game, for the Storage screen and Game data (docs/android/app-shell.md « Game library »):
 * the imported copy, the pack, the Prepare's work files, the game's caches (live or stashed, SaveCaches) and its
 * saves. Each can be deleted on its own; saves are never part of « delete everything ».
 *
 * Only what the app made is ever deleted: an imported copy under the app's external files (games/<TID>, or the
 * older game/), never a folder read in place or anything in the player's library folder. Plain Java on Files.
 */
final class Storage {
    private Storage() {}

    /** IMAGE: a disc image read in place (XBA-126): only its companion folder is the app's (a few MB of XBEs). */
    enum Kind { DATA, IMAGE, PACK, WORK, CACHES, SAVES }

    /** The app's directories, so the JVM tests can point them at a temporary folder. */
    static final class Dirs {
        final File filesDir, externalDir;

        Dirs(File filesDir, File externalDir) {
            this.filesDir = filesDir;
            this.externalDir = externalDir;
        }

        File saveDir() {
            return new File(filesDir, "save");
        }

        File workDir(String tid) {
            return new File(new File(filesDir, "work"), tid);
        }

        File ownerFile() {
            return Library.cacheOwnerFile(filesDir);
        }
    }

    static final class Item {
        final String tid;
        final Kind kind;
        final List<File> files = new ArrayList<>();
        /** Bytes, -1 until measured (measure(): a worker thread, the imported copy can be gigabytes). */
        long bytes = -1;

        Item(String tid, Kind kind) {
            this.tid = tid;
            this.kind = kind;
        }

        boolean exists() {
            for (File f : files) {
                if (f.exists())
                    return true;
            }
            return false;
        }
    }

    /** True when dir is an imported copy the app owns: under <external files>/games/ or the older <external>/game. */
    static boolean ownedCopy(Dirs d, String dir) {
        if (dir == null || dir.isEmpty() || d.externalDir == null)
            return false;
        File f = new File(dir).getAbsoluteFile();
        File legacy = new File(d.externalDir, GameStore.GAME_SUBDIR).getAbsoluteFile();
        if (f.equals(legacy))
            return true;
        File p = f.getParentFile();
        return p != null && p.equals(new File(d.externalDir, Library.GAMES_SUBDIR).getAbsoluteFile())
               && !f.getName().startsWith(".");
    }

    /** True when dir is a disc image's companion folder: <files>/discs/<TID> holding disc.json (DiscImages). */
    static boolean companion(Dirs d, String dir) {
        if (dir == null || dir.isEmpty())
            return false;
        File f = new File(dir).getAbsoluteFile(), p = f.getParentFile();
        return p != null && p.equals(new File(d.filesDir, DiscImages.DISCS_SUBDIR).getAbsoluteFile())
               && !f.getName().startsWith(".") && new File(f, DiscImages.DISC_JSON).isFile();
    }

    /** The image a companion item stands for (its size on its storage, its name), or null. */
    static DiscImages.Disc disc(Item i) {
        return i.kind == Kind.IMAGE && !i.files.isEmpty() ? DiscImages.read(i.files.get(0)) : null;
    }

    /** The save folders of a title: UDATA/<tid> and TDATA/<tid> (the kernel names them in lower case). */
    static List<File> saves(Dirs d, String tid) {
        List<File> l = new ArrayList<>();
        if (Console.ofKey(tid) != Console.XBOX) {
            // A GBA, PS1, N64 or GameCube game (XBA-181, XBA-264): save/<console>-<KEY>/ (game.sav, card1.mcr,
            // game.eep, cardA.raw...).
            l.add(new File(d.saveDir(), tid));
            return l;
        }
        for (String area : new String[] { "UDATA", "TDATA" }) {
            File a = new File(d.saveDir(), area);
            String[] names = a.list();
            boolean any = false;
            if (names != null) {
                for (String n : names) {   // any case (one entry each, also on a case-insensitive file system)
                    if (n.equalsIgnoreCase(tid)) {
                        l.add(new File(a, n));
                        any = true;
                    }
                }
            }
            if (!any)
                l.add(new File(a, tid.toLowerCase(Locale.ROOT)));
        }
        return l;
    }

    /** The caches of tid: the live set when it owns it (SaveCaches), else its stash. */
    static List<File> caches(Dirs d, String tid) {
        List<File> l = new ArrayList<>();
        if (tid.equals(SaveCaches.owner(d.ownerFile()))) {
            for (String n : SaveCaches.LIVE)
                l.add(new File(d.saveDir(), n));
        } else {
            l.add(SaveCaches.stashDir(d.saveDir(), tid));
        }
        return l;
    }

    /** What exists for the title of record r (r may be null: a title known only by its pack or saves). */
    static List<Item> items(Dirs d, String tid, Library.Record r) {
        List<Item> out = new ArrayList<>();
        if (r != null && ownedCopy(d, r.dataDir) && Library.KIND_IMPORTED.equals(r.dataKind)) {
            Item i = new Item(tid, Kind.DATA);
            i.files.add(new File(r.dataDir));
            out.add(i);
        }
        if (r != null && Library.KIND_IMAGE.equals(r.dataKind) && companion(d, r.dataDir)) {
            Item i = new Item(tid, Kind.IMAGE);
            i.files.add(new File(r.dataDir));
            out.add(i);
        }
        if (!Library.isLocalKey(tid) && SaveCaches.safeName(tid)) {
            Item pack = new Item(tid, Kind.PACK);
            pack.files.add(Pack.packDir(d.filesDir, tid));
            out.add(pack);
            Item work = new Item(tid, Kind.WORK);
            work.files.add(d.workDir(tid));
            out.add(work);
        }
        if (SaveCaches.safeName(tid)) {
            Item c = new Item(tid, Kind.CACHES);
            c.files.addAll(caches(d, tid));
            out.add(c);
            Item s = new Item(tid, Kind.SAVES);
            s.files.addAll(saves(d, tid));
            out.add(s);
        }
        out.removeIf(i -> !i.exists());
        return out;
    }

    /**
     * Every title the Storage screen lists: the records, plus any title with a pack, a Prepare work folder, stashed
     * caches or saves left behind (a game that left the library). Sorted.
     */
    static Set<String> titles(Dirs d, Library.Games g) {
        Set<String> s = new TreeSet<>(g.games.keySet());
        addNames(s, Pack.packsDir(d.filesDir));
        addNames(s, new File(d.filesDir, "work"));
        addNames(s, new File(d.saveDir(), SaveCaches.STASH));
        for (String area : new String[] { "UDATA", "TDATA" }) {
            String[] l = new File(d.saveDir(), area).list();
            if (l != null) {
                for (String n : l) {
                    String t = Pack.normTitleId(n);
                    if (t != null)
                        s.add(t);
                }
            }
        }
        String[] consoleSaves = d.saveDir().list();   // save/gba-<KEY>/, ps1-<KEY>/, n64-<KEY>/
        if (consoleSaves != null) {
            for (String n : consoleSaves) {
                if (Console.ofKey(n) != Console.XBOX && new File(d.saveDir(), n).isDirectory())
                    s.add(n);
            }
        }
        String owner = SaveCaches.owner(d.ownerFile());
        if (owner != null)
            s.add(owner);
        s.removeIf(t -> !SaveCaches.safeName(t));
        return s;
    }

    private static void addNames(Set<String> s, File dir) {
        String[] l = dir.list();
        if (l == null)
            return;
        for (String n : l) {
            if (!n.startsWith(".") && SaveCaches.safeName(n)) {
                String t = Pack.normTitleId(n);
                s.add(t != null ? t : n);
            }
        }
    }

    static long measure(Item i) {
        long n = 0;
        for (File f : i.files)
            n += GameStore.treeSize(f);
        i.bytes = n;
        return n;
    }

    /**
     * Deletes item i (the caller ended a running game first). The imported copy also leaves its record's data_dir
     * (the record stays: play time, the folder it came from). Never anything outside the app's own storage.
     */
    static void delete(Dirs d, Item i, Library.Games g) {
        for (File f : i.files) {
            if (!inside(f, d.filesDir) && !(i.kind == Kind.DATA && ownedCopy(d, f.getPath())))
                continue;   // defensive: never outside the app's storage
            GameStore.deleteTree(f);
        }
        if ((i.kind == Kind.DATA || i.kind == Kind.IMAGE) && g != null) {
            Library.Record r = g.get(i.tid);
            if (r != null && !i.files.isEmpty() && i.files.get(0).getPath().equals(r.dataDir)) {
                r.dataDir = "";
                r.dataKind = "";
            }
        }
    }

    static boolean inside(File f, File root) {
        if (f == null || root == null)
            return false;
        String a = f.getAbsolutePath(), b = root.getAbsolutePath();
        return a.startsWith(b + File.separator);
    }
}
