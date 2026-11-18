package fr.adserver.xrecomp;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * The player's Xbox 360 key file (XBA-282; docs/android/prepare.md « Xbox 360 packs »).
 *
 * <p>A retail title's XEX is encrypted; tools.prepare opens it with the keys built into the APK, when the build was given
 * them (the secrets XBA_X360_KEY and XBA_X360_DEVKIT_KEY, XBA-300: never in the repository), and with the key the player
 * picked in the app (the game's Home, « Key file »). The picked file is checked (16 raw bytes, or 32 hex digits on its first
 * line: tools/xex/xex.py load_keys) and copied to {@code <no backup files>/keys/x360.key}: private to the app, never
 * in a backup, a save archive or the cloud, never logged. The Prepare process gets its path in {@code XEX_KEY_FILE}.
 */
final class X360Key {
    static final String DIR = "keys";
    static final String NAME = "x360.key";
    /** The environment variable tools/xex/xex.py load_keys reads. */
    static final String ENV = "XEX_KEY_FILE";
    /** A key file is small: more than this is not one. */
    static final int MAX = 4096;
    /** The build's keys in the APK (android/app/build.gradle), retail first; Prepare's process gets copies of them. */
    static final String[] BUILTIN = {"builtin.key", "builtin-devkit.key"};
    static final String BUILTIN_DIR = "x360/";

    private X360Key() {}

    static File file(File noBackupDir) {
        return new File(new File(noBackupDir, DIR), NAME);
    }

    static File file(Context c) {
        return file(c.getNoBackupFilesDir());
    }

    static boolean present(Context c) {
        return file(c).isFile();
    }

    /** This APK was built with the retail key: retail games need no key file. */
    static boolean builtin(Context c) {
        try (InputStream in = c.getAssets().open(BUILTIN_DIR + BUILTIN[0])) {
            return in.read() >= 0;
        } catch (IOException e) {
            return false;
        }
    }

    /** The built-in keys as private files for Prepare's process, retail first; a key this APK lacks is null. */
    static File[] installBuiltin(Context c) {
        File[] out = new File[BUILTIN.length];
        for (int k = 0; k < BUILTIN.length; k++)
            out[k] = installBuiltin(c, BUILTIN[k]);
        return out;
    }

    private static File installBuiltin(Context c, String name) {
        File f = new File(new File(c.getNoBackupFilesDir(), DIR), name);
        try (InputStream in = c.getAssets().open(BUILTIN_DIR + name)) {
            //noinspection ResultOfMethodCallIgnored
            f.getParentFile().mkdirs();
            try (FileOutputStream o = new FileOutputStream(f)) {
                byte[] buf = new byte[64];
                for (int n; (n = in.read(buf)) > 0; )
                    o.write(buf, 0, n);
            }
            return f;
        } catch (IOException e) {
            return null;
        }
    }

    /** XEX_KEY_FILE's value: the player's key first, then the built-in ones in order; null when no file exists. */
    static String keyFiles(File player, File[] builtin) {
        StringBuilder s = new StringBuilder();
        if (player != null && player.isFile())
            s.append(player.getAbsolutePath());
        for (File f : builtin)
            if (f != null && f.isFile())
                s.append(s.length() > 0 ? File.pathSeparator : "").append(f.getAbsolutePath());
        return s.length() > 0 ? s.toString() : null;
    }

    /** The 16 key bytes of a key file's content, or null when it is not one (load_keys' two forms). */
    static byte[] parse(byte[] raw, int n) {
        if (n == 16) {
            byte[] k = new byte[16];
            System.arraycopy(raw, 0, k, 0, 16);
            return k;
        }
        String text = new String(raw, 0, n, StandardCharsets.US_ASCII).trim();
        int sp = 0;
        while (sp < text.length() && !Character.isWhitespace(text.charAt(sp)))
            sp++;
        text = text.substring(0, sp);
        if (text.length() != 32)
            return null;
        byte[] k = new byte[16];
        for (int i = 0; i < 16; i++) {
            int hi = Character.digit(text.charAt(2 * i), 16), lo = Character.digit(text.charAt(2 * i + 1), 16);
            if (hi < 0 || lo < 0)
                return null;
            k[i] = (byte)(hi << 4 | lo);
        }
        return k;
    }

    /** Checks and stores a picked key file. IOException (with why) when it is not a key file. */
    static void install(File noBackupDir, InputStream in) throws IOException {
        byte[] buf = new byte[MAX + 1];
        int n = 0, r;
        while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0)
            n += r;
        if (n > MAX)
            throw new IOException("too large for a key file");
        byte[] k = parse(buf, n);
        java.util.Arrays.fill(buf, (byte)0);
        if (k == null)
            throw new IOException("not a key file (16 bytes, or 32 hex digits)");
        File f = file(noBackupDir), tmp = new File(f.getParentFile(), NAME + ".tmp");
        //noinspection ResultOfMethodCallIgnored
        f.getParentFile().mkdirs();
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write(k);
            o.getFD().sync();
        } finally {
            java.util.Arrays.fill(k, (byte)0);
        }
        if (!tmp.renameTo(f))
            throw new IOException("cannot write the key file");
    }

    static boolean forget(Context c) {
        return file(c).delete();
    }
}
