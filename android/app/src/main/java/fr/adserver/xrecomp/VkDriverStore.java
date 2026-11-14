package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.text.DateFormat;
import java.util.Date;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The glue between the Drivers screen, the driver store (VkDrivers) and the downloadable list (VkDriverIndex): the
 * {@link VkDriverIndex.Store} adapter, what a driver's last start says, and the readable texts for a refusal or a
 * list's state. No view here, so the JVM tests cover it.
 */
final class VkDriverStore {
    private VkDriverStore() {}

    // ---- The store, as VkDriverIndex needs it ---------------------------------------------------------------------

    /** VkDriverIndex.Store over VkDrivers: a verified download is installed with its GitHub origin. */
    static final class Adapter implements VkDriverIndex.Store {
        final VkDrivers drivers;
        /** The driver the last install gave (already installed: the one there), or null. */
        volatile VkDrivers.Driver installed;

        Adapter(VkDrivers drivers) {
            this.drivers = drivers;
        }

        /** The SHA-256 (lower-case hex) of every installed package. */
        @Override
        public Set<String> installedSha256() {
            Set<String> out = new HashSet<>();
            for (VkDrivers.Driver d : drivers.list()) {
                if (d.sha256 != null && !d.sha256.isEmpty())
                    out.add(d.sha256.toLowerCase(Locale.ROOT));
            }
            return out;
        }

        @Override
        public void install(File zip, String expectedSha256, VkDriverIndex.Entry from) throws IOException {
            installed = drivers.install(zip, VkDrivers.Origin.github(from.source, from.tag, from.assetName,
                                                                     expectedSha256, from.size > 0 ? from.size : -1));
        }
    }

    /**
     * The listed drivers not installed yet: not the same sha256 as an installed package, nor the id that sha256 would
     * give (a driver directory without origin.json, put there by hand, has no recorded sha256 but its id).
     */
    static List<VkDriverIndex.Entry> toDownload(VkDriverIndex.Listing l, VkDrivers drivers) {
        List<VkDriverIndex.Entry> out = new ArrayList<>();
        for (VkDriverIndex.Entry e : l.notInstalled(new Adapter(drivers).installedSha256())) {
            if (e.sha256 == null || e.sha256.length() < VkDrivers.ID_HEX
                || drivers.read(e.sha256.substring(0, VkDrivers.ID_HEX)) == null)
                out.add(e);
        }
        return out;
    }

    // ---- A driver's last start ------------------------------------------------------------------------------------

    /**
     * What the native runtime said about a driver's last start: OK; CRASHED; UNSUPPORTED (it loaded, but the N64
     * renderer, paraLLEl-RDP, cannot run on it: no device, missing features, or its shaders fail); OPEN_FAILED (it
     * could not be opened); NOT_LOADED (Android kept the device's own driver); FAILED (a runtime before UNSUPPORTED /
     * OPEN_FAILED: either); UNKNOWN (not used yet, or a word this app does not know).
     */
    enum LastStart { OK, CRASHED, UNSUPPORTED, OPEN_FAILED, FAILED, NOT_LOADED, UNKNOWN }

    /** A status word of last_start / &lt;id&gt;.status, read. */
    static LastStart parseLastStart(String s) {
        if (s == null)
            return LastStart.UNKNOWN;
        switch (s) {
        case "ok":
            return LastStart.OK;
        case "crashed":
            return LastStart.CRASHED;
        case "unsupported":
            return LastStart.UNSUPPORTED;
        case "open_failed":
            return LastStart.OPEN_FAILED;
        case "not_loaded":
            return LastStart.NOT_LOADED;
        case "failed":
            return LastStart.FAILED;
        default:
            return LastStart.UNKNOWN;
        }
    }

    /**
     * The driver's last start: CRASHED when the trial marker names it, else what the runtime wrote for it
     * (VkDrivers.lastStartStatus: its own status file, else last_start).
     */
    static LastStart lastStart(VkDrivers drivers, String id) {
        if (drivers.failedLastStart(id))
            return LastStart.CRASHED;
        return parseLastStart(drivers.lastStartStatus(id));
    }

    /** The warning for a last start that did not work, or 0. */
    static int lastStartWarning(LastStart s) {
        switch (s) {
        case CRASHED:
            return R.string.VKD_LAST_CRASHED;
        case UNSUPPORTED:
            return R.string.VKD_LAST_UNSUPPORTED;
        case OPEN_FAILED:
            return R.string.VKD_LAST_OPEN_FAILED;
        case FAILED:
            return R.string.VKD_LAST_FAILED;
        case NOT_LOADED:
            return R.string.VKD_LAST_NOT_LOADED;
        default:
            return 0;
        }
    }

    /** The short badge for a last start that did not work (« Not compatible », « Failed last time »), or 0. */
    static int lastStartBadge(LastStart s, boolean active) {
        if (lastStartWarning(s) == 0)
            return 0;
        if (s == LastStart.UNSUPPORTED)
            return active ? R.string.VKD_ACTIVE_UNSUPPORTED_BADGE : R.string.VKD_UNSUPPORTED_BADGE;
        return active ? R.string.VKD_ACTIVE_WARN_BADGE : R.string.VKD_WARN_BADGE;
    }

    // ---- A game's own choice (ROM_OPTIONS, the in-game menu) ----------------------------------------------------------

    /**
     * The choices a game's row cycles through: null (as in Settings), VkDrivers.SYSTEM, then each installed driver's
     * id, in the order given.
     */
    static List<String> gameChoices(List<VkDrivers.Driver> installed) {
        List<String> out = new ArrayList<>();
        out.add(null);
        out.add(VkDrivers.SYSTEM);
        for (VkDrivers.Driver d : installed)
            out.add(d.id);
        return out;
    }

    /** The next choice from cur (dir +1 / -1, wrapping); a choice not in the list (a driver gone) counts as null. */
    static String nextGameChoice(List<String> choices, String cur, int dir) {
        int i = Math.max(0, choices.indexOf(cur)), n = choices.size();
        return choices.get(((i + dir) % n + n) % n);
    }

    /** Steps a game's choice and saves it; returns what is saved now (null: as in Settings). */
    static String stepGameChoice(VkDrivers drivers, String key, int dir) {
        List<String> choices = gameChoices(drivers.list());
        String v = nextGameChoice(choices, drivers.gameChoice(key), dir);
        // a driver that cannot be chosen (deleted meanwhile): the next one
        for (int k = 0; k < choices.size() && !drivers.setGameChoice(key, v); k++)
            v = nextGameChoice(choices, v, dir);
        return drivers.gameChoice(key);
    }

    /** « As in Settings (System) », « System », or a driver's label: a game's choice for its row. */
    static String gameChoiceLabel(Context c, VkDrivers drivers, String choice) {
        if (choice == null) {
            VkDrivers.Driver g = drivers.active();
            return c.getString(R.string.ROM_OPT_FOLLOW,
                               g == null ? c.getString(R.string.VKD_SYSTEM_SHORT) : label(g));
        }
        if (VkDrivers.SYSTEM.equals(choice))
            return c.getString(R.string.VKD_SYSTEM_SHORT);
        VkDrivers.Driver d = drivers.read(choice);
        return d == null ? choice : label(d);
    }

    /** The warning (a string resource) for the driver a game starts with (key null: the global choice), or 0. */
    static int effectiveWarning(VkDrivers drivers, String key) {
        VkDrivers.Driver d = drivers.resolve(key);
        return d == null ? 0 : lastStartWarning(lastStart(drivers, d.id));
    }

    // ---- The download list ----------------------------------------------------------------------------------------

    /**
     * A listed file's name for its row: without « .zip », then without « .adpkg » (case ignored):
     * « turnip_mrpurple_T29-toasted.adpkg.zip » -> « turnip_mrpurple_T29-toasted ».
     */
    static String displayName(String assetName) {
        String n = assetName == null ? "" : assetName;
        for (String ext : new String[] { ".zip", ".adpkg" }) {
            if (n.length() > ext.length() && n.toLowerCase(Locale.ROOT).endsWith(ext))
                n = n.substring(0, n.length() - ext.length());
        }
        return n;
    }

    /** Each listed driver's row name: displayName, with « · tag » when two listed drivers have the same one. */
    static List<String> rowNames(List<VkDriverIndex.Entry> entries) {
        java.util.Map<String, Integer> count = new java.util.HashMap<>();
        for (VkDriverIndex.Entry e : entries)
            count.merge(displayName(e.assetName).toLowerCase(Locale.ROOT), 1, Integer::sum);
        List<String> out = new ArrayList<>();
        for (VkDriverIndex.Entry e : entries) {
            String n = displayName(e.assetName);
            out.add(count.get(n.toLowerCase(Locale.ROOT)) > 1 ? n + " · " + e.tag : n);
        }
        return out;
    }

    // ---- Readable texts --------------------------------------------------------------------------------------------

    /** Why an install was refused, readable. */
    static int installReason(VkDrivers.Reason r) {
        switch (r) {
        case NOT_A_ZIP:
            return R.string.VKD_ERR_NOT_ZIP;
        case TOO_MANY_ENTRIES:
        case BAD_ENTRY_NAME:
        case DUPLICATE_ENTRY:
        case NO_META:
        case BAD_META:
        case NO_LIBRARY:
            return R.string.VKD_ERR_NOT_DRIVER;
        case TOO_LARGE:
            return R.string.VKD_ERR_TOO_LARGE;
        case BAD_ELF:
            return R.string.VKD_ERR_BAD_ELF;
        case MIN_API:
            return R.string.VKD_ERR_MIN_API;
        case SHA256_MISMATCH:
        case SIZE_MISMATCH:
            return R.string.VKD_ERR_CORRUPT;
        default:
            return R.string.VKD_ERR_IO;
        }
    }

    /** Why a download stopped, readable (CANCELLED included). */
    static int downloadReason(VkDriverIndex.DownloadException.Reason r) {
        switch (r) {
        case BAD_URL:
            return R.string.VKD_ERR_BAD_URL;
        case HTTP:
            return R.string.VKD_ERR_HTTP;
        case SIZE:
        case SHA256:
            return R.string.VKD_ERR_CORRUPT;
        default:
            return R.string.VKD_CANCELED;
        }
    }

    /** The note for an install or a download that failed: the reason in plain words (every string takes %1$s). */
    static String errorText(Context c, IOException e) {
        String detail = String.valueOf(e.getMessage());
        int res;
        if (e instanceof VkDrivers.InstallException)
            res = installReason(((VkDrivers.InstallException)e).reason);
        else if (e instanceof VkDriverIndex.DownloadException)
            res = downloadReason(((VkDriverIndex.DownloadException)e).reason);
        else
            res = R.string.VKD_ERR_NETWORK;
        return c.getString(res, detail);
    }

    /** « GitHub · owner/repo · tag », « Imported zip », « From XRecomp »: where an installed driver came from. */
    static String originText(Context c, VkDrivers.Driver d) {
        if (VkDrivers.Origin.GITHUB.equals(d.originKind))
            return c.getString(R.string.VKD_ORIGIN_GITHUB, d.repo == null ? "?" : d.repo, d.tag == null ? "?" : d.tag);
        if (VkDrivers.Origin.INHERITED.equals(d.originKind))
            return c.getString(R.string.VKD_ORIGIN_INHERITED);
        return c.getString(R.string.VKD_ORIGIN_MANUAL);
    }

    /** « Turnip T30 (Mesa 26.3.0) »: the name and, when the package gives one, its driver version. */
    static String label(VkDrivers.Driver d) {
        String v = d.driverVersion != null && !d.driverVersion.isEmpty() ? d.driverVersion : d.packageVersion;
        return v == null || v.isEmpty() || d.name.contains(v) ? d.name : d.name + " (" + v + ")";
    }

    /**
     * One line for the list's state, from the sources' states (the worst wins): updating, GitHub's limit (retry
     * after), no connection, an error, never fetched, or the date of the list shown.
     */
    static String listState(Context c, VkDriverIndex.Listing l, boolean refreshing) {
        return listState(c, l, refreshing, true);
    }

    /**
     * listState for a row's value when short: the reason alone (« GitHub limit reached », « No connection »), without
     * the retry time and the list's date (the description and the note give them).
     */
    static String listState(Context c, VkDriverIndex.Listing l, boolean refreshing, boolean full) {
        if (refreshing)
            return c.getString(R.string.VKD_LIST_UPDATING);
        if (l == null)
            return c.getString(R.string.VKD_LIST_NONE);
        long reset = 0, fetched = l.oldestFetch();
        boolean limited = false, offline = false, failed = false, stale = false, none = true;
        int code = 0;
        for (VkDriverIndex.SourceStatus s : l.status) {
            none &= s.state == VkDriverIndex.State.NONE;
            switch (s.state) {
            case RATE_LIMITED:
                limited = true;
                reset = Math.max(reset, s.resetAt);
                break;
            case OFFLINE:
                offline = true;
                break;
            case FAILED:
                failed = true;
                code = s.httpCode;
                break;
            case STALE:
                stale = true;
                break;
            default:
                break;
            }
        }
        String from = fetched > 0 ? c.getString(R.string.VKD_LIST_FROM, date(fetched)) : null;
        String why;
        if (limited)
            why = reset > 0 && full ? c.getString(R.string.VKD_LIST_LIMITED_UNTIL, time(reset))
                            : c.getString(R.string.VKD_LIST_LIMITED);
        else if (offline)
            why = c.getString(R.string.VKD_LIST_OFFLINE);
        else if (failed)
            why = c.getString(R.string.VKD_LIST_FAILED, code > 0 ? "HTTP " + code : "?");
        else if (none)
            return c.getString(R.string.VKD_LIST_NONE);
        else if (stale && from != null)
            return c.getString(R.string.VKD_LIST_OLD, from);
        else
            return from != null ? from : c.getString(R.string.VKD_LIST_NONE);
        return from != null && full ? why + " · " + from : why;
    }

    static String date(long ms) {
        return DateFormat.getDateInstance(DateFormat.MEDIUM).format(new Date(ms));
    }

    static String time(long ms) {
        return DateFormat.getTimeInstance(DateFormat.SHORT).format(new Date(ms));
    }
}
