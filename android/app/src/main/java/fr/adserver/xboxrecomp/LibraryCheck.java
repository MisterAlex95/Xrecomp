package fr.adserver.xboxrecomp;

import android.content.Context;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * « Check my library » (XBA-130, docs/android/app-shell.md « Library check »): the state of a check and its files.
 *
 * <p>A check prepares every title that needs it, boots each one for {@link #SECONDS} with the compat runner's pad
 * script and status file (docs/technical/compat-runner.md), grades it with tools.compat's own rules (the Python,
 * run by {@link CheckPlatform}), and appends one line per title to {@code <files>/check/compat-results.jsonl}, the
 * desktop runner's schema plus the device's fields. Its state is {@code <files>/check/check.json}, written after
 * every step, so a crash of the app or a reboot carries on where it stopped ({@link CheckDriver}).
 *
 * <p>The game process learns that it is checked from {@code <files>/check/current.json} (title ID, run folder, until
 * when): ShellActivity puts {@link #runEnv} in the environment before the runtime starts, also after a quick reboot.
 * Nothing from the game leaves the device beyond what the results line holds: title ID, the certificate's title
 * name, XDK library builds, engine names, stage, first blocker, and numbers.
 */
final class LibraryCheck {
    static final String TAG = "xboxrecomp";
    static final String DIR = "check", STATE = "check.json", RESULTS = "compat-results.jsonl", CURRENT = "current.json";
    /** How long each title runs, and the sustained window: tools.compat run's defaults. */
    static final int SECONDS = 180, SUSTAIN = 60;
    /** The pad script: Start three times from 8 s, then A, every 2 s (tools/compat/__main__.py PRESS_FIRST/EVERY). */
    static final long PRESS_FIRST_MS = 8000, PRESS_EVERY_MS = 2000;
    static final int PRESS_STARTS = 3;

    // The check's state.
    static final String RUNNING = "running", PAUSED = "paused", DONE = "done", STOPPED = "stopped";
    // Why it is paused.
    static final String AWAY = "away", HOT = "thermal", BATTERY = "battery", USER = "user";
    // A title's phase.
    static final String PENDING = "pending", PREPARING = "preparing", READY = "ready", BOOTING = "booting",
            GRADED = "graded";

    private LibraryCheck() {}

    /** One title of the check. */
    static final class Title {
        String tid = "", title = "";
        /** The game's files (default.xbe and the rest), "" when they are not in the app: Prepare only, no boot. */
        String gameDir = "";
        /** What the Prepare reads: the game's files, else the library's cached default.xbe. */
        String source = "";
        /** The active default.xbe's SHA-256 as the library knows it ("" unknown): a pack from another one is stale. */
        String xbeSha256 = "";
        String phase = PENDING;
        int prepareAttempts, bootAttempts;
        /** Time spent preparing (the Prepare's own run time, summed over resumed attempts), -1 = none was needed. */
        long prepareMs = -1;
        long prepareStartedAt;
        /** After grading: the line's stage and blocker (for the screen; the results file has the line). */
        String stage = "", blocker = "";

        boolean bootable() {
            return !gameDir.isEmpty();
        }

        JSONObject toJson() throws JSONException {
            return new JSONObject().put("tid", tid).put("title", title).put("game_dir", gameDir).put("source", source)
                    .put("xbe_sha256", xbeSha256).put("phase", phase).put("prepare_attempts", prepareAttempts).put("boot_attempts", bootAttempts)
                    .put("prepare_ms", prepareMs).put("prepare_started_at", prepareStartedAt).put("stage", stage)
                    .put("blocker", blocker);
        }

        static Title fromJson(JSONObject o) {
            Title t = new Title();
            t.tid = o.optString("tid", "");
            t.title = o.optString("title", "");
            t.gameDir = o.optString("game_dir", "");
            t.source = o.optString("source", "");
            t.xbeSha256 = o.optString("xbe_sha256", "");
            t.phase = o.optString("phase", PENDING);
            t.prepareAttempts = o.optInt("prepare_attempts", 0);
            t.bootAttempts = o.optInt("boot_attempts", 0);
            t.prepareMs = o.optLong("prepare_ms", -1);
            t.prepareStartedAt = o.optLong("prepare_started_at", 0);
            t.stage = o.optString("stage", "");
            t.blocker = o.optString("blocker", "");
            return t;
        }
    }

    /** The boot in progress. */
    static final class Run {
        long launchedAt;
        /** Wall time of the status clock's 0 (found from the first status line), 0 until then. */
        long statusOrigin;
        int pid;
        boolean sawGame;
        final List<Long> presses = new ArrayList<>();
        long peakKb;
        long awaySince, goneSince, noWindowSince, lastSampleAt;

        JSONObject toJson() throws JSONException {
            JSONArray p = new JSONArray();
            for (long x : presses)
                p.put(x);
            return new JSONObject().put("launched_at", launchedAt).put("status_origin", statusOrigin).put("pid", pid)
                    .put("saw_game", sawGame).put("presses", p).put("peak_kb", peakKb);
        }

        static Run fromJson(JSONObject o) {
            Run r = new Run();
            r.launchedAt = o.optLong("launched_at", 0);
            r.statusOrigin = o.optLong("status_origin", 0);
            r.pid = o.optInt("pid", 0);
            r.sawGame = o.optBoolean("saw_game", false);
            JSONArray p = o.optJSONArray("presses");
            for (int i = 0; p != null && i < p.length(); i++)
                r.presses.add(p.optLong(i));
            r.peakKb = o.optLong("peak_kb", 0);
            return r;
        }
    }

    /** check.json. */
    static final class State {
        String state = RUNNING, reason = "";
        long startedAt, updatedAt;
        int seconds = SECONDS, sustain = SUSTAIN;
        final List<Title> titles = new ArrayList<>();
        int index;
        Run run;

        Title current() {
            return index >= 0 && index < titles.size() ? titles.get(index) : null;
        }

        boolean active() {
            return RUNNING.equals(state) || PAUSED.equals(state);
        }

        /** Titles graded so far, per stage (in tools.compat's order). */
        Map<String, Integer> counts() {
            Map<String, Integer> m = new LinkedHashMap<>();
            for (String s : STAGES)
                m.put(s, 0);
            for (Title t : titles) {
                if (GRADED.equals(t.phase) && m.containsKey(t.stage))
                    m.put(t.stage, m.get(t.stage) + 1);
            }
            return m;
        }

        int graded() {
            int n = 0;
            for (Title t : titles)
                n += GRADED.equals(t.phase) ? 1 : 0;
            return n;
        }

        JSONObject toJson() throws JSONException {
            JSONArray a = new JSONArray();
            for (Title t : titles)
                a.put(t.toJson());
            JSONObject o = new JSONObject().put("version", 1).put("state", state).put("reason", reason)
                    .put("started_at", startedAt).put("updated_at", updatedAt).put("seconds", seconds)
                    .put("sustain", sustain).put("titles", a).put("index", index);
            if (run != null)
                o.put("run", run.toJson());
            return o;
        }

        static State fromJson(JSONObject o) {
            State s = new State();
            s.state = o.optString("state", STOPPED);
            s.reason = o.optString("reason", "");
            s.startedAt = o.optLong("started_at", 0);
            s.updatedAt = o.optLong("updated_at", 0);
            s.seconds = Math.max(5, o.optInt("seconds", SECONDS));
            s.sustain = Math.max(1, o.optInt("sustain", SUSTAIN));
            JSONArray a = o.optJSONArray("titles");
            for (int i = 0; a != null && i < a.length(); i++) {
                JSONObject t = a.optJSONObject(i);
                if (t != null)
                    s.titles.add(Title.fromJson(t));
            }
            s.index = o.optInt("index", 0);
            JSONObject r = o.optJSONObject("run");
            s.run = r == null ? null : Run.fromJson(r);
            return s;
        }
    }

    /** tools.compat's stages (tools/compat/__init__.py STAGES), lowest first. */
    static final String[] STAGES = { "none", "prepares", "boots", "renders", "responsive", "sustained" };

    // ---- Files ------------------------------------------------------------------------------------------------------

    static File dir(File filesDir) {
        return new File(filesDir, DIR);
    }

    static File stateFile(File filesDir) {
        return new File(dir(filesDir), STATE);
    }

    /** The report: one line per title and check, appended (« Share report » sends it). */
    static File resultsFile(File filesDir) {
        return new File(dir(filesDir), RESULTS);
    }

    static File currentFile(File filesDir) {
        return new File(dir(filesDir), CURRENT);
    }

    /** <files>/check/run/<TID>: the boot's status file, pad file and log (they stay on the device). */
    static File runDir(File filesDir, String tid) {
        return new File(new File(dir(filesDir), "run"), tid);
    }

    /** The title's Prepare work folder (ident.json, discovered.jsonl), as PrepareService names it. */
    static File workDir(File filesDir, String tid) {
        return new File(new File(filesDir, "work"), tid);
    }

    /** check.json, or null when there is none or it cannot be read. */
    static State read(File filesDir) {
        File f = stateFile(filesDir);
        if (!f.isFile())
            return null;
        try {
            return State.fromJson(new JSONObject(PrepareProtocol.readText(f, 4 << 20)));
        } catch (IOException | JSONException e) {
            Log.w(TAG, "check: " + f + " unreadable: " + e);
            return null;
        }
    }

    static void write(File filesDir, State s) throws IOException {
        try {
            Library.writeAtomically(stateFile(filesDir), s.toJson().toString(1) + "\n");
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    /** A check is running or paused (the launcher hands over to its screen). */
    static boolean active(File filesDir) {
        State s = read(filesDir);
        return s != null && s.active();
    }

    // ---- The game process's side --------------------------------------------------------------------------------

    /**
     * What a checked run puts in the game's environment: tools.compat's boot() minus the headless and screenshot
     * switches (tools/compat/device.py check_env, the same names and values): RECOMP_STATUS, RECOMP_STATUS_MS,
     * RECOMP_PAD_LIVE, and RECOMP_USB = recomp.cfg's when it has one, else 1 (the pad script drives the USB pad).
     */
    static Map<String, String> runEnv(File runDir, String cfgUsb) {
        Map<String, String> m = new LinkedHashMap<>();
        m.put("RECOMP_STATUS", new File(runDir, "status.jsonl").getAbsolutePath());
        m.put("RECOMP_STATUS_MS", "1000");
        m.put("RECOMP_PAD_LIVE", new File(runDir, "pad-live.txt").getAbsolutePath());
        m.put("RECOMP_USB", cfgUsb != null ? cfgUsb : "1");
        return m;
    }

    static void writeCurrent(File filesDir, String tid, File runDir, long until) throws IOException {
        try {
            Library.writeAtomically(currentFile(filesDir), new JSONObject().put("tid", tid)
                    .put("run_dir", runDir.getAbsolutePath()).put("until", until).toString() + "\n");
        } catch (JSONException e) {
            throw new IOException(e);
        }
    }

    static void clearCurrent(File filesDir) {
        //noinspection ResultOfMethodCallIgnored
        currentFile(filesDir).delete();
    }

    /** The environment for a game process that runs tid now, or null when no check runs it. */
    static Map<String, String> currentEnv(File filesDir, String tid, long now, String cfgUsb) {
        File f = currentFile(filesDir);
        if (tid == null || !f.isFile())
            return null;
        try {
            JSONObject o = new JSONObject(PrepareProtocol.readText(f, 1 << 16));
            if (!tid.equalsIgnoreCase(o.optString("tid", "")) || now > o.optLong("until", 0))
                return null;
            return runEnv(new File(o.optString("run_dir", "")), cfgUsb);
        } catch (IOException | JSONException e) {
            return null;
        }
    }

    /**
     * ShellActivity.onCreate, before SDL starts: a checked run gets {@link #runEnv} (it wins over recomp.cfg, which
     * the runtime reads later and which never replaces a variable already set). A normal start changes nothing.
     * Returns whether this start is a checked run (the on-screen pad stays off then, wave 4).
     */
    static boolean applyRunEnv(Context c) {
        try {
            File ext = GameStore.externalFilesDir(c);
            String usb = ext == null ? null : RecompConfig.load(ext).get("RECOMP_USB");
            Map<String, String> env = currentEnv(c.getFilesDir(), GameLanguage.currentTitleId(c),
                                                 System.currentTimeMillis(), usb);
            if (env == null)
                return false;
            for (Map.Entry<String, String> e : env.entrySet())
                Os.setenv(e.getKey(), e.getValue(), true);
            Log.i(TAG, "shell: library check run: " + env);
            return true;
        } catch (ErrnoException | RuntimeException e) {
            Log.w(TAG, "shell: cannot set the library check's environment", e);
            return false;
        }
    }

    // ---- The plan ---------------------------------------------------------------------------------------------------

    /** The toolkit the installed tools carry (files/toolkit-<v>/tools/VERSION), or "" when none is installed. */
    static String installedToolkit(File filesDir) {
        File[] l = filesDir.listFiles((d, n) -> n.startsWith("toolkit-"));
        if (l == null)
            return "";
        for (File d : l) {
            File v = new File(d, "tools/VERSION");
            if (new File(d, Toolchain.COMPLETE).isFile() && v.isFile()) {
                try {
                    return PrepareProtocol.readText(v, 4096).trim();
                } catch (IOException ignored) {
                }
            }
        }
        return "";
    }

    /**
     * The title needs a Prepare: no valid pack, or one PackFreshness (XBA-127) does not call current: built from
     * another default.xbe (« disc changed »), or by an older toolkit than this APK's (« toolkit changed »; Home
     * still offers Play there, but the check measures this APK's toolkit, so it prepares again). The toolkit is
     * judged by its content digest (pack.json toolkit_sha256 against Toolchain.toolkitDigest), as Home does: an
     * update that changed only Java or the shell keeps every pack current.
     */
    static boolean needsPrepare(File filesDir, String tid, String xbeSha256, String appToolkitDigest) {
        if (Pack.check(filesDir, tid) != Pack.Check.VALID)
            return true;
        return PackFreshness.of(filesDir, tid, xbeSha256, appToolkitDigest) != PackFreshness.Verdict.CURRENT;
    }

    /**
     * The check's titles, from the library's entries (sorted by title): every real title ID whose game can be
     * prepared (its files, or the scan's cached default.xbe). onlyTid: that title alone (« Check this game »).
     */
    static List<Title> plan(List<LibraryScreen.Entry> entries, File filesDir, String onlyTid) {
        List<Title> out = new ArrayList<>();
        for (LibraryScreen.Entry e : entries) {
            if (!e.realTid() || (onlyTid != null && !onlyTid.equals(e.tid)))
                continue;
            Title t = new Title();
            t.tid = e.tid;
            t.title = e.title();
            t.gameDir = gameDir(e);
            t.xbeSha256 = e.record != null && e.record.hasData() ? e.record.xbeSha256
                          : e.active != null ? e.active.xbeSha256 : "";
            if (t.gameDir.isEmpty() && e.active != null) {
                File x = e.active.xbeFile(filesDir);
                if (x != null && x.isFile())
                    t.source = x.getParent();
            } else {
                t.source = t.gameDir;
            }
            if (!t.source.isEmpty())
                out.add(t);
        }
        return out;
    }

    /**
     * What the game runs from: its folder, or a disc image the library scan found and the app can read in place by its
     * path (XBA-126: the runtime serves D:\ from it, Prepare reads it without extracting, ShellActivity hands a path
     * that is not a companion folder to the runtime as is); "" when its files are not here. An image reached only
     * through a document (content:) is not: the check cannot open it without the launcher's import.
     */
    static String gameDir(LibraryScreen.Entry e) {
        if (e.facts == null || !e.facts.hasData) {
            LibraryIndex.Item i = e.active;
            File img = i != null && LibraryIndex.KIND_ISO.equals(i.kind) && i.ok() ? LibraryIndex.locFile(i.loc) : null;
            return img != null && img.isFile() && img.canRead() ? img.getAbsolutePath() : "";
        }
        if (e.record != null && e.record.hasData())
            return e.record.dataDir;
        File f = LibraryScreen.inPlaceDir(e.active);
        return f == null ? "" : f.getAbsolutePath();
    }

    /** A new check over titles, running. */
    static State start(List<Title> titles, long now) {
        State s = new State();
        s.startedAt = s.updatedAt = now;
        s.titles.addAll(titles);
        return s;
    }

    /** « 2 h 40 min »: a rough length for a check (a Prepare ~ prepareMin minutes, a boot SECONDS + 15 s). */
    static long estimateMinutes(int toPrepare, int toBoot, int prepareMin) {
        return toPrepare * (long)prepareMin + (toBoot * (SECONDS + 15L) + 59) / 60;
    }
}
