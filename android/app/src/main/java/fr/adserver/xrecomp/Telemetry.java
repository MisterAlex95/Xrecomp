package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;

/**
 * The one consent gate for everything that leaves without a tap (XBA-338): automatic crash and Prepare reports, game
 * sessions (release health), Prepare timings. « Send reports automatically » is off by default; while it is off,
 * nothing here sends and there is no install id. A report the player sends by tapping Send (« Send report »,
 * « Report a problem ») does not need it.
 *
 * The state lives in small files under files/telemetry/ rather than SharedPreferences, so the launcher, the game and
 * the Prepare processes all see the same answer at once:
 * - auto: present while the option is on; holds the random install id (a UUID made when it is turned on, a new one
 *   on « Reset », gone when it is turned off). Nothing else identifies a device.
 * - killed: the app version the server refused (HTTP 403, or a « disabled » answer): automatic sending stops until
 *   the next version.
 * - backoff: epoch milliseconds before which automatic sending waits (HTTP 429, Retry-After or an hour).
 */
final class Telemetry {
    static final long BACKOFF_DEFAULT_MS = 3600_000L;

    private Telemetry() {}

    static File dir(Context c) {
        return new File(c.getFilesDir(), "telemetry");
    }

    private static File f(Context c, String name) {
        return new File(dir(c), name);
    }

    /** « Send reports automatically ». */
    static boolean auto(Context c) {
        return f(c, "auto").isFile();
    }

    /** Turns the option on (with a fresh install id) or off (the id is deleted). */
    static void setAuto(Context c, boolean on) {
        if (on) {
            if (!auto(c))
                write(f(c, "auto"), newId());
        } else {
            f(c, "auto").delete();
        }
    }

    /** The random install id, or null while the option is off. */
    static String installId(Context c) {
        String id = read(f(c, "auto"));
        return id == null || id.isEmpty() ? null : id;
    }

    /** A new install id (Settings › « Reset »); nothing when the option is off. */
    static void resetInstallId(Context c) {
        if (auto(c))
            write(f(c, "auto"), newId());
    }

    /** May an automatic item leave now: the option on, the server not refusing this version, no backoff running. */
    static boolean allowed(Context c) {
        if (!auto(c) || CrashReport.dsn(c) == null)
            return false;
        String killed = read(f(c, "killed"));
        if (killed != null && killed.equals(c.getString(R.string.xba_version)))
            return false;
        String until = read(f(c, "backoff"));
        try {
            return until == null || Long.parseLong(until) <= System.currentTimeMillis();
        } catch (NumberFormatException e) {
            return true;
        }
    }

    /**
     * What the server answered to any report (automatic or not): 403 stops automatic sending until the next app
     * version, 429 for retryAfterMs (or an hour). Other codes change nothing.
     */
    static void answered(Context c, int code, long retryAfterMs) {
        if (code == 403)
            write(f(c, "killed"), c.getString(R.string.xba_version));
        else if (code == 429)
            write(f(c, "backoff"), String.valueOf(System.currentTimeMillis()
                                                  + (retryAfterMs > 0 ? retryAfterMs : BACKOFF_DEFAULT_MS)));
    }

    private static String newId() {
        return UUID.randomUUID().toString();
    }

    private static String read(File f) {
        try {
            return f.isFile() ? new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim() : null;
        } catch (IOException e) {
            return null;
        }
    }

    private static void write(File f, String s) {
        File d = f.getParentFile();
        if (d != null && !(d.mkdirs() || d.isDirectory()))
            return;
        File tmp = new File(f.getPath() + ".tmp");
        try {
            Files.write(tmp.toPath(), s.getBytes(StandardCharsets.UTF_8));
            if (!tmp.renameTo(f))
                tmp.delete();
        } catch (IOException e) {
            tmp.delete();
        }
    }
}
