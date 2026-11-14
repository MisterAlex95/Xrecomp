package fr.adserver.xboxrecomp;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.net.Uri;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.cert.CertificateEncodingException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.BooleanSupplier;

/**
 * A game's own app takes over XRecomp's prepared pack (XBA-176, docs/android/prepare.md « Handing a pack to a game's
 * own app »), so that it does not prepare the game again.
 *
 * The game's app (the « clone ») starts PackHandoverActivity of its parent (manifest meta-data PARENT) for a result,
 * with its title ID. The parent ({@link #serve}) answers only a caller whose package is fr.adserver.xboxrecomp.t<tid>
 * for that very title ID and whose signing certificate is the one of the game apps' key in its AndroidKeyStore
 * (TitleApp.KEY_ALIAS): Android itself says who the caller is (getCallingPackage) and how it is signed
 * (GET_SIGNING_CERTIFICATES). It then grants read access to its pack's files, one URI each (pack.json, the library,
 * the disc's other images), through PackProvider, which is not exported: nothing else can read them.
 *
 * The clone ({@link #take}) reads pack.json, checks it fits (format, title_id, abi, shell_abi, lib name: Pack.check's
 * and tools/prepare/pack.py's checks; the disc it is locked to), the free space, copies the files into
 * packs/<TID>.partial/ while hashing the library (lib_sha256, lib_size), writes pack.json last, then publishes the
 * directory as pack.py does (an existing pack goes to <TID>.old, removed after the switch).
 */
final class PackHandover {
    private PackHandover() {}

    static final String ACTION = "fr.adserver.xboxrecomp.PACK_HANDOVER";
    /** The parent's activity: component class names stay fr.adserver.xboxrecomp.* in a generated app. */
    static final String ACTIVITY = "fr.adserver.xboxrecomp.PackHandoverActivity";
    static final String EXTRA_TITLE_ID = "fr.adserver.xboxrecomp.extra.HANDOVER_TITLE_ID",
                        EXTRA_FILES = "fr.adserver.xboxrecomp.extra.HANDOVER_FILES",
                        EXTRA_SIZES = "fr.adserver.xboxrecomp.extra.HANDOVER_SIZES",
                        EXTRA_REFUSAL = "fr.adserver.xboxrecomp.extra.HANDOVER_REFUSAL",
                        EXTRA_GAME_KIND = "fr.adserver.xboxrecomp.extra.HANDOVER_GAME_KIND",
                        EXTRA_GAME_PATH = "fr.adserver.xboxrecomp.extra.HANDOVER_GAME_PATH";
    /** Free space kept beyond the pack's size while copying. */
    static final long SPACE_MARGIN = 32L << 20;
    /** At most this many files in a pack (pack.json, the library, RECOMP_PACK_IMAGES_MAX = 8 images). */
    static final int MAX_FILES = 10;

    /** Why the parent refused. */
    enum Refusal { NOT_A_PARENT, NO_CALLER, BAD_TITLE, WRONG_PACKAGE, NO_KEY, WRONG_CERT, NO_PACK }

    // ---- The parent's side -----------------------------------------------------------------------------------------

    /** Null when caller may have tid's pack, else why not. No file is looked at. */
    static Refusal checkCaller(Context c, String caller, String tid) {
        if (TitleApp.ownTitleId(c) != null)
            return Refusal.NOT_A_PARENT;   // a game's own app never hands its pack on
        if (caller == null || caller.isEmpty())
            return Refusal.NO_CALLER;      // not started for a result (adb shell am start, a plain startActivity)
        String id = Pack.normTitleId(tid);
        if (id == null)
            return Refusal.BAD_TITLE;
        if (!TitleApk.packageName(id).equals(caller))
            return Refusal.WRONG_PACKAGE;
        X509Certificate cert = TitleApp.existingCert();
        if (cert == null)
            return Refusal.NO_KEY;
        return signedBy(c.getPackageManager(), caller, cert) ? null : Refusal.WRONG_CERT;
    }

    /** pkg is signed by cert alone (one signer, no other certificate in its signing history). */
    static boolean signedBy(PackageManager pm, String pkg, X509Certificate cert) {
        byte[] want;
        try {
            want = cert.getEncoded();
        } catch (CertificateEncodingException e) {
            return false;
        }
        try {
            PackageInfo pi = pm.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
            SigningInfo si = pi.signingInfo;
            if (si == null || si.hasMultipleSigners())
                return false;
            Signature[] s = si.getApkContentsSigners();
            return s != null && s.length == 1 && Arrays.equals(s[0].toByteArray(), want);
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            return false;
        }
    }

    /** The parent's answer: the result code and its data. */
    static final class Answer {
        final int code;
        final Intent data;
        final Refusal refusal;

        Answer(int code, Intent data, Refusal refusal) {
            this.code = code;
            this.data = data;
            this.refusal = refusal;
        }
    }

    static Answer refuse(Refusal r) {
        return new Answer(Activity.RESULT_CANCELED, new Intent().putExtra(EXTRA_REFUSAL, r.name()), r);
    }

    // ---- Where the game's files are (a game's own app starting straight into the game) ----------------------------

    /**
     * Where the parent reads the game from, told to a checked caller with its pack (or with its NO_PACK refusal), so
     * that a game's own app reads the same files in place instead of asking the player to choose them again: FOLDER
     * (a folder read in place: its path), IMAGE (a disc image read in place by path), or NOT_SHARED (the parent's own
     * imported copy in its private storage, or an image it reaches only through a document grant: the game's app
     * cannot read either, and asks the player once).
     */
    enum GameKind { FOLDER, IMAGE, NOT_SHARED }

    /** A game source: its kind and, for FOLDER and IMAGE, an absolute path. */
    static final class GameSource {
        final GameKind kind;
        final String path;

        GameSource(GameKind kind, String path) {
            this.kind = kind;
            this.path = path == null ? "" : path;
        }

        @Override
        public String toString() {
            return kind + (path.isEmpty() ? "" : " " + path);
        }
    }

    /**
     * The parent's source of tid's files: its library record (games.json under filesDir), else the one-game
     * preference (GameStore.chosenDir, when its default.xbe is tid's); null when it knows none. privateRoots: the
     * parent's own storage, never handed out as a path (NOT_SHARED instead).
     */
    static GameSource gameSource(Context c, File filesDir, String tid, File... privateRoots) {
        String id = Pack.normTitleId(tid);
        if (id == null)
            return null;
        Library.Record r = Library.read(Library.gamesFile(filesDir)).get(id);
        String dir, kind;
        if (r != null) {
            dir = r.dataDir;
            kind = r.dataKind;
            if (dir.isEmpty()) {   // a library game not imported: its file in the library folder, read as a path
                File f = LibraryIndex.locFile(r.source);
                return f == null ? null : source(f.isDirectory() ? GameKind.FOLDER : GameKind.IMAGE,
                                                 f.getAbsolutePath(), privateRoots);
            }
        } else {
            dir = c == null ? null : GameStore.chosenDir(c);
            if (dir == null || !id.equals(Pack.xbeTitleId(GameStore.findXbe(new File(dir)))))
                return null;
            kind = DiscImages.isCompanion(dir) ? Library.KIND_IMAGE : "";
        }
        if (Library.KIND_IMPORTED.equals(kind))
            return new GameSource(GameKind.NOT_SHARED, "");
        if (Library.KIND_IMAGE.equals(kind)) {
            DiscImages.Disc d = DiscImages.read(new File(dir));
            if (d == null || !d.viaPath())
                return new GameSource(GameKind.NOT_SHARED, "");
            return source(GameKind.IMAGE, d.path, privateRoots);
        }
        return source(GameKind.FOLDER, dir, privateRoots);
    }

    private static GameSource source(GameKind kind, String path, File... privateRoots) {
        if (!safePath(path))
            return null;
        for (File root : privateRoots) {
            if (root != null && under(path, root.getAbsolutePath()))
                return new GameSource(GameKind.NOT_SHARED, "");
        }
        return new GameSource(kind, path);
    }

    private static boolean under(String path, String root) {
        return path.equals(root) || path.startsWith(root.endsWith("/") ? root : root + "/");
    }

    /** An absolute path without « . » or « .. » parts, of a sane length. */
    static boolean safePath(String p) {
        if (p == null || p.length() < 2 || p.length() > 4096 || !p.startsWith("/") || p.indexOf(0) >= 0)
            return false;
        for (String part : p.split("/")) {
            if (part.equals(".") || part.equals(".."))
                return false;
        }
        return true;
    }

    /** Puts the source on an answer's data (nothing for null). */
    static void putSource(Intent data, GameSource s) {
        if (s == null)
            return;
        data.putExtra(EXTRA_GAME_KIND, s.kind.name());
        if (!s.path.isEmpty())
            data.putExtra(EXTRA_GAME_PATH, s.path);
    }

    /** The clone's side: the source in the parent's answer, or null when there is none or it is malformed. */
    static GameSource sourceOf(Intent data) {
        String k = data == null ? null : data.getStringExtra(EXTRA_GAME_KIND);
        if (k == null)
            return null;
        GameKind kind;
        try {
            kind = GameKind.valueOf(k);
        } catch (IllegalArgumentException e) {
            return null;
        }
        if (kind == GameKind.NOT_SHARED)
            return new GameSource(kind, "");
        String p = data.getStringExtra(EXTRA_GAME_PATH);
        return safePath(p) ? new GameSource(kind, p) : null;
    }

    /** The parent's private storage: its data directory, Android/data/<package>, the pack provider's directory. */
    static File[] privateRoots(Context c) {
        File data = c.getFilesDir().getParentFile(), ext = GameStore.externalFilesDir(c);
        File extPkg = ext == null ? null : ext.getParentFile();
        return new File[] { data, extPkg != null ? extPkg : ext, PackProvider.filesDir(c) };
    }

    /**
     * The parent's answer to request from caller: refused (EXTRA_REFUSAL), or RESULT_OK with the pack's file names
     * and sizes (pack.json first) and one content URI each in the ClipData, read access granted.
     */
    static Answer serve(Context c, String caller, Intent request) {
        String tid = request == null ? null : request.getStringExtra(EXTRA_TITLE_ID);
        Refusal r = checkCaller(c, caller, tid);
        if (r != null)
            return refuse(r);
        String id = Pack.normTitleId(tid);
        File filesDir = PackProvider.filesDir(c);
        // The caller is checked: it is told where the game's files are, with the pack or without it.
        GameSource src = gameSource(c, filesDir, id, privateRoots(c));
        List<String> names = Pack.check(filesDir, id) == Pack.Check.VALID ? servableNames(Pack.packDir(filesDir, id))
                                                                          : null;
        if (names == null) {
            Answer a = refuse(Refusal.NO_PACK);
            putSource(a.data, src);
            return a;
        }
        long[] sizes = new long[names.size()];
        ClipData clip = null;
        for (int i = 0; i < names.size(); i++) {
            sizes[i] = new File(Pack.packDir(filesDir, id), names.get(i)).length();
            Uri u = PackProvider.uriFor(c, id, names.get(i));
            if (clip == null)
                clip = ClipData.newRawUri("pack", u);
            else
                clip.addItem(new ClipData.Item(u));
        }
        Intent data = new Intent().putExtra(EXTRA_TITLE_ID, id)
                .putExtra(EXTRA_FILES, names.toArray(new String[0])).putExtra(EXTRA_SIZES, sizes);
        data.setClipData(clip);
        data.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        putSource(data, src);
        return new Answer(Activity.RESULT_OK, data, null);
    }

    /**
     * The files of the pack in dir that may be handed over: pack.json first, its lib, its images (pack.json
     * "images"), each a plain name of a file there. Null when pack.json cannot be read or names a missing file.
     */
    static List<String> servableNames(File dir) {
        JSONObject o;
        try {
            o = new JSONObject(PrepareProtocol.readText(new File(dir, Pack.JSON), 1 << 20));
        } catch (IOException | JSONException e) {
            return null;
        }
        List<String> out = new ArrayList<>();
        out.add(Pack.JSON);
        String lib = o.optString("lib", "");
        if (!plainName(lib) || !new File(dir, lib).isFile())
            return null;
        out.add(lib);
        JSONObject images = o.optJSONObject("images");
        if (images != null) {
            for (Iterator<String> it = images.keys(); it.hasNext();) {
                String n = images.optString(it.next(), "");
                if (!plainName(n) || !new File(dir, n).isFile())
                    return null;
                if (!out.contains(n))
                    out.add(n);
            }
        }
        return out.size() <= MAX_FILES ? out : null;
    }

    /** A file name inside a pack directory: no separator, not . or .., not pack.json's temporary file. */
    static boolean plainName(String n) {
        return n != null && n.matches("[A-Za-z0-9._-]{1,64}") && !n.equals(".") && !n.equals("..")
               && !n.endsWith(".tmp");
    }

    // ---- The clone's side ------------------------------------------------------------------------------------------

    /** What the parent offers: the title, the file names (pack.json first), their sizes and URIs. */
    static final class Offer {
        String titleId;
        final Map<String, Uri> uris = new LinkedHashMap<>();
        final Map<String, Long> sizes = new LinkedHashMap<>();

        long total() {
            long t = 0;
            for (long s : sizes.values())
                t += Math.max(0, s);
            return t;
        }
    }

    /**
     * The parent's RESULT_OK data as an offer for tid, or null when it is not one: another title, not pack.json first,
     * a name that is not plain, a URI that is not the parent's pack provider's for that very file.
     */
    static Offer offer(Intent data, String parent, String tid) {
        String id = Pack.normTitleId(tid);
        if (data == null || id == null || parent == null || !id.equals(data.getStringExtra(EXTRA_TITLE_ID)))
            return null;
        String[] names = data.getStringArrayExtra(EXTRA_FILES);
        long[] sizes = data.getLongArrayExtra(EXTRA_SIZES);
        ClipData clip = data.getClipData();
        if (names == null || sizes == null || clip == null || names.length < 2 || names.length > MAX_FILES
            || sizes.length != names.length || clip.getItemCount() != names.length || !Pack.JSON.equals(names[0]))
            return null;
        Offer o = new Offer();
        o.titleId = id;
        String authority = parent + PackProvider.AUTHORITY_SUFFIX;
        for (int i = 0; i < names.length; i++) {
            Uri u = clip.getItemAt(i).getUri();
            if (!plainName(names[i]) || u == null || !"content".equals(u.getScheme())
                || !authority.equals(u.getAuthority()) || !Arrays.asList(id, names[i]).equals(u.getPathSegments())
                || o.uris.containsKey(names[i]))
                return null;
            o.uris.put(names[i], u);
            o.sizes.put(names[i], sizes[i]);
        }
        return o;
    }

    /** Where the clone reads a file of the offer from (the content resolver; plain files in tests). */
    interface Source {
        InputStream open(String name) throws IOException;
    }

    interface Progress {
        void update(long done, long total);
    }

    /** How a take ended. */
    enum Outcome {
        /** Copied, checked and published. */
        COPIED,
        /** The clone's own pack is valid and the parent's is not newer for this app: kept as it is. */
        NOT_NEWER,
        /** The parent's pack does not fit this app (another format, abi, shell ABI or title). */
        UNFIT,
        /** Built from another default.xbe than the one the clone is locked to. */
        OTHER_XBE,
        NO_SPACE,
        /** A file had another size or the library another SHA-256 than pack.json says (or the parent's offer). */
        DAMAGED,
        FAILED,
        CANCELLED
    }

    static final class Result {
        final Outcome outcome;
        final String detail;
        /** pack.json of the parent's pack (null when it could not be read). */
        final JSONObject meta;
        /** The pack copied was lifted by another toolkit than this app's (it plays; Prepare again is offered). */
        final boolean olderToolkit;

        Result(Outcome outcome, String detail, JSONObject meta, boolean olderToolkit) {
            this.outcome = outcome;
            this.detail = detail == null ? "" : detail;
            this.meta = meta;
            this.olderToolkit = olderToolkit;
        }
    }

    /**
     * Whether the clone should ask its parent for tid's pack: it has no valid pack, or a valid one lifted by another
     * toolkit than its own (appToolkit, Toolchain.toolkitDigest).
     */
    static boolean wanted(File filesDir, String tid, String appToolkit) {
        String id = Pack.normTitleId(tid);
        if (id == null)
            return false;
        if (Pack.check(filesDir, id) != Pack.Check.VALID)
            return true;
        return !current(Pack.toolkitSha256(filesDir, id), appToolkit);
    }

    /** A pack lifted with packToolkit is current for an app whose toolkit is appToolkit (unknown: current). */
    static boolean current(String packToolkit, String appToolkit) {
        return PackFreshness.judge(null, null, packToolkit, appToolkit) == PackFreshness.Verdict.CURRENT;
    }

    /** Why meta does not fit this app for id, or null when it does (Pack.check's and pack.py's checks). */
    static String unfit(JSONObject meta, String id, Offer offer) {
        if (meta.optInt("format", -1) != Pack.FORMAT)
            return "format " + meta.opt("format");
        if (!Pack.sameTitleId(meta.optString("title_id", ""), id))
            return "title_id " + meta.optString("title_id", "");
        if (!"arm64-v8a".equals(meta.optString("abi", "")))
            return "abi " + meta.optString("abi", "");
        if (meta.optInt("shell_abi", -1) != Pack.SHELL_ABI)
            return "shell_abi " + meta.opt("shell_abi") + ", this app is " + Pack.SHELL_ABI;
        String lib = meta.optString("lib", "");
        if (!plainName(lib) || !offer.uris.containsKey(lib))
            return "lib " + lib;
        if (meta.optLong("lib_size", -1) <= 0)
            return "lib_size";
        if (!LibraryScreen.isSha256(meta.optString("lib_sha256", "")))
            return "lib_sha256";
        JSONObject images = meta.optJSONObject("images");
        if (images != null) {
            for (Iterator<String> it = images.keys(); it.hasNext();) {
                String n = images.optString(it.next(), "");
                if (!plainName(n) || !offer.uris.containsKey(n))
                    return "image " + n;
            }
        }
        return null;
    }

    /**
     * Takes the offered pack into filesDir/packs/<TID> (see the class comment). lockedXbe: the default.xbe SHA-256
     * the clone is locked to ("" for none); appToolkit: its toolkit digest ("" unknown).
     */
    static Result take(Source src, Offer offer, File filesDir, String lockedXbe, String appToolkit, Progress p,
                       BooleanSupplier cancelled) {
        String id = offer.titleId;
        JSONObject meta;
        byte[] json;
        try (InputStream in = src.open(Pack.JSON)) {
            json = readAll(in, 1 << 20);
            meta = new JSONObject(new String(json, StandardCharsets.UTF_8));
        } catch (IOException | JSONException e) {
            return new Result(Outcome.DAMAGED, "pack.json: " + e.getMessage(), null, false);
        }
        String why = unfit(meta, id, offer);
        if (why != null)
            return new Result(Outcome.UNFIT, why, meta, false);
        String packXbe = meta.optString("xbe_sha256", "");
        if (LibraryScreen.isSha256(lockedXbe) && LibraryScreen.isSha256(packXbe) && !packXbe.equalsIgnoreCase(lockedXbe))
            return new Result(Outcome.OTHER_XBE, packXbe, meta, false);
        boolean parentCurrent = current(meta.optString("toolkit_sha256", ""), appToolkit);
        if (Pack.check(filesDir, id) == Pack.Check.VALID
            && (!parentCurrent || current(Pack.toolkitSha256(filesDir, id), appToolkit)))
            return new Result(Outcome.NOT_NEWER, "", meta, !parentCurrent);

        String lib = meta.optString("lib");
        long libSize = meta.optLong("lib_size");
        Long offered = offer.sizes.get(lib);
        if (offered != null && offered >= 0 && offered != libSize)
            return new Result(Outcome.DAMAGED, lib + ": " + offered + " bytes offered, pack.json says " + libSize,
                              meta, false);
        // Only what pack.json names is copied: its lib, then its images (an extra file offered is left out).
        List<String> names = new ArrayList<>();
        names.add(lib);
        JSONObject images = meta.optJSONObject("images");
        if (images != null) {
            for (Iterator<String> it = images.keys(); it.hasNext();) {
                String n = images.optString(it.next(), "");
                if (!names.contains(n))
                    names.add(n);
            }
        }
        long total = 0;
        for (String n : names)
            total += n.equals(lib) ? libSize : Math.max(0, offer.sizes.get(n));
        File packs = Pack.packsDir(filesDir);
        if (!(packs.mkdirs() || packs.isDirectory()))
            return new Result(Outcome.FAILED, "cannot create " + packs, meta, false);
        long free = packs.getUsableSpace();
        if (free < total + SPACE_MARGIN)
            return new Result(Outcome.NO_SPACE, String.valueOf(total + SPACE_MARGIN), meta, false);

        recover(filesDir, id);
        File part = new File(packs, id + ".partial");
        GameStore.deleteTree(part);
        if (!part.mkdirs())
            return new Result(Outcome.FAILED, "cannot create " + part, meta, false);
        try {
            long[] done = { 0 };
            for (String name : names) {
                long want = name.equals(lib) ? libSize : offer.sizes.get(name);
                String sha = copy(src, name, new File(part, name), done, total, p, cancelled);
                File f = new File(part, name);
                if (want >= 0 && f.length() != want)
                    throw new Damaged(name + ": " + f.length() + " bytes, expected " + want);
                if (name.equals(lib) && !sha.equalsIgnoreCase(meta.optString("lib_sha256")))
                    throw new Damaged(name + ": sha256 " + sha + ", pack.json says " + meta.optString("lib_sha256"));
            }
            // pack.json last: a temporary file, synced, renamed (a reader sees it whole or not at all).
            File tmp = new File(part, Pack.JSON + ".tmp");
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(json);
                out.getFD().sync();
            }
            if (!tmp.renameTo(new File(part, Pack.JSON)))
                throw new IOException("cannot rename " + tmp);
            publish(filesDir, id);
        } catch (Damaged e) {
            GameStore.deleteTree(part);
            return new Result(Outcome.DAMAGED, e.getMessage(), meta, false);
        } catch (InterruptedIOException e) {
            GameStore.deleteTree(part);
            return new Result(Outcome.CANCELLED, "", meta, false);
        } catch (IOException | RuntimeException e) {
            GameStore.deleteTree(part);
            return new Result(Outcome.FAILED, String.valueOf(e.getMessage()), meta, false);
        }
        Pack.Check ck = Pack.check(filesDir, id);
        if (ck != Pack.Check.VALID)
            return new Result(Outcome.FAILED, "published pack: " + ck, meta, false);
        return new Result(Outcome.COPIED, "", meta, !parentCurrent);
    }

    private static final class Damaged extends IOException {
        Damaged(String m) {
            super(m);
        }
    }

    /** Copies name from src to dst (synced), adding to done; returns its SHA-256 in lower case. */
    private static String copy(Source src, String name, File dst, long[] done, long total, Progress p,
                               BooleanSupplier cancelled) throws IOException {
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        try (InputStream in = src.open(name); FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) {
                if (cancelled != null && cancelled.getAsBoolean())
                    throw new InterruptedIOException("cancelled");
                out.write(buf, 0, n);
                md.update(buf, 0, n);
                done[0] += n;
                if (p != null)
                    p.update(done[0], total);
            }
            out.getFD().sync();
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest())
            sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    private static byte[] readAll(InputStream in, int max) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) {
            o.write(buf, 0, n);
            if (o.size() > max)
                throw new IOException("too large");
        }
        return o.toByteArray();
    }

    /** pack.py recover(): a publish cut between its two renames is undone; a leftover .old is removed. */
    static void recover(File filesDir, String id) {
        File packs = Pack.packsDir(filesDir), fin = new File(packs, id), old = new File(packs, id + ".old");
        if (old.isDirectory()) {
            if (!fin.exists() && old.renameTo(fin))
                return;
            GameStore.deleteTree(old);
        }
    }

    /** pack.py publish(): <TID>.partial becomes <TID>, an existing pack moved to <TID>.old first, removed after. */
    static void publish(File filesDir, String id) throws IOException {
        File packs = Pack.packsDir(filesDir), part = new File(packs, id + ".partial"), fin = new File(packs, id),
             old = new File(packs, id + ".old");
        GameStore.deleteTree(old);
        if (fin.exists() && !fin.renameTo(old))
            throw new IOException("cannot move " + fin + " aside");
        if (!part.renameTo(fin)) {
            if (old.isDirectory())
                //noinspection ResultOfMethodCallIgnored
                old.renameTo(fin);
            throw new IOException("cannot rename " + part);
        }
        GameStore.deleteTree(old);
        Log.i(LauncherActivity.TAG, "launcher: pack " + id + " published from the handover");
    }
}
