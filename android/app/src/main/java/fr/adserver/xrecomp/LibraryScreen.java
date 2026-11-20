package fr.adserver.xrecomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The library screen's model and grid (docs/android/app-shell.md « Game library »): the launcher's records
 * (Library.Games) joined with the scan index (LibraryIndex) into one Entry per game key (an Xbox title ID, or
 * « <console>-<title key> » for a game of another console, XBA-178), each with its GameState chip,
 * and the scrolling full-width 5-column grid of GameCards. LauncherActivity builds the rest of the screen (the context
 * line, the Options sheet, hints) with its own helpers; this class keeps the library's logic out of it.
 */
final class LibraryScreen {
    private LibraryScreen() {}

    /** The grid's columns on a 16:9 stage (and the most it has). */
    static final int COLUMNS = 5;
    /** The fewest (a square screen), and the least room between two arts. */
    static final int MIN_COLUMNS = 3, MIN_GAP = Theme.S3;

    /** The columns for a grid avail design pixels wide: as many arts as fit with MIN_GAP, MIN_COLUMNS to COLUMNS. */
    static int columns(int avail) {
        int n = (avail + MIN_GAP) / (GameCard.ART + MIN_GAP);
        return Math.max(MIN_COLUMNS, Math.min(COLUMNS, n));
    }
    /** A « running » status this fresh counts as a live Prepare (LauncherActivity.PREPARE_FRESH_MS). */
    static final long PREPARE_FRESH_MS = 15000;

    /** What the facts of every entry share: the toolchain, the folder's access, the running game, the queue. */
    static final class Env {
        boolean toolchain, accessible, prepareProcess;
        /**
         * The library folders the app cannot read now (their tree URIs; "" too when it is the first folder, which
         * owns the items of an older index): their games show « No file access ».
         */
        java.util.Set<String> noAccess = Collections.emptySet();
        String runningTid, importingTid;
        /** This APK's toolkit digest (Toolchain.toolkitDigest, cached), "" when unknown: packs from another are out of date. */
        String toolkit = "";
        PrepareProtocol.Queue queue = new PrepareProtocol.Queue();
        long now = System.currentTimeMillis();
        /** Title IDs whose pack the shell refused this session. */
        java.util.Set<String> packDropped = Collections.emptySet();
        /** XBA-319: the network library's games not on the device (RemoteLibrary.notLocal), by key. */
        Map<String, RemoteLibrary.Game> remote = Collections.emptyMap();
        /** XBA-321: the downloads (NetDownloads), by key. */
        Map<String, NetDownloads.Job> downloads = Collections.emptyMap();

        /** The folder of item i can be read (accessible, and not one of noAccess). */
        boolean accessible(LibraryIndex.Item i) {
            return accessible && (i == null || !noAccess.contains(i.folder));
        }
    }

    /** One game of the library: its record (may be null), its variants in the folder (may be empty), its state. */
    static final class Entry {
        final String tid;
        Library.Record record;
        List<LibraryIndex.Item> variants = Collections.emptyList();
        LibraryIndex.Item active;
        GameState.Facts facts;
        GameState.Chip chip;
        /** How prepared a Ready game is (GameState.prepared): the card's label then. */
        GameState.Prepared prepared = GameState.Prepared.NONE;
        /** 1 for the next Prepare to run, 0 when not queued. */
        int queuePos;
        /** XBA-258: whether the game resumes where the player left off (Pack.resume), read for a Ready game only. */
        Pack.Resume resume = Pack.Resume.NO;
        /** XBA-319: a game of the network library not on the device (then no record, no variant), else null. */
        RemoteLibrary.Game remote;
        /** XBA-321: its download job (any state), or null. */
        NetDownloads.Job download;
        /** A prepared game's runs met this much code its preparation missed (its card says « +N new »). */
        int found;

        Entry(String tid) {
            this.tid = tid;
        }

        String title() {
            if (record != null && !record.title.isEmpty())
                return record.title;
            if (active != null && !active.title.isEmpty())
                return active.title;
            if (remote != null)
                return remote.title;
            return tid;
        }

        boolean hasData() {
            return facts != null && facts.hasData;
        }

        /** The default.xbe to read for art and theme: the game's files, else the scan's cached copy; null if none. */
        File xbe(File filesDir) {
            if (record != null && !record.dataDir.isEmpty()) {
                File x = GameStore.findXbe(new File(record.dataDir));
                if (x != null)
                    return x;
            }
            return active == null ? null : active.xbeFile(filesDir);
        }

        /** The game's console, from its key. */
        Console console() {
            return Console.ofKey(tid);
        }

        /**
         * A game of another console than the Xbox: no Xbox Prepare or Home; « Not playable yet » unless its console
         * plays files as they are (the GBA, XBA-157, and the PS1, XBA-168: A or Start launches it).
         */
        boolean otherConsole() {
            return console() != Console.XBOX;
        }

        /** A game started from its file as it is (a GBA ROM, a PS1 disc): A or Start on its card launches it. */
        boolean directLaunch() {
            return console().directLaunch();
        }

        /**
         * Title IDs are 8 hex digits; a « local-… » key has none, nor a game of another console (no Prepare, no
         * theme, no XBE art).
         */
        boolean realTid() {
            return !Library.isLocalKey(tid) && !RemoteLibrary.isRemoteKey(tid) && !otherConsole();
        }
    }

    /** True when s looks like a SHA-256 (64 hex digits): a pack.json without one is « unknown », not a mismatch. */
    static boolean isSha256(String s) {
        if (s == null || s.length() != 64)
            return false;
        for (int i = 0; i < 64; i++) {
            char c = Character.toLowerCase(s.charAt(i));
            if (!(c >= '0' && c <= '9' || c >= 'a' && c <= 'f'))
                return false;
        }
        return true;
    }

    /** The pack of tid was built from the XBE whose hash is sha, as far as both sides say. */
    static boolean packMatches(File filesDir, String tid, String sha) {
        String h = Pack.xbeSha256(filesDir, tid);
        return !isSha256(h) || !isSha256(sha) || h.equalsIgnoreCase(sha);
    }

    /**
     * The library's entries: every title ID of the records and of the index's games, with its chip, those that show
     * in the grid only (GameState.inGrid), sorted by title.
     */
    static List<Entry> entries(File filesDir, Library.Games g, LibraryIndex x, Env env) {
        Map<String, Entry> m = new LinkedHashMap<>();
        Map<String, List<LibraryIndex.Item>> games = x == null ? new HashMap<>() : x.games();
        for (Map.Entry<String, List<LibraryIndex.Item>> e : games.entrySet()) {
            Entry en = new Entry(e.getKey());
            en.variants = e.getValue();
            m.put(e.getKey(), en);
        }
        for (RemoteLibrary.Game rg : env.remote.values()) {   // XBA-319: games of the network library
            if (m.containsKey(rg.key))
                continue;
            Entry en = new Entry(rg.key);
            en.remote = rg;
            m.put(rg.key, en);
        }
        for (Library.Record r : g.games.values()) {
            if (RemoteLibrary.isRemoteKey(r.titleId))
                continue;   // never a record for a remote key (none is written: an older build's, say)
            Entry en = m.get(r.titleId);
            if (en == null) {
                en = new Entry(r.titleId);
                m.put(r.titleId, en);
            }
            en.record = r;
        }
        List<Entry> out = new ArrayList<>();
        for (Entry en : m.values()) {
            fill(filesDir, en, env);
            if (GameState.inGrid(en.chip))
                out.add(en);
        }
        out.sort((a, b) -> {
            int c = a.title().compareToIgnoreCase(b.title());
            return c != 0 ? c : a.tid.compareTo(b.tid);
        });
        return out;
    }

    /** One entry, for a title not in the grid's list (Home of the selected game). */
    static Entry entry(File filesDir, String tid, Library.Games g, LibraryIndex x, Env env) {
        Entry en = new Entry(tid);
        en.record = g.get(tid);
        List<LibraryIndex.Item> v = x == null ? null : x.games().get(tid);
        if (v != null)
            en.variants = v;
        if (RemoteLibrary.isRemoteKey(tid)) {
            en.record = null;
            en.remote = env.remote.get(tid);
        }
        fill(filesDir, en, env);
        return en;
    }

    /** The active variant, the facts and the chip of en (reads the pack and the Prepare status: small files). */
    static void fill(File filesDir, Entry en, Env env) {
        if (RemoteLibrary.isRemoteKey(en.tid)) {   // XBA-319: nothing of it on the device: no pack, no Prepare
            GameState.Facts f = new GameState.Facts();
            f.titleId = en.tid;
            f.console = en.console();
            en.download = env.downloads.get(en.tid);
            f.source = en.remote != null ? GameState.Source.REMOTE : GameState.Source.MISSING;
            f.downloading = en.download != null && en.download.active();
            en.facts = f;
            en.chip = GameState.chip(f);
            en.prepared = GameState.Prepared.NONE;
            en.resume = Pack.Resume.NO;
            return;
        }
        en.active = LibraryIndex.active(en.variants, en.record == null ? null : en.record.source);
        GameState.Facts f = new GameState.Facts();
        f.titleId = en.tid;
        f.console = en.console();
        f.dataKind = en.record == null ? "" : en.record.dataKind;
        boolean recordData = en.record != null && en.record.hasData();
        f.hasData = recordData;
        boolean accessible = env.accessible(en.active);
        f.source = LibraryScanner.source(en.active, accessible);
        // A game folder of the library read as files is played in place: its files are here, record or not.
        File inPlace = inPlaceDir(en.active);
        if (!f.hasData && inPlace != null && accessible && (en.record == null || en.record.dataDir.isEmpty())
            && GameStore.findXbe(inPlace) != null) {
            f.hasData = true;
            f.dataKind = Library.KIND_IN_PLACE;
        }
        // A GBA game (XBA-158), a PS1 disc (XBA-168) and an N64 cartridge (XBA-174) have a Prepare and a pack too,
        // keyed by their library key (packs/gba-<KEY>, packs/ps1-<KEY>, packs/n64-<KEY>).
        boolean prepares = en.realTid() || (en.directLaunch() && en.console().prepares());
        f.toolchain = env.toolchain && prepares;
        if (prepares) {
            f.pack = Pack.check(filesDir, en.tid);
            if (en.realTid()) {
                String sha = recordData ? en.record.xbeSha256 : en.active != null ? en.active.xbeSha256 : "";
                f.packMatches = packMatches(filesDir, en.tid, sha);
            }
            // Out of date: a valid pack lifted by an older toolkit (pack.json is small; the APK's digest is cached).
            if (f.pack == Pack.Check.VALID && isSha256(env.toolkit)) {
                String packToolkit = en.realTid() ? Pack.toolkitSha256(filesDir, en.tid)
                                                  : Pack.toolkitSha256OfKey(filesDir, en.tid);
                f.packOlderToolkit = PackFreshness.judge(null, null, packToolkit, env.toolkit)
                                     == PackFreshness.Verdict.OLDER_TOOLKIT;
            }
            f.prepare = PrepareProtocol.readStatus(new File(new File(new File(filesDir, "prepare"), en.tid),
                                                            "status.json"));
            f.prepareFresh = f.prepare != null && env.now - f.prepare.updatedAt < PREPARE_FRESH_MS;
        }
        f.packDropped = env.packDropped.contains(en.tid);
        f.prepareProcess = env.prepareProcess;
        f.runningTid = env.runningTid;
        f.importingTid = env.importingTid;
        en.facts = f;
        en.chip = GameState.chip(f);
        en.prepared = GameState.prepared(f, en.chip);
        en.queuePos = env.queue.position(en.tid);
        en.resume = en.chip == GameState.Chip.READY ? Pack.resume(filesDir, en.tid) : Pack.Resume.NO;
        en.found = en.prepared == GameState.Prepared.PREPARED || en.chip == GameState.Chip.READY && prepares
                   ? found(filesDir, en) : 0;
    }

    /**
     * What a prepared game's runs found that its preparation missed: an Xbox title's Self-healing offer (its seeds,
     * work/<TID>/selfheal.json), the other consoles' discovered addresses beyond the pack's (cached count, no read).
     */
    static int found(File filesDir, Entry en) {
        if (en.facts == null || en.facts.pack != Pack.Check.VALID)
            return 0;
        if (en.realTid()) {
            SelfHeal.State s = SelfHeal.read(new File(new File(filesDir, "work"), en.tid));
            return s.offer == SelfHeal.Offer.IMPROVE || s.offer == SelfHeal.Offer.PREPARE_AGAIN ? s.seeds : 0;
        }
        return Math.max(0, Pack.newlyDiscoveredNow(filesDir, en.tid));
    }

    /**
     * A record for en when it has none yet (a game found in the folder, opened for the first time): its title and
     * active source; a folder read in place is its own data.
     */
    static Library.Record recordFor(Entry en, long now) {
        Library.Record r = en.record;
        if (r == null) {
            r = new Library.Record();
            r.titleId = en.tid;
            r.title = en.title();
            r.addedAt = now;
            if (en.active != null) {
                r.source = en.active.loc;
                r.xbeSha256 = en.active.xbeSha256;
            }
            en.record = r;
        }
        File f = inPlaceDir(en.active);
        if (r.dataDir.isEmpty() && f != null) {   // (also a record made earlier for an image of the same title)
            r.dataDir = f.getAbsolutePath();
            r.dataKind = Library.KIND_IN_PLACE;
            r.source = en.active.loc;
            r.xbeSha256 = en.active.xbeSha256;
        }
        return r;
    }

    /** The folder of a game folder variant read as files (played in place), or null. */
    static File inPlaceDir(LibraryIndex.Item i) {
        return i != null && LibraryIndex.KIND_FOLDER.equals(i.kind) ? LibraryIndex.locFile(i.loc) : null;
    }

    // ---- Labels -----------------------------------------------------------------------------------------------------

    static int chipLabel(GameState.Chip c) {
        switch (c) {
        case RUNNING:
            return R.string.CHIP_RUNNING;
        case PREPARING:
            return R.string.CHIP_PREPARING;
        case QUEUED:
            return R.string.CHIP_QUEUED;
        case IMPORTING:
            return R.string.LIB_CHIP_IMPORTING;
        case PREPARE_FAILED:
            return R.string.CHIP_PREPARE_FAILED;
        case PACK_INVALID:
            return R.string.LIB_CHIP_PACK_INVALID;
        case READY:
            return R.string.STATE_READY;
        case NEEDS_PREPARING:
            return R.string.CHIP_NEEDS_PREPARE;
        case NOT_IMPORTED:
            return R.string.LIB_CHIP_NOT_IMPORTED;
        case NEEDS_COPYING:
            return R.string.LIB_CHIP_NEEDS_COPYING;
        case NO_FILE_ACCESS:
            return R.string.STATE_NO_ACCESS;
        case SOURCE_GONE:
            return R.string.LIB_CHIP_SOURCE_GONE;
        case NOT_PLAYABLE_YET:
            return R.string.LIB_CHIP_NOT_PLAYABLE_YET;
        case ON_NETWORK:
            return R.string.NET_CHIP_ON_NETWORK;
        case DOWNLOADING:
            return R.string.NET_CHIP_DOWNLOADING;
        default:
            return R.string.STATE_MISSING;
        }
    }

    /** The label of a Ready game's preparation (GameState.prepared); 0 for NONE. */
    static int preparedLabel(GameState.Prepared p) {
        switch (p) {
        case PREPARED:
            return R.string.LIB_CHIP_PREPARED;
        case OUTDATED:
            return R.string.LIB_CHIP_OUTDATED;
        case NOT_PREPARED:
            return R.string.LIB_CHIP_NOT_PREPARED;
        default:
            return 0;
        }
    }

    /** The colour of an entry's state: its preparation's when it says one (OK, WARN, neutral), else its chip's. */
    static int chipColor(Entry e) {
        if (e.found > 0 && e.prepared != GameState.Prepared.OUTDATED)
            return Theme.WARN;
        switch (e.prepared) {
        case PREPARED:
            return Theme.OK;
        case OUTDATED:
            return Theme.WARN;
        case NOT_PREPARED:
            return Theme.TEXT_DIM;   // neutral: it plays, in the interpreter
        default:
            return chipColor(e.chip);
        }
    }

    /**
     * XBA-258: the card's « Resume » tag: a Ready game that resumes where the player left off (a GBA / PS1 / N64
     * game; an Xbox title prepared with resume entries), so the player knows it before launching.
     */
    static boolean resumeTag(Entry e) {
        return e.chip == GameState.Chip.READY && e.resume == Pack.Resume.YES;
    }

    /** The card shows a state line: not for a plain « Ready » (no toolchain: nothing worth saying). */
    static boolean chipShown(Entry e) {
        return e.chip != GameState.Chip.READY || e.prepared != GameState.Prepared.NONE || e.found > 0;
    }

    static int chipColor(GameState.Chip c) {
        switch (c) {
        case RUNNING:
        case PREPARING:
        case READY:
            return Theme.OK;
        case QUEUED:
        case IMPORTING:
            return Theme.ACCENT;
        case NOT_IMPORTED:
        case NOT_PLAYABLE_YET:
        case ON_NETWORK:
            return Theme.TEXT_DIM;   // neutral: nothing is wrong (ACCENT is the « ready » green in stock)
        case DOWNLOADING:
            return Theme.ACCENT;
        case PREPARE_FAILED:
        case PACK_INVALID:
            return Theme.ERROR;
        default:
            return Theme.WARN;
        }
    }

    /** « Queued (2) » for a queued entry, « Prepared · out of date » etc. for a Ready one, else the chip's label. */
    static String chipText(Context c, Entry e) {
        if (e.chip == GameState.Chip.QUEUED && e.queuePos > 0)
            return c.getString(R.string.LIB_QUEUED_N, e.queuePos);
        if (e.chip == GameState.Chip.DOWNLOADING && e.download != null)
            return NetDownloads.RUNNING.equals(e.download.state)
                   ? c.getString(NetUnpack.E_UNPACKING.equals(e.download.error) ? R.string.NET_CHIP_UNPACKING_N
                                                                               : R.string.NET_CHIP_DOWNLOADING_N,
                                 e.download.percent())
                   : c.getString(R.string.NET_CHIP_WAITING);
        if (e.found > 0 && e.prepared != GameState.Prepared.OUTDATED)
            return c.getString(R.string.LIB_NEW_CODE_N, e.found);
        if (e.prepared != GameState.Prepared.NONE)
            return c.getString(preparedLabel(e.prepared));
        return c.getString(chipLabel(e.chip));
    }

    /** « North America · Japan », « World », "" for none (the certificate's region bits). */
    static String regionText(Context c, int region) {
        List<String> l = new ArrayList<>();
        if ((region & XbeInfo.REGION_NA) != 0)
            l.add(c.getString(R.string.LIB_REGION_NA));
        if ((region & XbeInfo.REGION_JAPAN) != 0)
            l.add(c.getString(R.string.LIB_REGION_JP));
        if ((region & XbeInfo.REGION_WORLD) != 0)
            l.add(c.getString(R.string.LIB_REGION_WORLD));
        return android.text.TextUtils.join(" · ", l);
    }

    /** The certificate's version as the dashboard shows it: 1.02 for 0x102... kept as a plain number otherwise. */
    static String versionText(int v) {
        return String.format(Locale.ROOT, "%d.%02d", v >> 8 & 0xff, v & 0xff);
    }

    // ---- Art --------------------------------------------------------------------------------------------------------

    /** The title image from the shared cache (files/art/<TID>.png, TitleArt), decoded once per title; null if none. */
    static Bitmap art(File filesDir, Map<String, Bitmap> cache, Entry e) {
        if (RemoteLibrary.isRemoteKey(e.tid))
            return null;   // XBA-319: no cover before the download, the card draws the « remote content » art
        if (e.otherConsole() && !Library.isLocalKey(e.tid)) {   // a GBA / PS1 / N64 game: its cover (CoverArt)
            if (!cache.containsKey(e.tid))
                cache.put(e.tid, CoverArt.load(filesDir, e.tid));
            return cache.get(e.tid);
        }
        if (!e.realTid())
            return null;
        if (cache.containsKey(e.tid))
            return cache.get(e.tid);
        File f = TitleArt.cacheFile(filesDir, e.tid);
        Bitmap b = f.isFile() ? BitmapFactory.decodeFile(f.getPath()) : null;
        if (b == null && e.hasData()) {
            File x = e.xbe(filesDir);
            b = x == null ? null : TitleArt.load(filesDir, x, e.tid);
        }
        cache.put(e.tid, b);
        return b;
    }

    /** The backdrop glows for a focused card: its cached theme's GLOW and GLOW2, else the stock accent. */
    static int[] glows(Context c, Entry e) {
        int stock = GamePalette.STOCK[GamePalette.ACCENT];
        if (e == null || !e.realTid())
            return new int[] { stock, stock };
        GameTheme t = GameTheme.load(c, null, e.tid);
        if (t == null)
            return new int[] { stock, stock };
        return new int[] { t.colors.get(GamePalette.GLOW), t.colors.get(GamePalette.GLOW2) };
    }

    // ---- The grid ---------------------------------------------------------------------------------------------------

    /**
     * The grid: rows of cols cards in a vertical scroll (the D-pad's focus search moves between them by geometry),
     * the first column on the left margin, the last art on the right one. cards are added in order; the caller
     * registers them as focusables.
     */
    static ScrollView grid(Context c, List<? extends View> cards, View footer, int cols) {
        ScrollView scroll = scroll(c);
        LinearLayout col = (LinearLayout)scroll.getChildAt(0);
        addRows(c, col, cards, cols);
        if (footer != null) {
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                                        LinearLayout.LayoutParams.WRAP_CONTENT);
            p.topMargin = Theme.S2;
            col.addView(footer, p);
        }
        return scroll;
    }

    /** The library's vertical scroll and its column (its only child), empty. */
    static ScrollView scroll(Context c) {
        ScrollView scroll = new ScrollView(c);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setClipToPadding(false);
        scroll.setClipChildren(false);
        scroll.setPadding(0, Theme.S1, 0, Theme.S3);
        LinearLayout col = new LinearLayout(c);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setClipChildren(false);
        scroll.addView(col, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                                                         FrameLayout.LayoutParams.WRAP_CONTENT));
        return scroll;
    }

    /**
     * The rows of cols cards for cards, added to col (the « Recently played » row is one of them, XBA-206). A card's
     * slot is GameCard.W (room for its title past the art) when the row has room for it, else the art's width.
     */
    static void addRows(Context c, LinearLayout col, List<? extends View> cards, int cols) {
        LinearLayout row = null;
        int n = cards.size(), rows = (n + cols - 1) / cols;
        for (int i = 0; i < rows * cols; i++) {
            if (i % cols == 0) {
                row = new GridRow(c, cols);
                col.addView(row, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, GameCard.H));
            } else {
                row.addView(new View(c), new LinearLayout.LayoutParams(0, 1, 1));   // the gaps share the rest
            }
            boolean last = i % cols == cols - 1;
            int w = last ? GameCard.ART : GameCard.W;   // the last art ends on the margin
            View v = i < n ? cards.get(i) : new View(c);
            row.addView(v, new LinearLayout.LayoutParams(w, GameCard.H));
        }
    }

    /** A row of the grid: its cards' slots narrow to the art's width when GameCard.W each does not fit. */
    private static final class GridRow extends LinearLayout {
        private final int cols;

        GridRow(Context c, int cols) {
            super(c);
            this.cols = cols;
            setOrientation(HORIZONTAL);
            setClipChildren(false);
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            if (MeasureSpec.getMode(wSpec) != MeasureSpec.UNSPECIFIED) {
                boolean wide = (cols - 1) * GameCard.W + GameCard.ART <= MeasureSpec.getSize(wSpec);
                for (int i = 0; i < getChildCount() - 1; i++) {   // the last slot stays the art's width
                    LayoutParams p = (LayoutParams)getChildAt(i).getLayoutParams();
                    if (p.weight == 0)   // a card's slot, not a gap
                        p.width = wide ? GameCard.W : GameCard.ART;
                }
            }
            super.onMeasure(wSpec, hSpec);
        }
    }
}
