package fr.adserver.xrecomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

/**
 * Play time as the game process sees it (XBA-206, docs/android/app-shell.md « Play time »): ShellActivity counts the
 * time its game is on screen (onStart to onStop, Quit), in files/play_clock.properties, written at each change and
 * once a minute while it shows (a crash loses a minute at most). The launcher, closing the session, takes that time
 * instead of the wall clock from launch to return (which counted a game left in the background). Pure: the callers
 * hand it the time.
 *
 * <p>Fields: session (the launcher's session start, GameStore.sessionStartedAt, so a game process the system restarts
 * goes on with the same count), created (when this process began counting), fg_ms (closed on-screen time), fg_since
 * (when it came on screen, 0 while it is not), seen (the last write: where an on-screen stretch a crash cut ends).
 */
final class PlayClock {
    static final String FILE = "play_clock.properties";
    /** How often a game on screen writes its count. */
    static final long BEAT_MS = 60_000;

    String tid = "";
    long session, created, fgMs, fgSince, seen;

    static File file(File filesDir) {
        return new File(filesDir, FILE);
    }

    /** The clock in the file, or null when there is none or it cannot be read. */
    static PlayClock read(File f) {
        if (f == null || !f.isFile())
            return null;
        Properties p = new Properties();
        try (InputStream in = new FileInputStream(f)) {
            p.load(new InputStreamReader(in, StandardCharsets.UTF_8));
        } catch (IOException | IllegalArgumentException e) {
            return null;
        }
        PlayClock c = new PlayClock();
        c.tid = p.getProperty("tid", "");
        c.session = num(p, "session");
        c.created = num(p, "created");
        c.fgMs = Math.max(0, num(p, "fg_ms"));
        c.fgSince = num(p, "fg_since");
        c.seen = num(p, "seen");
        return c.created > 0 ? c : null;
    }

    private static long num(Properties p, String k) {
        try {
            return Long.parseLong(p.getProperty(k, "0").trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    boolean write(File f) {
        String s = "tid=" + tid + "\nsession=" + session + "\ncreated=" + created + "\nfg_ms=" + fgMs
                   + "\nfg_since=" + fgSince + "\nseen=" + seen + "\n";
        try {
            Library.writeAtomically(f, s);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    /**
     * The game process starts: the clock of the same launcher session goes on (a stretch on screen a crash cut is
     * closed at its last write), any other starts from zero.
     */
    static PlayClock begin(PlayClock was, String tid, long session, long now) {
        if (was != null && session > 0 && was.session == session) {
            was.close(was.seen);
            was.seen = now;
            return was;
        }
        PlayClock c = new PlayClock();
        c.tid = tid == null ? "" : tid;
        c.session = session;
        c.created = c.seen = now;
        return c;
    }

    /** The game comes on screen. */
    void resume(long now) {
        if (fgSince <= 0)
            fgSince = now;
        seen = now;
    }

    /** The game leaves the screen (background, quit). */
    void pause(long now) {
        close(now);
        seen = now;
    }

    /** A beat while on screen: only seen moves (total() counts up to it). */
    void beat(long now) {
        seen = Math.max(seen, now);
    }

    private void close(long end) {
        if (fgSince > 0) {
            fgMs += Math.max(0, end - fgSince);
            fgSince = 0;
        }
    }

    /** The time on screen: the closed stretches and the open one up to the last write. */
    long total() {
        return fgMs + (fgSince > 0 ? Math.max(0, seen - fgSince) : 0);
    }

    /** This clock counts the launcher session that started at sessionStart. */
    boolean counts(long sessionStart) {
        return sessionStart > 0 && (session == sessionStart || created >= sessionStart);
    }

    /**
     * The length of the launcher session started at start and closed at now: the game's own count when its clock is
     * this session's, the wall clock otherwise (a game process too old to count); at most GameStore.SESSION_MAX.
     */
    static long sessionLength(PlayClock c, long start, long now) {
        if (start <= 0)
            return 0;
        long d = c != null && c.counts(start) ? c.total() : now - start;
        return Math.max(0, Math.min(GameStore.SESSION_MAX, d));
    }
}
