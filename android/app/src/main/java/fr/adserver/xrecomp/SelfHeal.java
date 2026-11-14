package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The app's self-healing loop (XBA-74, XBA-129; docs/android/prepare.md « Self-healing »).
 *
 * <p>A run writes what the static analysis missed to {@code <work>/discovered.jsonl} (docs/technical/discovery.md);
 * the next Prepare seeds the analysis with it. When a run of a title ends, the launcher looks at what the file gained
 * since the last Prepare began:
 *
 * <ul>
 *   <li>a crash with code addresses (icall, thread, thread_ctx) → {@link Offer#PREPARE_AGAIN}: a Prepare at once,
 *       then the game starts again;</li>
 *   <li>a run that ended without a crash, with code addresses (a skipped indirect call does not crash: the title goes
 *       on, maybe wrong) → {@link Offer#IMPROVE}: a Prepare in the background, Play stays;</li>
 *   <li>a crash with what a Prepare cannot fix (a guest exception nothing handled, a stack switch the runtime could
 *       not carry, an untranslated instruction), or with nothing new → {@link Offer#NEEDS_FIX}, named.</li>
 * </ul>
 *
 * <p>With {@link #auto} on (the default) the launcher starts those rounds itself, with a note, never a dialog; off,
 * it offers them as buttons. There is no cap on rounds as such: the cap is on rounds without progress, a round whose
 * Prepare accepted no new seed ({@link #MAX_IDLE_ROUNDS} of them stop the loop). On the device a crash is known only
 * by the system's words for it, so « the same blocker twice » is a crash that brought nothing new (the discovery file
 * never writes an address twice: a repeat of the same miss adds nothing). {@link #MAX_ROUNDS} is a safety net.
 *
 * <p>The state is per title, in {@code <work>/selfheal.json}: the file's length when the last Prepare began, the
 * rounds done in a row, the open round, when the game was last launched, the last exit handled, and the standing
 * offer. The PrepareService process writes {@code offset} and ends a background round, the launcher the rest (never
 * at the same time: a Prepare and a run of the same title do not overlap, and a round ends once).
 */
final class SelfHeal {
    static final String STATE = "selfheal.json";
    static final String DISCOVERY = "discovered.jsonl";
    /** tools.prepare's seeds taken from discovered.jsonl (tools/prepare/discovered.py): progress is it growing. */
    static final String SEEDS = "discovered-seeds.json";
    /**
     * Rounds without progress in a row after which new finds no longer start a round by themselves. A crash round
     * without progress stops at once (its pack is the same: no relaunch); the second one lets a later run's fresh
     * finds have their chance (tools.compat heal, with no player between rounds, stops at the first: --max-idle 1).
     */
    static final int MAX_IDLE_ROUNDS = 2;
    /** A safety net, not the cap: rounds in a row, each with progress, before the title is reported instead. */
    static final int MAX_ROUNDS = 12;
    /** What is read of discovered.jsonl past the offset: a run finds tens of entries, not thousands. */
    static final int MAX_READ = 1 << 20;
    static final String PREF_AUTO = "self_heal_auto";

    enum Offer { NONE, PREPARE_AGAIN, IMPROVE, NEEDS_FIX }

    /** What discovered.jsonl gained past an offset. */
    static final class Finds {
        /** Code addresses the next Prepare can seed (icall, thread, thread_ctx). */
        int seeds;
        /** Untranslated instructions reached (the lifter needs a fix: a Prepare does not help). */
        int unimpl;
        /** The first untranslated instruction, « bsf eax, ecx at 0x00012345 », or "". */
        String firstUnimpl = "";
        /** Guest stack switches the runtime could not carry (XBA-101, docs/technical/stack-switching.md). */
        int stackSwitch;
        /** The first of them, « at 0x00012345 to 0x00023456: <why> », or "". */
        String firstStackSwitch = "";
        /** Guest exceptions nothing handled (XBA-129: a divide error that stops the title, an unhandled raise). */
        int exception;
        /** The first of them, « 0xC0000094 at 0x00012345: divide by zero », or "". */
        String firstException = "";
        /** Stub kernel routes called (XBA-115): named when nothing else is to blame. */
        int stub;
        /** The first of them, « ordinal 136 (KeRemoveQueue) », or "". */
        String firstStub = "";
        /** Each stub route once, in the order first called (tools/compat/blockers.py stub_names). */
        final java.util.List<String> stubNames = new java.util.ArrayList<>();
        /** Code written or run at run time (XBA-118): the lifted C cannot run it, a Prepare does not help. */
        int smc;
        /** Their sites, « code_write 0x00012345 (.text) », in order (blockers.py smc_site). */
        final java.util.List<String> smcSites = new java.util.ArrayList<>();
    }

    /** How the game's process last ended (ApplicationExitInfo, or a test's). */
    static final class Exit {
        final long time;
        final boolean crashed;
        /** The system's words for it (signal, exception), for the report; may be "". */
        final String why;

        Exit(long time, boolean crashed, String why) {
            this.time = time;
            this.crashed = crashed;
            this.why = why == null ? "" : why;
        }
    }

    static final class State {
        /** discovered.jsonl's length when the last Prepare began: what lies past it is new to the analysis. */
        long offset;
        /** Rounds in a row; back to 0 when a run ends without a crash and without new code. */
        int rounds;
        /** Rounds without progress in a row (a round whose Prepare accepted no new seed). */
        int idle;
        /** When the game was last launched (an exit before it is another run's). */
        long launchedAt;
        /** The time of the last exit looked at. */
        long handled;
        Offer offer = Offer.NONE;
        /** NEEDS_FIX: what blocks; else "". */
        String blocker = "";
        /** PREPARE_AGAIN / IMPROVE: the code addresses found (for the line on Home). */
        int seeds;
        /** A round's Prepare is running: launch the game when it is done (not for a background round). */
        boolean relaunch;
        /** A round is open: its Prepare started and its outcome is not counted yet. */
        boolean roundOpen;
        /** The open round runs in the background (IMPROVE): Play stays, no relaunch. */
        boolean background;
        /** discovered-seeds.json's entries when the open round began. */
        int seedsAtRound;
        /** The last round's outcome for the note: seeds it added, or -1 before any. */
        int lastAccepted = -1;

        JSONObject toJson() throws JSONException {
            return new JSONObject().put("offset", offset).put("rounds", rounds).put("idle", idle)
                    .put("launched_at", launchedAt).put("handled", handled).put("offer", offer.name())
                    .put("blocker", blocker).put("seeds", seeds).put("relaunch", relaunch)
                    .put("round_open", roundOpen).put("background", background).put("seeds_at_round", seedsAtRound)
                    .put("last_accepted", lastAccepted);
        }

        static State fromJson(JSONObject o) {
            State s = new State();
            s.offset = Math.max(0, o.optLong("offset", 0));
            s.rounds = Math.max(0, o.optInt("rounds", 0));
            s.idle = Math.max(0, o.optInt("idle", 0));
            s.launchedAt = o.optLong("launched_at", 0);
            s.handled = o.optLong("handled", 0);
            try {
                s.offer = Offer.valueOf(o.optString("offer", "NONE"));
            } catch (IllegalArgumentException e) {
                s.offer = Offer.NONE;
            }
            s.blocker = o.optString("blocker", "");
            s.seeds = Math.max(0, o.optInt("seeds", 0));
            s.relaunch = o.optBoolean("relaunch", false);
            s.roundOpen = o.optBoolean("round_open", false);
            s.background = o.optBoolean("background", false);
            s.seedsAtRound = Math.max(0, o.optInt("seeds_at_round", 0));
            s.lastAccepted = o.optInt("last_accepted", -1);
            return s;
        }
    }

    private SelfHeal() {
    }

    // ---- The setting -----------------------------------------------------------------------------------------------

    /** Settings › General › Self-healing: the launcher starts the rounds itself (on by default). */
    static boolean auto(Context c) {
        return GameStore.prefs(c).getBoolean(PREF_AUTO, true);
    }

    static boolean setAuto(Context c, boolean on) {
        return GameStore.prefs(c).edit().putBoolean(PREF_AUTO, on).commit();
    }

    // ---- Files -----------------------------------------------------------------------------------------------------

    static File stateFile(File work) {
        return new File(work, STATE);
    }

    static File discoveryFile(File work) {
        return new File(work, DISCOVERY);
    }

    /** The title's state; a fresh one when there is none or it cannot be read. */
    static State read(File work) {
        File f = stateFile(work);
        if (!f.isFile())
            return new State();
        try {
            return State.fromJson(new JSONObject(PrepareProtocol.readText(f, 1 << 16)));
        } catch (IOException | JSONException e) {
            return new State();
        }
    }

    static void write(File work, State s) throws IOException {
        if (!(work.mkdirs() || work.isDirectory()))
            throw new IOException("cannot create " + work);
        byte[] bytes;
        try {
            bytes = (s.toJson().toString(1) + "\n").getBytes(StandardCharsets.UTF_8);
        } catch (JSONException e) {
            throw new IOException(e);
        }
        File f = stateFile(work), tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(bytes);
        }
        if (!tmp.renameTo(f))
            throw new IOException("cannot rename " + tmp + " to " + f);
    }

    /** The seeds tools.prepare kept from discovered.jsonl (entries of discovered-seeds.json); 0 without the file. */
    static int seedCount(File work) {
        File f = new File(work, SEEDS);
        if (!f.isFile())
            return 0;
        try {
            return new JSONArray(PrepareProtocol.readText(f, 4 << 20)).length();
        } catch (IOException | JSONException e) {
            return 0;
        }
    }

    // ---- Events ----------------------------------------------------------------------------------------------------

    /**
     * A Prepare of the title begins (PrepareService): from now on, only what the runs add to discovered.jsonl is new.
     * The rounds and the offer stay (the launcher counts the rounds).
     */
    static void prepareStarted(File work) throws IOException {
        State s = read(work);
        s.offset = discoveryFile(work).length();
        write(work, s);
    }

    /** The game is launched (not brought back): an offer made for the previous run goes. */
    static void launched(File work, long now) throws IOException {
        State s = read(work);
        s.launchedAt = now;
        s.offer = Offer.NONE;
        s.blocker = "";
        s.seeds = 0;
        s.relaunch = false;
        write(work, s);
    }

    /** « Prepare again » after a crash (pressed, or started by the launcher): a round, then the game starts again. */
    static void prepareAgain(File work) throws IOException {
        startRound(work, false);
    }

    /**
     * One more round: its Prepare is about to start. background: after a run that ended well (IMPROVE), Play stays
     * and nothing starts by itself afterwards.
     */
    static void startRound(File work, boolean background) throws IOException {
        State s = read(work);
        s.rounds++;
        s.offer = Offer.NONE;
        s.blocker = "";
        s.relaunch = !background;
        s.background = background;
        s.roundOpen = true;
        s.seedsAtRound = seedCount(work);
        write(work, s);
    }

    /**
     * The open round's Prepare ended (done: a valid pack). Counts the round's progress, once: seeds it added to
     * discovered-seeds.json. None, after a crash round: the loop stops, the title needs a fix (the addresses the run
     * found are not code, or the Prepare could not use them). Not done (failed, cancelled: the player's Stop): the
     * loop stops quietly. Returns whether the game should start now.
     */
    static boolean roundEnded(File work, boolean done) throws IOException {
        State s = read(work);
        if (!s.roundOpen)
            return false;
        s.roundOpen = false;
        int accepted = seedCount(work) - s.seedsAtRound;
        s.lastAccepted = Math.max(0, accepted);
        if (!done) {
            s.relaunch = false;
            s.offer = Offer.NONE;
        } else if (accepted <= 0) {
            s.idle++;
            if (s.relaunch) {   // the same pack would crash the same way: no relaunch
                s.offer = Offer.NEEDS_FIX;
                s.blocker = "crash, and the Prepare could not use what the run found (" + s.seeds
                            + " address" + (s.seeds == 1 ? "" : "es") + ", none of them code)";
            }
            s.relaunch = false;
        } else {
            s.idle = 0;
        }
        s.background = false;
        write(work, s);
        return s.relaunch;
    }

    /** What discovered.jsonl gained past {@code from} (complete lines only; a torn last line waits). */
    static Finds scan(File jsonl, long from) {
        Finds f = new Finds();
        long len = jsonl.length();
        if (from < 0 || from > len)   // the file was cut or replaced: all of it is new
            from = 0;
        if (len <= from)
            return f;
        byte[] buf = new byte[(int)Math.min(len - from, MAX_READ)];
        try (RandomAccessFile r = new RandomAccessFile(jsonl, "r")) {
            r.seek(from);
            r.readFully(buf);
        } catch (IOException e) {
            return f;
        }
        String text = new String(buf, StandardCharsets.UTF_8);
        int end = text.lastIndexOf('\n');
        if (end < 0)
            return f;
        for (String line : text.substring(0, end).split("\n")) {
            line = line.trim();
            if (line.isEmpty())
                continue;
            JSONObject o;
            try {
                o = new JSONObject(line);
            } catch (JSONException e) {
                continue;
            }
            switch (o.optString("t", "")) {
            case "icall":
            case "thread":
            case "thread_ctx":
                // Written after the run went wrong ("tainted": a v2 runtime marks them, profile-free WP1): the
                // Prepare's gate holds them back (tools/seed_from_log), so they are no progress it can make.
                if (!o.optBoolean("tainted", false))
                    f.seeds++;
                break;
            case "unimpl":
                if (f.unimpl++ == 0)
                    f.firstUnimpl = o.optString("insn", "?") + " at " + o.optString("va", "?");
                break;
            case "stack_switch":
                if (f.stackSwitch++ == 0)
                    f.firstStackSwitch = "at " + o.optString("va", "?") + " to " + o.optString("to", "?") + ": "
                            + o.optString("why", "?");
                break;
            case "exception":
                if (f.exception++ == 0)
                    f.firstException = o.optString("code", "?") + " at " + o.optString("va", "?") + ": "
                            + o.optString("why", "?");
                break;
            case "stub": {
                String name = "ordinal " + o.optInt("ord", -1)
                              + (o.optString("name", "").isEmpty() ? "" : " (" + o.optString("name", "") + ")");
                if (f.stub++ == 0)
                    f.firstStub = name;
                if (!f.stubNames.contains(name))
                    f.stubNames.add(name);
                break;
            }
            case "smc":
                f.smc++;
                if ("code_write".equals(o.optString("kind", "")))
                    f.smcSites.add("code_write " + (o.has("at") ? o.optString("at") : o.optString("va", "null"))
                                   + " (" + o.optString("section", "?") + ")");
                else
                    f.smcSites.add(o.optString("kind", "smc") + " " + o.optString("va", "null") + " ("
                                   + o.optString("where", "?") + ", from " + o.optString("ret", "?") + ")");
                break;
            default:   // run, d3d, wait_spin, launch: the runtime acts on them, the analysis does not
                break;
            }
        }
        return f;
    }

    /**
     * What a Prepare cannot fix and stops a title, in words, or null. The same words as tools/compat/blockers.py, which
     * the desktop's classify() (the matrix, the device's library check) and heal.py use: one blocker reads the same
     * everywhere (SelfHealTest and test_blockers.py pin the strings).
     */
    static String fatal(Finds f) {
        if (f.exception > 0)
            return "guest exception " + f.firstException + more(f.exception);
        if (f.stackSwitch > 0)
            return "stack switch " + f.firstStackSwitch + more(f.stackSwitch);
        if (f.unimpl > 0)
            return "untranslated instruction: " + f.firstUnimpl + more(f.unimpl);
        if (f.smc > 0)
            return smcWords(f);
        return null;
    }

    /** blockers.py smc_words: « code written or run at run time x2: site, site » (at most 3, then « +N more »). */
    static String smcWords(Finds f) {
        java.util.List<String> sites = new java.util.ArrayList<>(f.smcSites);
        if (sites.size() > 3) {
            int extra = sites.size() - 3;
            sites = new java.util.ArrayList<>(sites.subList(0, 3));
            sites.add("+" + extra + " more");
        }
        return "code written or run at run time x" + f.smc + ": " + String.join(", ", sites);
    }

    /** blockers.py stub_words: « calls stub ordinal 136 (KeRemoveQueue), ... » (at most 4, then « +N more »), or "". */
    static String stubWords(Finds f) {
        java.util.List<String> names = new java.util.ArrayList<>(f.stubNames);
        if (names.isEmpty())
            return "";
        if (names.size() > 4) {
            int extra = names.size() - 4;
            names = new java.util.ArrayList<>(names.subList(0, 4));
            names.add("+" + extra + " more");
        }
        return "calls stub " + String.join(", ", names);
    }

    private static String more(int n) {
        return n > 1 ? " (+" + (n - 1) + ")" : "";
    }

    /** The offer for a run of a title that crashed with these finds, after {@code rounds} rounds. */
    static Offer decide(Finds f, int rounds) {
        return fatal(f) == null && f.seeds > 0 && rounds < MAX_ROUNDS ? Offer.PREPARE_AGAIN : Offer.NEEDS_FIX;
    }

    /** What blocks a title that needs a fix, for the report. */
    static String blocker(Finds f, int rounds, Exit e) {
        String fatal = fatal(f);
        if (fatal != null)
            return fatal;
        String crash = "crash" + (e.why.isEmpty() ? "" : " (" + e.why + ")");
        if (f.seeds > 0)
            return crash + ", still after " + rounds + " rounds";
        return crash + ", nothing new found" + (f.stub > 0 ? "; " + stubWords(f) : "");
    }

    /**
     * The game's process ended: update the title's state, return true when it changed. An exit of an earlier run (before
     * launchedAt) or already looked at changes nothing. Without a crash: new code → IMPROVE, else the rounds end.
     */
    static boolean onExit(File work, Exit e) throws IOException {
        State s = read(work);
        if (e == null || e.time <= s.handled || e.time < s.launchedAt)
            return false;
        s.handled = e.time;
        Finds f = scan(discoveryFile(work), s.offset);
        s.seeds = f.seeds;
        if (!e.crashed) {
            s.offer = f.seeds > 0 && s.idle < MAX_IDLE_ROUNDS ? Offer.IMPROVE : Offer.NONE;
            s.blocker = "";
            if (s.offer == Offer.NONE) {
                s.rounds = 0;
                s.idle = 0;
            }
        } else if (s.idle >= MAX_IDLE_ROUNDS && fatal(f) == null) {
            s.offer = Offer.NEEDS_FIX;
            s.blocker = "crash" + (e.why.isEmpty() ? "" : " (" + e.why + ")") + ", and the last Prepare could not use "
                        + "what the runs found";
        } else {
            s.offer = decide(f, s.rounds);
            s.blocker = s.offer == Offer.NEEDS_FIX ? blocker(f, s.rounds, e) : "";
        }
        write(work, s);
        return true;
    }
}
