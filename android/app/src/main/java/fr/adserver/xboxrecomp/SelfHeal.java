package fr.adserver.xboxrecomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

import org.json.JSONException;
import org.json.JSONObject;

/**
 * The app's self-healing loop (XBA-74, docs/android/prepare.md « Prepare again »).
 *
 * <p>A run writes what the static analysis missed to {@code <work>/discovered.jsonl} (docs/technical/discovery.md);
 * the next Prepare seeds the analysis with it. When a run of a title ends on a crash, the launcher looks at what the
 * file gained since the last Prepare began: code addresses (icall, thread, thread_ctx) → « Prepare again », which
 * prepares and relaunches; nothing of the kind, or {@link #MAX_ROUNDS} rounds already done → the title needs a fix,
 * with the blocker (an untranslated instruction when one was reached, else the crash).
 *
 * <p>The state is per title, in {@code <work>/selfheal.json}: the file's length when the last Prepare began, the
 * rounds done in a row, when the game was last launched, the last exit handled, and the standing offer. The
 * PrepareService process writes {@code offset}, the launcher the rest (never at the same time: a Prepare and a run of
 * the same title do not overlap).
 */
final class SelfHeal {
    static final String STATE = "selfheal.json";
    static final String DISCOVERY = "discovered.jsonl";
    /** Prepare-again rounds in a row (crash, new finds, prepare, relaunch) before the title is reported instead. */
    static final int MAX_ROUNDS = 3;
    /** What is read of discovered.jsonl past the offset: a run finds tens of entries, not thousands. */
    static final int MAX_READ = 1 << 20;

    enum Offer { NONE, PREPARE_AGAIN, NEEDS_FIX }

    /** What discovered.jsonl gained past an offset. */
    static final class Finds {
        /** Code addresses the next Prepare can seed (icall, thread, thread_ctx). */
        int seeds;
        /** Untranslated instructions reached (the lifter needs a fix: a Prepare does not help). */
        int unimpl;
        /** The first untranslated instruction, « bsf eax, ecx at 0x00012345 », or "". */
        String firstUnimpl = "";
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
        /** Prepare-again rounds in a row; back to 0 when a run ends without a crash. */
        int rounds;
        /** When the game was last launched (an exit before it is another run's). */
        long launchedAt;
        /** The time of the last exit looked at. */
        long handled;
        Offer offer = Offer.NONE;
        /** NEEDS_FIX: what blocks; PREPARE_AGAIN: "". */
        String blocker = "";
        /** PREPARE_AGAIN: the seeds found (for the line on Home). */
        int seeds;
        /** A Prepare again is running: launch the game when it is done. */
        boolean relaunch;

        JSONObject toJson() throws JSONException {
            return new JSONObject().put("offset", offset).put("rounds", rounds).put("launched_at", launchedAt)
                    .put("handled", handled).put("offer", offer.name()).put("blocker", blocker).put("seeds", seeds)
                    .put("relaunch", relaunch);
        }

        static State fromJson(JSONObject o) {
            State s = new State();
            s.offset = Math.max(0, o.optLong("offset", 0));
            s.rounds = Math.max(0, o.optInt("rounds", 0));
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
            return s;
        }
    }

    private SelfHeal() {
    }

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

    /** « Prepare again » pressed: one more round, launch the game when the Prepare is done. */
    static void prepareAgain(File work) throws IOException {
        State s = read(work);
        s.rounds++;
        s.offer = Offer.NONE;
        s.blocker = "";
        s.seeds = 0;
        s.relaunch = true;
        write(work, s);
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
                f.seeds++;
                break;
            case "unimpl":
                if (f.unimpl++ == 0)
                    f.firstUnimpl = o.optString("insn", "?") + " at " + o.optString("va", "?");
                break;
            default:   // run, d3d, wait_spin, launch: the runtime acts on them, the analysis does not
                break;
            }
        }
        return f;
    }

    /** The offer for a run of a title that crashed with these finds, after {@code rounds} rounds. */
    static Offer decide(Finds f, int rounds) {
        return f.seeds > 0 && rounds < MAX_ROUNDS ? Offer.PREPARE_AGAIN : Offer.NEEDS_FIX;
    }

    /** What blocks a title that needs a fix, for the report. */
    static String blocker(Finds f, int rounds, Exit e) {
        if (f.unimpl > 0)
            return "untranslated instruction: " + f.firstUnimpl + (f.unimpl > 1 ? " (+" + (f.unimpl - 1) + ")" : "");
        String crash = "crash" + (e.why.isEmpty() ? "" : " (" + e.why + ")");
        if (f.seeds > 0)
            return crash + ", still after " + rounds + " Prepare again round" + (rounds == 1 ? "" : "s");
        return crash + ", nothing new found";
    }

    /**
     * The game's process ended: update the title's state, return true when it changed. An exit of an earlier run (before
     * launchedAt) or already looked at changes nothing; one without a crash ends the rounds.
     */
    static boolean onExit(File work, Exit e) throws IOException {
        State s = read(work);
        if (e == null || e.time <= s.handled || e.time < s.launchedAt)
            return false;
        s.handled = e.time;
        if (!e.crashed) {
            s.rounds = 0;
            s.offer = Offer.NONE;
            s.blocker = "";
            s.seeds = 0;
        } else {
            Finds f = scan(discoveryFile(work), s.offset);
            s.offer = decide(f, s.rounds);
            s.seeds = f.seeds;
            s.blocker = s.offer == Offer.NEEDS_FIX ? blocker(f, s.rounds, e) : "";
        }
        write(work, s);
        return true;
    }
}
