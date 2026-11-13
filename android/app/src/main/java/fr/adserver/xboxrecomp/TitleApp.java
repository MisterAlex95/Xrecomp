package fr.adserver.xboxrecomp;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;
import android.util.TypedValue;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.math.BigInteger;
import java.security.GeneralSecurityException;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.security.spec.ECGenParameterSpec;
import java.util.Calendar;
import java.util.function.BooleanSupplier;

import javax.security.auth.x500.X500Principal;

/**
 * The Android side of « Create an app » (Manage; docs/android/app-shell.md « A game's own app »): the template (this
 * app's own APK), the signing key, the icon, the PackageInstaller session, and what a generated app reads of itself
 * (its one-game meta-data). TitleApk makes the APK; TitleAppReceiver hears the installer.
 *
 * The key: EC P-256 in the AndroidKeyStore (alias KEY_ALIAS), made at the first « Create an app » and never exported.
 * apksig signs through java.security.Signature, which the keystore's provider serves for its own keys, and reads the
 * public key from the keystore's self-signed certificate. Every generated app, and every regeneration, is signed with
 * it, so a regenerated app installs over the previous one. It lives as long as this app's install: uninstalling
 * XRecomp (not updating it) loses it, and a generated app made before then must be uninstalled to be made again.
 */
final class TitleApp {
    private TitleApp() {}

    static final String KEY_ALIAS = "xba_title_apps";
    static final String WORK_DIR = "title-app";
    /** Free space needed beyond the two copies (the unsigned one and the signed one) during a build. */
    static final long SPACE_MARGIN = 64L << 20;
    static final String PREFS = "title_app";

    // ---- What a generated app knows about itself -----------------------------------------------------------------

    private static Bundle meta;
    private static boolean metaRead;

    /** This app's application meta-data (cached; empty when there is none). */
    static synchronized Bundle meta(Context c) {
        if (!metaRead) {
            metaRead = true;
            try {
                ApplicationInfo ai = c.getPackageManager().getApplicationInfo(c.getPackageName(),
                                                                              PackageManager.GET_META_DATA);
                meta = ai.metaData;
            } catch (PackageManager.NameNotFoundException | RuntimeException e) {
                meta = null;
            }
        }
        return meta == null ? Bundle.EMPTY : meta;
    }

    /** Tests: forget the cached meta-data. */
    static synchronized void resetMeta() {
        metaRead = false;
        meta = null;
    }

    /** The title ID this app was generated for (a game's own app), or null for XRecomp itself. */
    static String ownTitleId(Context c) {
        String t = meta(c).getString(TitleApk.META_TITLE_ID);
        return t == null || t.isEmpty() ? null : t;
    }

    /** The default.xbe SHA-256 a generated app is locked to, or "" (none, or XRecomp itself). */
    static String ownXbeSha256(Context c) {
        String s = meta(c).getString(TitleApk.META_XBE_SHA256);
        return s == null ? "" : s;
    }

    /** The package of the app this one was generated from (XRecomp), or null for XRecomp itself. */
    static String ownParent(Context c) {
        String p = meta(c).getString(TitleApk.META_PARENT);
        return p == null || p.isEmpty() || p.equals(c.getPackageName()) ? null : p;
    }

    // ---- The template --------------------------------------------------------------------------------------------

    /** This app's installed APK, or null when it is split (a copy of base.apk alone would not run). */
    static File template(Context c) {
        ApplicationInfo ai = c.getApplicationInfo();
        if (ai.splitSourceDirs != null && ai.splitSourceDirs.length > 0)
            return null;
        return ai.sourceDir == null ? null : new File(ai.sourceDir);
    }

    /**
     * The zip entry of the icon slot's layer in this APK (« res/drawable-nodpi-v4/xba_title_icon_art.png », or a
     * shortened path in an optimised release build): what Resources says the file is.
     */
    static String iconEntry(Context c) {
        TypedValue v = new TypedValue();
        c.getResources().getValue(R.drawable.xba_title_icon_art, v, true);
        String s = v.string == null ? "" : v.string.toString();
        return entryName(s);
    }

    /** A resource file path as a zip entry: « res/… » as it is, the part from « /res/ » of an absolute path. */
    static String entryName(String path) {
        if (path.startsWith("res/"))
            return path;
        int i = path.lastIndexOf("/res/");
        return i >= 0 ? path.substring(i + 1) : path;
    }

    /** The generated app's icon layer: TitleShortcut's composition (title image on its border colour, monogram). */
    static byte[] iconPng(Bitmap art, String tid, String title) {
        Bitmap b = TitleShortcut.icon(art, tid, title);
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        b.compress(Bitmap.CompressFormat.PNG, 100, o);
        return o.toByteArray();
    }

    /** The space a build needs in the work directory: two copies of the template and a margin. */
    static long spaceNeeded(File template) {
        return 2 * template.length() + SPACE_MARGIN;
    }

    static File workDir(Context c) {
        return new File(c.getFilesDir(), WORK_DIR);
    }

    /** Removes what a build left (a scratch copy, a signed APK already handed to the installer). */
    static void clean(Context c) {
        File[] l = workDir(c).listFiles();
        if (l != null) {
            for (File f : l)
                //noinspection ResultOfMethodCallIgnored
                f.delete();
        }
    }

    // ---- The key ---------------------------------------------------------------------------------------------

    /** The signing key and its certificate (made once, in the AndroidKeyStore). */
    static final class Key {
        final PrivateKey key;
        final X509Certificate cert;

        Key(PrivateKey key, X509Certificate cert) {
            this.key = key;
            this.cert = cert;
        }
    }

    /** Tests: the key to use instead of the AndroidKeyStore's (which Robolectric does not have). */
    static Key testKey;

    static Key key() throws IOException {
        if (testKey != null)
            return testKey;
        try {
            KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
            ks.load(null);
            if (!ks.containsAlias(KEY_ALIAS)) {
                Calendar from = Calendar.getInstance();
                Calendar to = Calendar.getInstance();
                to.add(Calendar.YEAR, 30);
                KeyPairGenerator g = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
                g.initialize(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_SIGN
                                                                        | KeyProperties.PURPOSE_VERIFY)
                             .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1"))
                             .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                             .setCertificateSubject(new X500Principal("CN=XRecomp game apps"))
                             .setCertificateSerialNumber(BigInteger.ONE)
                             .setCertificateNotBefore(from.getTime())
                             .setCertificateNotAfter(to.getTime())
                             .build());
                g.generateKeyPair();
                Log.i(LauncherActivity.TAG, "launcher: game apps' signing key created");
            }
            PrivateKey k = (PrivateKey)ks.getKey(KEY_ALIAS, null);
            X509Certificate cert = (X509Certificate)ks.getCertificate(KEY_ALIAS);
            if (k == null || cert == null)
                throw new IOException("no key " + KEY_ALIAS);
            return new Key(k, cert);
        } catch (GeneralSecurityException | RuntimeException e) {
            throw new IOException("signing key: " + e.getMessage(), e);
        }
    }

    /**
     * The certificate of the game apps' key when it exists, without making it (PackHandover: an XRecomp that never
     * made a game app has no key and serves no pack). Null when there is none or the keystore cannot be read.
     */
    static X509Certificate existingCert() {
        if (testKey != null)
            return testKey.cert;
        try {
            KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
            ks.load(null);
            if (!ks.containsAlias(KEY_ALIAS))
                return null;
            return (X509Certificate)ks.getCertificate(KEY_ALIAS);
        } catch (GeneralSecurityException | IOException | RuntimeException e) {
            Log.w(LauncherActivity.TAG, "launcher: game apps' key: " + e);
            return null;
        }
    }

    // ---- Building ------------------------------------------------------------------------------------------------

    /** What a build reports: copying (done/total bytes), then signing (no measure). */
    interface Progress extends ApkZip.Progress {
        void signing();
    }

    /** Builds tid's app from this APK into the work directory; returns the signed APK. */
    static File build(Context c, TitleApk.Spec s, Key key, Progress p, BooleanSupplier cancelled) throws IOException {
        File template = template(c);
        if (template == null)
            throw new IOException("split install");
        File dir = workDir(c);
        if (!(dir.mkdirs() || dir.isDirectory()))
            throw new IOException("cannot create " + dir);
        clean(c);
        File unsigned = new File(dir, "unsigned.apk"), out = new File(dir, s.packageName() + ".apk");
        try {
            TitleApk.build(template, unsigned, out, s, key.key, key.cert, p, p::signing, cancelled);
        } catch (IOException | RuntimeException e) {
            //noinspection ResultOfMethodCallIgnored
            out.delete();
            throw e;
        }
        return out;
    }

    // ---- Installing ----------------------------------------------------------------------------------------------

    /** This app may ask Android to install apps (« Install unknown apps » allowed for it). */
    static boolean canInstall(Context c) {
        try {
            return c.getPackageManager().canRequestPackageInstalls();
        } catch (RuntimeException e) {
            return false;
        }
    }

    /**
     * Hands apk to Android's installer: a PackageInstaller session written from the file (progress), committed with
     * TitleAppReceiver as the status receiver (which shows Android's confirmation, then reports the outcome).
     */
    static void install(Context c, File apk, String pkg, String label, ApkZip.Progress p, BooleanSupplier cancelled)
            throws IOException {
        PackageInstaller pi = c.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(pkg);
        params.setSize(apk.length());
        params.setAppLabel(label);
        int id = pi.createSession(params);
        boolean committed = false;
        try (PackageInstaller.Session session = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[1 << 16];
                long done = 0, total = apk.length();
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (cancelled != null && cancelled.getAsBoolean())
                        throw new InterruptedIOException("cancelled");
                    out.write(buf, 0, n);
                    done += n;
                    if (p != null)
                        p.update(done, total);
                }
                session.fsync(out);
            }
            Intent i = new Intent(c, TitleAppReceiver.class).setAction(TitleAppReceiver.ACTION)
                    .putExtra(TitleAppReceiver.EXTRA_LABEL, label);
            PendingIntent pend = PendingIntent.getBroadcast(c, id, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pend.getIntentSender());
            committed = true;
            Log.i(LauncherActivity.TAG, "launcher: game app " + pkg + " handed to the installer (session " + id + ")");
        } finally {
            if (!committed) {
                try {
                    pi.abandonSession(id);
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    // ---- The installer's outcome --------------------------------------------------------------------------------

    /** The last outcome, kept until the launcher shows it: status (PackageInstaller.STATUS_*), message, label. */
    static void saveOutcome(Context c, int status, String message, String label, String pkg) {
        prefs(c).edit().putInt("status", status).putString("message", message == null ? "" : message)
                .putString("label", label == null ? "" : label).putString("package", pkg == null ? "" : pkg)
                .putBoolean("pending", true).apply();
    }

    /** The outcome not shown yet as {status, message, label, package}, cleared; null when there is none. */
    static String[] takeOutcome(Context c) {
        SharedPreferences p = prefs(c);
        if (!p.getBoolean("pending", false))
            return null;
        p.edit().putBoolean("pending", false).apply();
        return new String[] { String.valueOf(p.getInt("status", PackageInstaller.STATUS_FAILURE)),
                              p.getString("message", ""), p.getString("label", ""), p.getString("package", "") };
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }
}
